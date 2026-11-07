"""Receives what tap.js flushes after a run: meta.json, bitstream.bin, frame-<idx>.raw.

    py -E -X utf8 collector.py <out dir> [port=18471]

Listens on 127.0.0.1 only. The lab page is HTTPS on the LAN; Chrome lets it reach
loopback (a trustworthy origin) after a Private Network Access preflight, answered here.
"""
import http.server
import os
import re
import sys
import urllib.parse

OUT = os.path.abspath(sys.argv[1])
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 18471
SAFE = re.compile(r"^[A-Za-z0-9._-]{1,80}$")


class H(http.server.BaseHTTPRequestHandler):
    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Access-Control-Allow-Private-Network", "true")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        tag = q.get("tag", "")
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        ok = SAFE.match(tag) is not None
        name = None
        if ok and u.path == "/meta":
            name = "meta.json"
        elif ok and u.path == "/bitstream":
            name = "bitstream.bin"
        elif ok and u.path == "/frame" and q.get("idx", "").isdigit():
            name = "frame-%06d.raw" % int(q["idx"])
        if not name:
            self.send_response(400)
            self._cors()
            self.end_headers()
            return
        d = os.path.join(OUT, tag)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name), "wb") as f:
            f.write(body)
        self.send_response(204)
        self._cors()
        self.end_headers()
        sys.stdout.write("%s/%s %d\n" % (tag, name, len(body)))
        sys.stdout.flush()

    def log_message(self, *a):
        pass


os.makedirs(OUT, exist_ok=True)
print("collector on 127.0.0.1:%d -> %s" % (PORT, OUT), flush=True)
http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
