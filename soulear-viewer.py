#!/usr/bin/env python3
"""
soulear_viewer.py - Live-Bild eines Soulear/AiSee-Otoskops (Beken BK7231U, Firmware "XRH")
im Browser, VLC oder ffplay anzeigen - ohne die Hersteller-App.

Ablauf:
  1. Wir öffnen einen UDP-Socket auf einem freien Port P.
  2. Wir schicken dem Otoskop den START-Befehl an UDP 10006 und teilen ihm darin P mit.
  3. Das Otoskop schickt ab dann JPEG-Stückchen an P. Wir setzen sie zu ganzen Bildern zusammen.
  4. Ein kleiner HTTP-Server liefert die Bilder als MJPEG-Stream aus.

Protokoll-Grundlage: pedrodinisf/otoscope-viewer und Fyfar/ms5-wifi-microscope (beide GitHub).
Nur Python-Standardbibliothek nötig.

Aufruf:  python3 soulear_viewer.py [KAMERA_IP]
Dann:    http://127.0.0.1:45100          (Browser)
         http://127.0.0.1:45100/stream   (VLC / ffplay)
         http://127.0.0.1:45100/snapshot (Einzelbild)
"""

import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# --- Konfiguration ---------------------------------------------------------------
CAMERA_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.1"
DISCOVERY_PORT = 10005     # hierhin geht GetDeviceInfo (muss vor START kommen)
VIDEO_CTRL_PORT = 10006    # hierhin geht der START-Befehl
HTTP_PORT = 45100          # Port unseres eigenen MJPEG-Servers
HTTP_BIND = "0.0.0.0"      # 0.0.0.0 = auch aus dem Heimnetz erreichbar (z.B. vom Handy)
CHUNK_HEADER_LEN = 16      # jedes Videopaket beginnt mit 16 Byte Kopfdaten
STALL_TIMEOUT_S = 1.0      # so lange ohne Daten -> START erneut senden
DEBUG_HEADERS = 5          # so viele Paket-Header zur Kontrolle ausgeben

MAGIC = 0xFFEEFFEE
MAGIC_BYTES = b"\xee\xff\xee\xff"   # so steht die Magic "auf der Leitung" (little-endian)
SOI = b"\xff\xd8"                   # JPEG "Start of Image"
EOI = b"\xff\xd9"                   # JPEG "End of Image"


# --- Gemeinsamer Bildspeicher ----------------------------------------------------
class FrameStore:
    """Hält das jeweils neueste JPEG. Der Empfänger schreibt, die HTTP-Clients lesen."""

    def __init__(self):
        self._cond = threading.Condition()
        self._frame = None
        self._seq = 0              # zählt hoch bei jedem neuen Bild
        self.frames_total = 0

    def publish(self, jpeg: bytes):
        with self._cond:
            self._frame = jpeg
            self._seq += 1
            self.frames_total += 1
            self._cond.notify_all()   # wartende Stream-Clients aufwecken

    def wait_next(self, last_seq: int, timeout: float = 2.0):
        """Wartet, bis ein Bild neuer als last_seq da ist (oder Timeout)."""
        with self._cond:
            self._cond.wait_for(lambda: self._seq != last_seq, timeout=timeout)
            return self._seq, self._frame

    def latest(self):
        with self._cond:
            return self._frame


store = FrameStore()


# --- Protokoll: START-Befehl -----------------------------------------------------
def build_start_packet(port: int) -> bytes:
    """
    START-Befehl laut Doku:  eeffeeff 0200 0400 01 00 0200 <PORT_LE16> 0000
      magic  = 0xFFEEFFEE
      id     = 2
      type   = 0x04 (OpenVideo)
      unk    = 1
      err    = 0
      length = 2
      danach: unser Empfangsport (2 Byte, little-endian) + 2 Nullbytes
    """
    header = struct.pack("<IHHBBH", MAGIC, 2, 0x04, 1, 0, 2)
    return header + struct.pack("<H", port) + b"\x00\x00"


def build_discovery_packet() -> bytes:
    """GetDeviceInfo (type 0x01, id 0, ohne Nutzdaten). Ohne diesen Befehl
    bestätigt das Gerät START zwar, schickt aber kein Video."""
    return struct.pack("<IHHBBH", MAGIC, 0, 0x01, 1, 0, 0)


# --- Empfänger-Thread: UDP-Pakete -> ganze JPEGs ---------------------------------
def receiver():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # Größerer Empfangspuffer, damit bei kurzen Rucklern keine Pakete verloren gehen
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind(("0.0.0.0", 0))                # Port 0 = Betriebssystem wählt freien Port
    my_port = sock.getsockname()[1]
    sock.settimeout(STALL_TIMEOUT_S)

    start_pkt = build_start_packet(my_port)
    print(f"[video] Empfange auf UDP-Port {my_port}")
    print(f"[video] Sende START an {CAMERA_IP}:{VIDEO_CTRL_PORT} -> {start_pkt.hex(' ')}")

    discovery_pkt = build_discovery_packet()

    def handshake():
        # Beides vom Video-Socket selbst, damit die Antworten (ACK) auch hier landen
        sock.sendto(discovery_pkt, (CAMERA_IP, DISCOVERY_PORT))
        sock.sendto(start_pkt, (CAMERA_IP, VIDEO_CTRL_PORT))

    handshake()

    buf = bytearray()        # hier wächst das aktuelle JPEG
    in_frame = False         # sind wir gerade mitten in einem Bild?
    debug_left = DEBUG_HEADERS

    while True:
        # 1) Paket empfangen - bei Stille START wiederholen
        try:
            pkt, _addr = sock.recvfrom(65535)
        except socket.timeout:
            print("[video] Keine Daten -> sende START erneut")
            handshake()
            in_frame = False
            continue

        # 2) Steuer-Antworten (ACK) beginnen mit der Magic -> keine Videodaten
        if pkt.startswith(MAGIC_BYTES):
            print(f"[video] ACK vom Gerät: {pkt[:12].hex(' ')} ({len(pkt)} Byte)")
            continue

        if len(pkt) <= CHUNK_HEADER_LEN:
            continue

        # 3) Zur Kontrolle die ersten Header ausgeben
        if debug_left > 0:
            print(f"[video] Header: {pkt[:CHUNK_HEADER_LEN].hex(' ')}  (+{len(pkt) - CHUNK_HEADER_LEN} Byte JPEG)")
            debug_left -= 1

        # 4) 16-Byte-Kopf abschneiden, Rest sind JPEG-Bytes
        payload = pkt[CHUNK_HEADER_LEN:]

        # 5) Beginnt hier ein neues Bild? (erstes Stück startet mit FF D8)
        if payload.startswith(SOI):
            buf = bytearray(payload)
            in_frame = True
        elif in_frame:
            buf += payload
        else:
            # Mittendrin eingestiegen -> warten bis zum nächsten Bildanfang
            continue

        # 6) Endet hier das Bild? (FF D9, eventuell gefolgt von Füll-Nullen)
        if payload.rstrip(b"\x00").endswith(EOI):
            frame = bytes(buf).rstrip(b"\x00")
            store.publish(frame)
            in_frame = False


# --- Statistik-Thread: Bilder pro Sekunde ausgeben -------------------------------
def stats():
    last = 0
    while True:
        time.sleep(5)
        total = store.frames_total
        print(f"[stats] {(total - last) / 5:.1f} fps, {total} Bilder gesamt")
        last = total


# --- HTTP-Server -----------------------------------------------------------------
INDEX_HTML = b"""<!doctype html>
<html><head><meta charset="utf-8"><title>Soulear Viewer</title>
<style>body{background:#111;color:#ddd;font-family:sans-serif;text-align:center}
img{max-width:95vw;max-height:85vh;border-radius:50%}</style></head>
<body><h3>Soulear Live</h3><img src="/stream">
<p><a style="color:#8cf" href="/snapshot" download="otoskop.jpg">Snapshot speichern</a></p>
</body></html>"""


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            self._send(200, "text/html; charset=utf-8", INDEX_HTML)

        elif self.path == "/snapshot":
            frame = store.latest()
            if frame is None:
                self._send(503, "text/plain", b"Noch kein Bild empfangen")
            else:
                self._send(200, "image/jpeg", frame)

        elif self.path == "/stream":
            # MJPEG = viele JPEGs hintereinander, getrennt durch eine "boundary"
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            seq = 0
            try:
                while True:
                    new_seq, frame = store.wait_next(seq)
                    if new_seq == seq or frame is None:
                        continue          # Timeout ohne neues Bild
                    seq = new_seq
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\n")
                    self.wfile.write(b"Content-Length: %d\r\n\r\n" % len(frame))
                    self.wfile.write(frame)
                    self.wfile.write(b"\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass                      # Client hat das Fenster geschlossen

        else:
            self._send(404, "text/plain", b"Not found")

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass   # keine Zeile pro HTTP-Anfrage ins Terminal


# --- Start -----------------------------------------------------------------------
def main():
    threading.Thread(target=receiver, daemon=True).start()
    threading.Thread(target=stats, daemon=True).start()

    server = ThreadingHTTPServer((HTTP_BIND, HTTP_PORT), Handler)
    print(f"[http] Viewer läuft: http://127.0.0.1:{HTTP_PORT}  (Stream: /stream, Einzelbild: /snapshot)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nBeendet.")


if __name__ == "__main__":
    main()