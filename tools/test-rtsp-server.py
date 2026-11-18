#!/usr/bin/python3
"""Local RTSP test server (H.264, H.265, MJPEG) for checking HMI VMS without cameras.

    sudo apt install python3-gi gir1.2-gst-rtsp-server-1.0 gstreamer1.0-plugins-ugly
    python3 tools/test-rtsp-server.py [port]

Streams: rtsp://127.0.0.1:8554/h264  /h265  /mjpeg  /h264-4k
"""
import sys

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstRtspServer", "1.0")
from gi.repository import GLib, Gst, GstRtspServer  # noqa: E402

Gst.init(None)
port = sys.argv[1] if len(sys.argv) > 1 else "8554"

SRC = ("videotestsrc is-live=true pattern={pattern} ! "
       "video/x-raw,width={w},height={h},framerate=25/1 ! "
       "timeoverlay font-desc=\"Sans 32\" ! videoconvert ! video/x-raw,format=I420 ! ")
STREAMS = {
    "/h264": SRC + "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=50 bitrate=2000 ! "
                   "rtph264pay name=pay0 pt=96 config-interval=1",
    "/h265": SRC + "x265enc tune=zerolatency speed-preset=ultrafast key-int-max=50 bitrate=2000 ! "
                   "rtph265pay name=pay0 pt=96 config-interval=1",
    "/mjpeg": SRC + "jpegenc quality=70 ! rtpjpegpay name=pay0 pt=26",
    "/h264-4k": SRC + "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=50 bitrate=8000 ! "
                      "rtph264pay name=pay0 pt=96 config-interval=1",
}
SIZES = {"/h264": (1280, 720, "ball"), "/h265": (1920, 1080, "smpte"),
         "/mjpeg": (640, 480, "pinwheel"), "/h264-4k": (3840, 2160, "bar")}

server = GstRtspServer.RTSPServer()
server.props.service = port
mounts = server.get_mount_points()
for path, launch in STREAMS.items():
    w, h, pattern = SIZES[path]
    factory = GstRtspServer.RTSPMediaFactory()
    factory.set_launch("( " + launch.format(w=w, h=h, pattern=pattern) + " )")
    factory.set_shared(True)
    mounts.add_factory(path, factory)
server.attach(None)
print("RTSP test server: " + ", ".join(f"rtsp://127.0.0.1:{port}{p}" for p in STREAMS), flush=True)
GLib.MainLoop().run()
