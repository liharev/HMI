#!/usr/bin/python3
"""Mock ONVIF camera (+ a Flussonic media_info.json) for testing stream discovery.

    python3 tools/onvif-mock.py [--port 8082] [--user admin --password 12345] [--rtsp 127.0.0.1:8555]

ONVIF device / media service at http://127.0.0.1:8082/onvif/device_service:
  GetSystemDateAndTime (clock 2 minutes ahead), GetCapabilities (Media XAddr with an
  internal address 10.0.0.5, as cameras behind NAT report), GetProfiles (3 profiles),
  GetStreamUri. WS-UsernameToken PasswordDigest is checked.
Flussonic: GET /cam1/media_info.json  (video tracks v1 1920x1080, v2 640x360 + audio a1).
"""
import argparse
import base64
import datetime
import hashlib
import json
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=8082)
ap.add_argument("--user", default="admin")
ap.add_argument("--password", default="12345")
ap.add_argument("--rtsp", default="127.0.0.1:8555", help="host:port of the RTSP streams")
args = ap.parse_args()

SKEW = datetime.timedelta(minutes=2)
rtsp_port = args.rtsp.split(":")[1] if ":" in args.rtsp else "554"
PROFILES = [
    ("main", "MainStream", "H264", 1920, 1080, 25, 6000, True, "0/0"),
    ("sub", "SubStream", "H264", 640, 360, 25, 1000, False, "0/1"),
    ("third", "ThirdStream", "H264", 1280, 720, 15, 900, False, "0/4"),
]

ENV = ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
       'xmlns:tt="http://www.onvif.org/ver10/schema" xmlns:tds="http://www.onvif.org/ver10/device/wsdl" '
       'xmlns:trt="http://www.onvif.org/ver10/media/wsdl"><s:Body>{}</s:Body></s:Envelope>')


def fault(text):
    return ENV.format('<s:Fault><s:Code><s:Value>s:Sender</s:Value></s:Code><s:Reason>'
                      f'<s:Text xml:lang="en">{text}</s:Text></s:Reason></s:Fault>')


def authorized(body):
    m = {k: re.search(rf"<(?:\w+:)?{k}(?:\s[^>]*)?>([^<]*)<", body) for k in ("Username", "Password", "Nonce", "Created")}
    if not all(m.values()):
        return False
    user, digest, nonce, created = (m[k].group(1) for k in ("Username", "Password", "Nonce", "Created"))
    expect = base64.b64encode(hashlib.sha1(base64.b64decode(nonce) + created.encode() +
                                           args.password.encode()).digest()).decode()
    try:
        t = datetime.datetime.strptime(created, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc)
    except ValueError:
        return False
    cam_now = datetime.datetime.now(datetime.timezone.utc) + SKEW
    return user == args.user and digest == expect and abs((cam_now - t).total_seconds()) < 30


def profile_xml(p):
    token, name, enc, w, h, fps, kbps, audio, _ = p
    a = ('<tt:AudioEncoderConfiguration token="aec"><tt:Name>AAC</tt:Name><tt:Encoding>AAC</tt:Encoding>'
         '</tt:AudioEncoderConfiguration>') if audio else ""
    return (f'<trt:Profiles token="{token}" fixed="true"><tt:Name>{name}</tt:Name>'
            f'<tt:VideoSourceConfiguration token="vsc"><tt:Name>VideoSource</tt:Name>'
            f'<tt:Bounds x="0" y="0" width="1920" height="1080"/></tt:VideoSourceConfiguration>'
            f'<tt:VideoEncoderConfiguration token="vec_{token}"><tt:Name>VE_{token}</tt:Name>'
            f'<tt:Encoding>{enc}</tt:Encoding><tt:Resolution><tt:Width>{w}</tt:Width><tt:Height>{h}</tt:Height>'
            f'</tt:Resolution><tt:RateControl><tt:FrameRateLimit>{fps}</tt:FrameRateLimit>'
            f'<tt:BitrateLimit>{kbps}</tt:BitrateLimit></tt:RateControl></tt:VideoEncoderConfiguration>{a}'
            f'</trt:Profiles>')


class Handler(BaseHTTPRequestHandler):
    def reply(self, code, data, ctype="application/soap+xml; charset=utf-8"):
        data = data.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path.startswith("/cam1/media_info.json"):
            info = {"tracks": [
                {"content": "video", "track_id": "v1", "codec": "h264", "width": 1920, "height": 1080,
                 "fps": 25, "bitrate": 4000},
                {"content": "video", "track_id": "v2", "codec": "h264", "width": 640, "height": 360,
                 "fps": 25, "bitrate": 800},
                {"content": "audio", "track_id": "a1", "codec": "aac", "bitrate": 128}]}
            self.reply(200, json.dumps(info), "application/json")
        else:
            self.send_error(404)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode(errors="replace")
        if "GetSystemDateAndTime" in body:
            t = datetime.datetime.now(datetime.timezone.utc) + SKEW
            self.reply(200, ENV.format(
                '<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:UTCDateTime>'
                f'<tt:Time><tt:Hour>{t.hour}</tt:Hour><tt:Minute>{t.minute}</tt:Minute><tt:Second>{t.second}</tt:Second></tt:Time>'
                f'<tt:Date><tt:Year>{t.year}</tt:Year><tt:Month>{t.month}</tt:Month><tt:Day>{t.day}</tt:Day></tt:Date>'
                '</tt:UTCDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>'))
            return
        if not authorized(body):
            self.reply(400, fault("Sender not Authorized"))
            return
        if "GetCapabilities" in body:
            self.reply(200, ENV.format(
                '<tds:GetCapabilitiesResponse><tds:Capabilities><tt:Media>'
                '<tt:XAddr>http://10.0.0.5/onvif/media_service</tt:XAddr></tt:Media>'
                '</tds:Capabilities></tds:GetCapabilitiesResponse>'))
        elif "GetProfiles" in body:
            self.reply(200, ENV.format('<trt:GetProfilesResponse>' + "".join(profile_xml(p) for p in PROFILES)
                                       + '</trt:GetProfilesResponse>'))
        elif "GetStreamUri" in body:
            tok = re.search(r"ProfileToken>([^<]+)<", body).group(1)
            path = next(p[8] for p in PROFILES if p[0] == tok)
            self.reply(200, ENV.format(
                '<trt:GetStreamUriResponse><trt:MediaUri>'
                f'<tt:Uri>rtsp://10.0.0.5:{rtsp_port}/{path}</tt:Uri></trt:MediaUri></trt:GetStreamUriResponse>'))
        else:
            self.reply(400, fault("Action not supported"))

    def log_message(self, fmt, *a):
        pass


print(f"ONVIF mock: http://127.0.0.1:{args.port}/onvif/device_service (user {args.user}), "
      f"Flussonic: http://127.0.0.1:{args.port}/cam1/media_info.json", flush=True)
ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()
