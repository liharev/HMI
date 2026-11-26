#!/usr/bin/python3
"""Mock of a CAMAI device for testing CAMAI VMS without the hardware.

    sudo apt install python3-gi gir1.2-gst-rtsp-server-1.0 gstreamer1.0-plugins-ugly gstreamer1.0-libav
    python3 tools/camai-mock.py [--http 8080] [--rtsp 8555] [--channels 2] [--no-signal 1]
                                [--fail-503 0/3] [--hide 0/4]

REST (same JSON layout as the device):
    GET /api/v1/preview   catalogue of channels and their stream profiles
    GET /api/v1/status    channel state (signal / splash, preview encoders)
RTSP: rtsp://127.0.0.1:8555/<channel>/<stream>
    stream 0: 1920x1080 25 fps H.264 + AAC     ("stream", always running)
    stream 1:  640x360  25 fps H.264 + AAC     ("stream")
    stream 2:  640x360   5 fps H.264           ("preview", on demand)
    stream 3:  848x480  10 fps H.264           ("preview")
    stream 4: 1280x720  15 fps H.264           ("preview")
--fail-503 makes the listed paths answer "503 Service Unavailable";
--hide removes profiles from the catalogue (the RTSP path answers 404).
"""
import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstRtspServer", "1.0")
gi.require_version("GstRtsp", "1.0")
from gi.repository import GLib, Gst, GstRtsp, GstRtspServer  # noqa: E402

Gst.init(None)

ap = argparse.ArgumentParser()
ap.add_argument("--http", type=int, default=8080)
ap.add_argument("--rtsp", type=int, default=8555)
ap.add_argument("--host", default="127.0.0.1", help="address written into rtsp_uri")
ap.add_argument("--channels", type=int, default=2)
ap.add_argument("--no-signal", default="", help="comma separated channel ids without signal (splash)")
ap.add_argument("--fail-503", default="", help="comma separated ch/stream paths answering 503")
ap.add_argument("--hide", default="", help="comma separated ch/stream paths missing from the catalogue")
args = ap.parse_args()

NO_SIGNAL = {int(x) for x in args.no_signal.split(",") if x.strip()}
FAIL = {x.strip() for x in args.fail_503.split(",") if x.strip()}
HIDE = {x.strip() for x in args.hide.split(",") if x.strip()}

PROFILES = [
    # stream_id, id, kind, w, h, fps, kbps, audio
    (0, "stream_0", "stream", 1920, 1080, 25, 6000, True),
    (1, "stream_1", "stream", 640, 360, 25, 1000, True),
    (2, "360p", "preview", 640, 360, 5, 250, False),
    (3, "480p", "preview", 848, 480, 10, 500, False),
    (4, "720p", "preview", 1280, 720, 15, 900, False),
]
PATTERNS = ["smpte", "ball", "pinwheel", "bar", "snow"]
viewers = {}          # (channel, stream) -> clients
lock = threading.Lock()


def launch(ch, sid, w, h, fps, kbps, audio):
    pattern = "snow" if ch in NO_SIGNAL else PATTERNS[ch % len(PATTERNS)]
    label = f"CH{ch} S{sid} {w}x{h}@{fps}" + (" NO SIGNAL" if ch in NO_SIGNAL else "")
    video = (f"videotestsrc is-live=true pattern={pattern} ! video/x-raw,width={w},height={h},framerate={fps}/1 ! "
             f"timeoverlay font-desc=\"Sans {max(12, h // 20)}\" text=\"{label}\" ! videoconvert ! "
             f"video/x-raw,format=I420 ! x264enc tune=zerolatency speed-preset=ultrafast key-int-max={fps * 2} "
             f"bitrate={kbps} ! rtph264pay name=pay0 pt=96 config-interval=1")
    if audio:
        video += (" audiotestsrc is-live=true wave=ticks volume=0.3 ! audioconvert ! audioresample ! "
                  "audio/x-raw,rate=48000,channels=2 ! avenc_aac ! rtpmp4gpay name=pay1 pt=97")
    return "( " + video + " )"


class Factory(GstRtspServer.RTSPMediaFactory):
    def __init__(self, key):
        super().__init__()
        self.key = key
        self.set_shared(True)

    def do_configure(self, media):
        with lock:
            viewers[self.key] = viewers.get(self.key, 0) + 1
        media.connect("unprepared", self.on_unprepared)

    def on_unprepared(self, media):
        with lock:
            viewers[self.key] = max(0, viewers.get(self.key, 0) - 1)


server = GstRtspServer.RTSPServer()
server.props.service = str(args.rtsp)
mounts = server.get_mount_points()
for ch in range(args.channels):
    for (sid, pid, kind, w, h, fps, kbps, audio) in PROFILES:
        path = f"{ch}/{sid}"
        if path in HIDE:
            continue
        f = Factory((ch, sid))
        f.set_launch(launch(ch, sid, w, h, fps, kbps, audio))
        mounts.add_factory("/" + path, f)




def pre_describe(client, ctx):
    path = ctx.uri.abspath.strip("/")
    if path in FAIL:
        return GstRtsp.RTSPStatusCode.SERVICE_UNAVAILABLE
    return GstRtsp.RTSPStatusCode.OK


server.connect("client-connected", lambda srv, client: client.connect("pre-describe-request", pre_describe))
server.attach(None)


def catalogue():
    chans = []
    for ch in range(args.channels):
        profs = []
        for (sid, pid, kind, w, h, fps, kbps, audio) in PROFILES:
            if f"{ch}/{sid}" in HIDE:
                continue
            profs.append({"bitrate_kbps": kbps, "codec": "h264", "fps": fps, "height": h, "id": pid,
                          "kind": kind, "rtsp_uri": f"rtsp://{args.host}:{args.rtsp}/{ch}/{sid}",
                          "stream_id": sid, "width": w})
        chans.append({"id": ch, "profiles": profs})
    return {"channels": chans}


def status():
    chans = []
    with lock:
        v = dict(viewers)
    for ch in range(args.channels):
        nosig = ch in NO_SIGNAL
        chans.append({
            "id": ch,
            "source": {"description": "" if nosig else f"mock source {ch}", "fps": 0 if nosig else 25,
                       "signal": not nosig, "splash": nosig, "width": 0 if nosig else 1920,
                       "height": 0 if nosig else 1080},
            "preview": [{"id": sid, "clients": v.get((ch, sid), 0),
                         "state": "running" if v.get((ch, sid), 0) else "idle"}
                        for (sid, *_rest) in PROFILES if sid >= 2],
            "streams": [{"id": sid, "clients": v.get((ch, sid), 0), "codec": "h264", "fps": fps,
                         "width": w, "height": h, "bitrate_kbps": kbps}
                        for (sid, pid, kind, w, h, fps, kbps, audio) in PROFILES if kind == "stream"],
        })
    return {"channels": chans, "rtsp": {"enabled": True}, "uptime_s": 1}


class Http(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.rstrip("/") == "/api/v1/preview":
            body = catalogue()
        elif self.path.rstrip("/") == "/api/v1/status":
            body = status()
        else:
            self.send_error(404)
            return
        data = json.dumps(body, indent=2).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *a):
        pass


httpd = ThreadingHTTPServer(("0.0.0.0", args.http), Http)
threading.Thread(target=httpd.serve_forever, daemon=True).start()
print(f"CAMAI mock: http://127.0.0.1:{args.http}/api/v1/preview, rtsp://127.0.0.1:{args.rtsp}/<ch>/<stream>",
      flush=True)
GLib.MainLoop().run()
