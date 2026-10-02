#!/usr/bin/env python3
"""
soulear_viewer.py - show the live image of a Soulear/AiSee otoscope (Beken BK7231U,
firmware "XRH") in a browser, VLC or ffplay - without the vendor app.

Sequence:
  1. We open a UDP socket on a free port P.
  2. We send the otoscope the START command to UDP 10006 and tell it P in it.
  3. From then on the otoscope sends JPEG chunks to P. We assemble them into whole frames.
  4. A small HTTP server delivers the frames as an MJPEG stream.

Protocol basis: pedrodinisf/otoscope-viewer and Fyfar/ms5-wifi-microscope (both GitHub).
Only the Python standard library is needed.

Usage:  python3 soulear_viewer.py [CAMERA_IP]
Then:   http://127.0.0.1:45100          (browser)
        http://127.0.0.1:45100/stream   (VLC / ffplay)
        http://127.0.0.1:45100/snapshot (single frame)
"""

import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# --- Configuration ---------------------------------------------------------------
CAMERA_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.1"
DISCOVERY_PORT = 10005     # GetDeviceInfo goes here (must come before START)
VIDEO_CTRL_PORT = 10006    # the START command goes here
HTTP_PORT = 45100          # port of our own MJPEG server
HTTP_BIND = "0.0.0.0"      # 0.0.0.0 = also reachable from the home network (e.g. a phone)
CHUNK_HEADER_LEN = 16      # every video packet starts with 16 bytes of header
STALL_TIMEOUT_S = 1.0      # no data this long -> send START again
DEBUG_HEADERS = 5          # print this many packet headers for checking

MAGIC = 0xFFEEFFEE
MAGIC_BYTES = b"\xee\xff\xee\xff"   # the magic as it appears "on the wire" (little-endian)
SOI = b"\xff\xd8"                   # JPEG "start of image"
EOI = b"\xff\xd9"                   # JPEG "end of image"


# --- Shared frame store ----------------------------------------------------------
class FrameStore:
    """Holds the newest JPEG. The receiver writes, the HTTP clients read."""

    def __init__(self):
        self._cond = threading.Condition()
        self._frame = None
        self._seq = 0              # counts up with every new frame
        self.frames_total = 0

    def publish(self, jpeg: bytes):
        with self._cond:
            self._frame = jpeg
            self._seq += 1
            self.frames_total += 1
            self._cond.notify_all()   # wake up waiting stream clients

    def wait_next(self, last_seq: int, timeout: float = 2.0):
        """Waits until a frame newer than last_seq is there (or timeout)."""
        with self._cond:
            self._cond.wait_for(lambda: self._seq != last_seq, timeout=timeout)
            return self._seq, self._frame

    def latest(self):
        with self._cond:
            return self._frame


store = FrameStore()


# --- Protocol: START command -----------------------------------------------------
def build_start_packet(port: int) -> bytes:
    """
    START command per documentation:  eeffeeff 0200 0400 01 00 0200 <PORT_LE16> 0000
      magic  = 0xFFEEFFEE
      id     = 2
      type   = 0x04 (OpenVideo)
      unk    = 1
      err    = 0
      length = 2
      then: our receive port (2 bytes, little-endian) + 2 zero bytes
    """
    header = struct.pack("<IHHBBH", MAGIC, 2, 0x04, 1, 0, 2)
    return header + struct.pack("<H", port) + b"\x00\x00"


def build_discovery_packet() -> bytes:
    """GetDeviceInfo (type 0x01, id 0, no payload). Without this command the device
    acknowledges START but sends no video."""
    return struct.pack("<IHHBBH", MAGIC, 0, 0x01, 1, 0, 0)


# --- Receiver thread: UDP packets -> whole JPEGs ---------------------------------
def receiver():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # larger receive buffer so no packets are lost during short hiccups
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind(("0.0.0.0", 0))                # port 0 = the OS picks a free port
    my_port = sock.getsockname()[1]
    sock.settimeout(STALL_TIMEOUT_S)

    start_pkt = build_start_packet(my_port)
    print(f"[video] receiving on UDP port {my_port}")
    print(f"[video] sending START to {CAMERA_IP}:{VIDEO_CTRL_PORT} -> {start_pkt.hex(' ')}")

    discovery_pkt = build_discovery_packet()

    def handshake():
        # both from the video socket itself, so the replies (ACK) arrive here too
        sock.sendto(discovery_pkt, (CAMERA_IP, DISCOVERY_PORT))
        sock.sendto(start_pkt, (CAMERA_IP, VIDEO_CTRL_PORT))

    handshake()

    buf = bytearray()        # the current JPEG grows here
    in_frame = False         # are we in the middle of a frame?
    debug_left = DEBUG_HEADERS

    while True:
        # 1) receive a packet - repeat START on silence
        try:
            pkt, _addr = sock.recvfrom(65535)
        except socket.timeout:
            print("[video] no data -> sending START again")
            handshake()
            in_frame = False
            continue

        # 2) control replies (ACK) start with the magic -> no video data
        if pkt.startswith(MAGIC_BYTES):
            print(f"[video] ACK from device: {pkt[:12].hex(' ')} ({len(pkt)} bytes)")
            continue

        if len(pkt) <= CHUNK_HEADER_LEN:
            continue

        # 3) print the first headers for checking
        if debug_left > 0:
            print(f"[video] header: {pkt[:CHUNK_HEADER_LEN].hex(' ')}  (+{len(pkt) - CHUNK_HEADER_LEN} bytes JPEG)")
            debug_left -= 1

        # 4) cut off the 16-byte header, the rest are JPEG bytes
        payload = pkt[CHUNK_HEADER_LEN:]

        # 5) does a new frame start here? (first chunk starts with FF D8)
        if payload.startswith(SOI):
            buf = bytearray(payload)
            in_frame = True
        elif in_frame:
            buf += payload
        else:
            # joined mid-frame -> wait for the next frame start
            continue

        # 6) does the frame end here? (FF D9, possibly followed by padding zeros)
        if payload.rstrip(b"\x00").endswith(EOI):
            frame = bytes(buf).rstrip(b"\x00")
            store.publish(frame)
            in_frame = False


# --- Statistics thread: print frames per second ----------------------------------
def stats():
    last = 0
    while True:
        time.sleep(5)
        total = store.frames_total
        print(f"[stats] {(total - last) / 5:.1f} fps, {total} frames total")
        last = total


# --- HTTP server -----------------------------------------------------------------
INDEX_HTML = b"""<!doctype html>
<html><head><meta charset="utf-8"><title>Soulear Viewer</title>
<style>body{background:#111;color:#ddd;font-family:sans-serif;text-align:center}
img{max-width:95vw;max-height:85vh;border-radius:50%}</style></head>
<body><h3>Soulear Live</h3><img src="/stream">
<p><a style="color:#8cf" href="/snapshot" download="otoscope.jpg">Save snapshot</a></p>
</body></html>"""


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            self._send(200, "text/html; charset=utf-8", INDEX_HTML)

        elif self.path == "/snapshot":
            frame = store.latest()
            if frame is None:
                self._send(503, "text/plain", b"No frame received yet")
            else:
                self._send(200, "image/jpeg", frame)

        elif self.path == "/stream":
            # MJPEG = many JPEGs in a row, separated by a "boundary"
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            seq = 0
            try:
                while True:
                    new_seq, frame = store.wait_next(seq)
                    if new_seq == seq or frame is None:
                        continue          # timeout without a new frame
                    seq = new_seq
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n")
                    self.wfile.write(b"Content-Length: %d\r\n\r\n" % len(frame))
                    self.wfile.write(frame)
                    self.wfile.write(b"\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass                      # client closed the window

        else:
            self._send(404, "text/plain", b"Not found")

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass   # no line per HTTP request in the terminal


# --- Start -----------------------------------------------------------------------
def main():
    threading.Thread(target=receiver, daemon=True).start()
    threading.Thread(target=stats, daemon=True).start()

    server = ThreadingHTTPServer((HTTP_BIND, HTTP_PORT), Handler)
    print(f"[http] viewer running: http://127.0.0.1:{HTTP_PORT}  (stream: /stream, single frame: /snapshot)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
