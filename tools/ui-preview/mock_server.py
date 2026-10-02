#!/usr/bin/env python3
"""
Mock server for the WiFi-Cam-Proxy web UI - develop and screenshot the UI without hardware.

The pages are not copied: they are compiled straight from firmware/include/web_ui.h with
the host C++ compiler, so the browser sees exactly the bytes the firmware would send.
The API (/status, /cameras.json, /stream, /orientation, ...) is simulated with plausible
values; the camera image is a generated test picture, clearly marked as simulated.

    python3 tools/ui-preview/mock_server.py [--port 8080] [--scenario normal|choose|rescue]

Then open http://127.0.0.1:8080/. Needs g++ (or c++) and Pillow (pip install pillow).
"""

import argparse
import io
import json
import math
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = ROOT / "firmware" / "include"

# Page name in web_ui.h -> URL
PAGES = {
    "INDEX_HTML": "/",
    "CALIBRATE_HTML": "/calibrate",
    "UPDATE_HTML": "/update",
    "CAMERAS_HTML": "/cameras",
    "WIFI_SETUP_HTML": "/wifi-setup",
    "APP_JS": "/app.js",
    "STYLE_CSS": "/style.css",
}


def build_pages() -> dict:
    """Compiles a tiny host program that includes web_ui.h and dumps every page."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        lines = ["#include <cstdio>", '#include "web_ui.h"', "int main(){"]
        for name in PAGES:
            lines.append(f'{{FILE*f=fopen("{tmp / name}","wb");fwrite({name},1,sizeof({name})-1,f);fclose(f);}}')
        lines.append("return 0;}")
        src = tmp / "dump.cpp"
        src.write_text("\n".join(lines))
        exe = tmp / "dump"
        subprocess.run(["c++", "-std=c++17", "-I", str(INCLUDE), str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        return {url: (tmp / name).read_bytes() for name, url in PAGES.items()}


def test_image(size=480) -> bytes:
    """Generated test picture (no real camera image): warm radial gradient, marked as simulated."""
    from PIL import Image, ImageDraw, ImageFilter

    img = Image.new("RGB", (size, size), (0, 0, 0))
    px = img.load()
    c = size / 2
    for y in range(size):
        for x in range(size):
            d = math.hypot(x - c, y - c) / c
            if d < 1:
                k = (1 - d) ** 0.7
                px[x, y] = (int(40 + 190 * k), int(18 + 95 * k * k), int(14 + 70 * k * k))
    draw = ImageDraw.Draw(img)
    for r in (60, 120, 180):  # soft rings for some structure
        draw.ellipse([c - r, c - r, c + r, c + r], outline=(255, 210, 170), width=2)
    img = img.filter(ImageFilter.GaussianBlur(1.2))
    draw = ImageDraw.Draw(img)
    draw.line([c, c - 150, c, c - 60], fill=(255, 255, 255), width=4)  # "up" marker
    draw.text((c - 62, c + 150), "SIMULATED IMAGE", fill=(255, 255, 255))
    # The camera sits rotated by 90° in the otoscope; the web UI always turns the image
    # back by -90°. Deliver the test picture the same way so it ends up upright.
    img = img.rotate(-90)
    out = io.BytesIO()
    img.save(out, "JPEG", quality=85)
    return out.getvalue()


class State:
    """Simulated device state, changed by the POST endpoints."""

    def __init__(self, scenario: str):
        self.scenario = scenario
        self.led = 0
        self.start = time.time()
        self.preferred = "" if scenario == "choose" else "Soulear-6b1c9"
        self.connected = scenario == "normal"
        self.calibration = b"{}"
        self.home_ssid = "HomeNetwork"

    def networks(self):
        return [
            {"ssid": "Soulear-6b1c9", "rssi": -48, "open": True, "proto": "i4season"},
            {"ssid": "wifi_camera_MS5_1A2B", "rssi": -63, "open": True, "proto": "i4season"},
            {"ssid": "HomeNetwork", "rssi": -58, "open": False, "proto": ""},
            {"ssid": "Guest", "rssi": -74, "open": True, "proto": ""},
            {"ssid": "DIRECT-printer", "rssi": -81, "open": False, "proto": ""},
        ]

    def cameras_json(self):
        c = self.connected
        return {
            "state": "connected" if c else "choose" if self.scenario == "choose" else "restart",
            "ssid": "Soulear-6b1c9" if c else "",
            "proto": "i4season" if c else "",
            "preferred": self.preferred,
            "pref_proto": "auto",
            "recognized": 2,
            "scan_age_s": 12,
            "orientation": c,
            "battery": 80 if c else -1,
            "charging": 0 if c else -1,
            "led": self.led if c else -1,
            "led_supported": c,
            "width": 640 if c else 0,
            "height": 480 if c else 0,
            "vendor": "YPC" if c else "",
            "product": "BK7231U-XRH-FBPRO" if c else "",
            "firmware": "HKV41B" if c else "",
            "networks": self.networks(),
        }

    def status_json(self):
        rescue = self.scenario == "rescue"
        return {
            "version": "Oct  1 2026 12:00:00", "reset_reason": "software", "boot_count": 1,
            "mode": "rescue" if rescue else "normal", "fps": 17.2 if self.connected else 0.0,
            "frames": 48211, "dropped": 37, "drop_nomem": 0, "drop_toobig": 0, "drop_incomplete": 37,
            "packets_lost": 112, "damaged": 64, "max_frame": 41730, "handshakes": 3, "keepalives": 0,
            "stalls_loss": 2, "stalls_clean": 0, "clean_stall_times": [], "stream_clients": 1,
            "sse_clients": 1, "client_tasks": 3,
            "wifi_connected": True, "wifi_ssid": "HomeNetwork" if rescue else "Soulear-6b1c9",
            "wifi_rssi": -52, "wifi_mode": "bg", "wifi_tx_dbm": 11.0,
            "wifi_ip": "192.168.178.57" if rescue else "192.168.1.10",
            "home_ssid": self.home_ssid, "ap": "WiFi-Cam-1A2B" if rescue else "", "eth10": 1,
            "cam_proto": "" if rescue else "i4season", "battery": -1 if rescue else 80, "led": self.led,
            "eth_begin": True, "eth_started": True, "eth_link": not rescue, "eth_speed": 0 if rescue else 10,
            "eth_full_duplex": True, "eth_tx_store_forward": 1,
            "eth_ip": "" if rescue else "192.168.178.130",
            "free_heap": 61234, "max_alloc": 31732, "min_heap": 23116, "iram_heap": 18432, "psram": 0,
            "uptime_s": int(time.time() - self.start) + 3 * 3600 + 17 * 60, "last_crash": "",
        }


def make_handler(pages: dict, frame: bytes, state: State):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass

        def send(self, code, ctype, body: bytes):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        def json(self, obj):
            self.send(200, "application/json", json.dumps(obj).encode())

        def do_GET(self):
            path = self.path.split("?")[0]
            if path in pages:
                ctype = {"/app.js": "application/javascript", "/style.css": "text/css"}.get(path, "text/html; charset=utf-8")
                return self.send(200, ctype, pages[path])
            if path == "/status":
                return self.json(state.status_json())
            if path == "/cameras.json":
                return self.json(state.cameras_json())
            if path == "/calibration":
                return self.send(200, "application/json", state.calibration)
            if path == "/snapshot":
                return self.send(200, "image/jpeg", frame)
            if path == "/stream":
                return self.stream()
            if path == "/orientation":
                return self.orientation()
            self.send(404, "text/plain", b"Not found")

        def do_POST(self):
            path = self.path.split("?")[0]
            body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
            if path.startswith("/led/"):
                state.led = 1 if path.endswith("1") else 0
                return self.send(202, "text/plain", b"sent")
            if path == "/calibration":
                state.calibration = body
                return self.send(200, "text/plain", b"Saved")
            if path == "/cameras/select":
                ssid = parse_qs(body.decode()).get("ssid", [""])[0]
                state.preferred, state.connected = ssid, bool(ssid)
                return self.send(202, "text/plain", b"Connecting\xe2\x80\xa6" if ssid else b"Selection cleared")
            if path == "/wifi-setup":
                state.home_ssid = parse_qs(body.decode()).get("ssid", [""])[0]
                return self.send(200, "text/plain", b"Saved (used in rescue mode)")
            self.send(200, "text/plain", b"OK (simulated)")

        def stream(self):
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            try:
                while True:
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(frame))
                    self.wfile.write(frame + b"\r\n")
                    self.wfile.flush()
                    time.sleep(0.2)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def orientation(self):
            # Probe turning slowly around its long axis: one turn every 4 s
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            try:
                while True:
                    a = (time.time() % 4) / 4 * 2 * math.pi
                    msg = {"x": round(-7 + 118 * math.sin(a)), "y": round(6 + 124 * math.cos(a)), "z": 9}
                    self.wfile.write(f"data: {json.dumps(msg)}\n\n".encode())
                    self.wfile.flush()
                    time.sleep(1 / 17)
            except (BrokenPipeError, ConnectionResetError):
                pass

    return Handler


def serve(port=8080, scenario="normal"):
    """Starts the server in a background thread, returns it (for screenshots.py)."""
    pages = build_pages()
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(pages, test_image(), State(scenario)))
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--scenario", choices=["normal", "choose", "rescue"], default="normal")
    args = ap.parse_args()
    serve(args.port, args.scenario)
    print(f"Mock WiFi-Cam on http://127.0.0.1:{args.port}/  (scenario: {args.scenario}, Ctrl+C to stop)")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
