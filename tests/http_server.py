#!/usr/bin/env python3
"""Test-HTTP-Server mit Range-Support und simuliertem Live-HLS (/live.m3u8)."""
import http.server, os, re, sys, time

ROOT = sys.argv[2] if len(sys.argv) > 2 else "."
START = time.time()

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass

    def send_body(self, data, ctype="application/octet-stream"):
        rng = self.headers.get("Range")
        total = len(data)
        if rng:
            m = re.match(r"bytes=(\d+)-(\d*)", rng)
            a = int(m.group(1)); b = int(m.group(2)) if m.group(2) else total - 1
            b = min(b, total - 1)
            if a >= total:
                self.send_response(416); self.send_header("Content-Range", f"bytes */{total}")
                self.send_header("Content-Length", "0"); self.end_headers(); return
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {a}-{b}/{total}")
            data = data[a:b + 1]
        else:
            self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/redirect.mp4":
            self.send_response(302); self.send_header("Location", "/bbb.mp4")
            self.send_header("Content-Length", "0"); self.end_headers(); return
        if path == "/needs_referer.m3u8" and self.headers.get("Referer") != "https://ref.example/":
            self.send_response(403); self.send_header("Content-Length", "0"); self.end_headers(); return
        if path in ("/icy.aac", "/radio_redirect"):
            if path == "/radio_redirect":
                self.send_response(302); self.send_header("Location", "/icy.aac"); self.send_header("Content-Length", "0"); self.end_headers(); return
            # Icecast-artig: keine Laenge, keine Ranges, Daten in Echtzeit (Endlosschleife)
            data = open(os.path.join(ROOT, "radio.aac"), "rb").read()
            self.protocol_version = "HTTP/1.0"
            self.send_response(200)
            self.send_header("Content-Type", "audio/aac")
            self.send_header("icy-name", "Testradio")
            self.send_header("icy-br", "64")
            self.end_headers()
            rate = 64000 // 8 * 2      # doppelte Echtzeit, damit der Test zuegig puffert
            try:
                while True:
                    for i in range(0, len(data), 4096):
                        self.wfile.write(data[i:i + 4096]); self.wfile.flush()
                        time.sleep(4096 / rate)
            except (BrokenPipeError, ConnectionResetError):
                pass
            self.close_connection = True
            return
        if path == "/live.m3u8":
            segs = sorted(f for f in os.listdir(os.path.join(ROOT, "hls")) if f.endswith(".ts"))
            # alle 2 s (Testzeit) kommt ein Segment dazu, Fenster von 4 Segmenten
            avail = min(len(segs), 4 + int((time.time() - START) / 2))
            first = max(0, avail - 4)
            lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:4", f"#EXT-X-MEDIA-SEQUENCE:{first}"]
            for s in segs[first:avail]:
                lines += ["#EXTINF:4.0,", "hls/" + s]
            return self.send_body(("\n".join(lines) + "\n").encode(), "application/vnd.apple.mpegurl")
        fp = os.path.join(ROOT, path.lstrip("/"))
        if not os.path.isfile(fp):
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        with open(fp, "rb") as f:
            self.send_body(f.read())

http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
