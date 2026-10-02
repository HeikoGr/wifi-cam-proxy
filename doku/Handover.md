# HANDOVER – Multi-Cam-Proxy für günstige WLAN-Otoskope & -Mikroskope

> Übergabe aus einer Recherche-Session mit Claude (Stand: 2026-10-01).
> Zweck: Startpunkt für die Weiterarbeit in VS Code mit Claude.
> **Nicht übernommen** wurde der in der Session erzeugte ESP32-MVP-Prototyp (Adapter, Detector,
> `main.cpp`, `platformio.ini`), weil er auf einer falschen Protokollzuordnung beruhte (siehe Abschnitt 7).
> Die **Code-Beispiele in Abschnitt 5** sind dagegen neu und direkt aus dem Quellcode bzw. der
> verifizierten Doku der Referenz-Repos abgeleitet.

---

## 1. Die Idee

Billige WLAN-Otoskope, Ohrreiniger-Kameras, Endoskope und WLAN-Mikroskope spannen alle ein
eigenes WLAN auf und sprechen proprietäre, aber **unverschlüsselte UDP-Protokolle**. Pro Hersteller
braucht man eine eigene, meist chinesische App (teils mit Cloud-Lizenzprüfung, siehe 3.1).

**Ziel:** Ein **Multi-Cam-Proxy**, der

1. Kameras in der Nähe erkennt (WLAN-Scan nach SSID-Präfix **und** BLE-Scan),
2. sich mit der jeweiligen Kamera verbindet,
3. das passende Protokoll per **Adapter/Vendor-Modul** spricht,
4. alles **einheitlich** ausgibt: MJPEG über HTTP + Telemetrie (Lagewinkel, Akku, Licht) als JSON/SSE,
5. ohne Hersteller-App und ohne Cloud funktioniert.

Zielgruppe: eigener Homelab-Einsatz, perspektivisch Maker Labs (ein Gerät, das „alles Billige“
anzeigen kann).

### Eigene Geräte (Testhardware)

| Gerät | App | Vermutete Familie | Status |
|---|---|---|---|
| Hopefox-Ohrreiniger (Soulear-App) | Soulear | **i4season** (10005/10006) | Doku verifiziert an *Hopefox Find T* durch king-cake, am eigenen Gerät noch prüfen |
| Zoom-Kamera/Mikroskop | MAX-VIEW 1.0.12(46), iOS | i4season-App, Protokoll **unklar** (MaxSee 10900/20000 oder i4season) | per Capture prüfen |
| Tuya ZB-GW03-V1.3 (ESP32) | – | Kandidat für Proxy-Hardware | Flash/Chip am Gerät verifizieren |

---

## 2. Bekannte Kamera-Familien (Protokoll-Übersicht)

Quellen in Abschnitt 3. „verifiziert“ = im Quellcode bzw. in der Doku des Repos nachgelesen.

| Familie | Erkennung | Steuerung | Video | Lage/Akku/Licht | Quelle | Stand |
|---|---|---|---|---|---|---|
| **i4season / Soulear** | SSID `Soulear-…` (App-Präfixe auch `i4season`, `SUEAR`, `inskam`, `Yanxuan`); Broadcast an `192.168.1.255:10005` | UDP 10005 (req/resp), 10006 (OpenVideo), Status-Push an Client-Port 10007 | UDP an selbst gewählten `picport`, JPEG in Chunks, Header 16 B (Typ 1) / 28 B (Typ 6) | Accel gepackt (3× 9 Bit + Vorzeichen) im Video-Header; Akku in Devinfo und Status-Push; LED per Cmd `0x000A` | king-cake/otoscope-windows `docs/i4season-protocol.md`, rico001/open-web-soulear | verifiziert (Doku) |
| **EarFairy** | SSID-Präfix `Cooleer_` (WLAN-Scan) | UDP 7099 (Heartbeat `01 01` jede 1 s) | **RTSP** `rtsp://<ip>:7070/webcam` (RTP/JPEG) | Winkel Byte 0–1, Akku Byte 2 (101 = lädt), Flip-Flag Byte 7; LED `05 02` an / `05 01` aus | rbeilvert/otoscope | verifiziert (Code) |
| **JEGOAT** | **BLE**-Advert mit BSSID; WPA2-Passphrase = BSSID als Hex (klein) | UDP 61500 (`10 01` alle 5 s → JSON-Telemetrie) | UDP 61501: Start `20 01`, Stop `20 02`, Keepalive 1 s; 24-B-Header, Float32-Winkel an Offset 3 | Winkel im Video-Header, Akku per JSON | rbeilvert/otoscope | verifiziert (Code) |
| **Xylla** | **BLE**-Advert, Manufacturer-Data `66 99` + BSSID | UDP 50000, Magic `0x9999`: `0x1017` Akku, `0x1060` Board-Info, `0x1002` Version | UDP 8032: Cmd 1 = Start-Preview (11× Burst, dann alle 800 ms), Cmd 2 = Stop; 24-B-Header, Magic `0x66` | Accel 3×10 Bit an Offset 16 im Video-Header | rbeilvert/otoscope | verifiziert (Code) |
| **iTiMO / jetion** | BLE-Name `iTiMO-` / `jetion_` (vor Xylla prüfen, gleiche BLE-Magic) | UDP 50000 | UDP 8031/8032 | wie Xylla | rbeilvert/otoscope, raunak51299/Endoscope-Hacking | verifiziert (Code) |
| **MaxSee / JoyHonest („JHCMD“)** | SSID z. B. `Maxsee_xxxx`, Kamera fest auf `192.168.29.1` | UDP 20000: `JHCMD` + 2 Byte (`10 00`, `20 00` Init; `d0 01` Heartbeat/Start; `d0 02` Stop) | UDP 10900 (Client bindet), 8-B-Header, JPEG | nicht bekannt | czietz/wifimicroscope (Code), chzsoft.de | verifiziert (Code, Stand 2020) |
| **Borescope (TCP)** | – | – | TCP 7060, eigenes Frame-Format | – | mplough.github.io, n8henrie.com | aus Recherche |
| i4season-Nebenfamilien B, C, Novatek-HTTP | – | B: 44506/52219/52220; C: 6080/6090; Novatek: HTTP `192.168.1.254` | – | – | Ende von `i4season-protocol.md` | nur erwähnt, nicht analysiert |

**Auflösung, realistisch:** Die Hopefox Find T liefert **480×480-JPEGs**, obwohl der Header 640×480
meldet (~17 fps, ~12–14 Chunks à ≤1400 B, ~7 % Frame-Verlust im WLAN). MaxSee-Mikroskope sollen laut
Recherche 1280×720 liefern. Amazon-Angaben wie „1080p/5 MP“ beziehen sich meist auf den Sensor oder sind
Marketing. Pro Gerät messen, nicht annehmen.

---

## 3. Links – Projekte & Quellen

Alle GitHub-Repos am 2026-10-01 per `git ls-remote` auf Existenz geprüft (✅).
Gists konnten aus der Session-Umgebung nicht geprüft werden (❔).

### 3.1 Kern-Referenzen (Otoskope)

- ✅ **[rbeilvert/otoscope](https://github.com/rbeilvert/otoscope)** – Android-App, **die** Referenz für
  den Multi-Vendor-Ansatz: `vendor/CameraVendor.kt` + `CameraVendors`-Registry, Discovery per BLE **oder**
  WLAN-Scan, Sessions für EarFairy, JEGOAT, Xylla, iTiMO. GPL. Letzter Commit im Klon: 2026-09-15.
  - [Issue #38 – i4season/Soulear-Support](https://github.com/rbeilvert/otoscope/issues/38)
  - [Issue #41 – AIR-ES-xxxxxx-Geräte](https://github.com/rbeilvert/otoscope/issues/41)
- ✅ **[king-cake/otoscope](https://github.com/king-cake/otoscope)** – Fork von rbeilvert mit Soulear/i4season-Support,
  [Release v0.7.0-soulear.1](https://github.com/king-cake/otoscope/releases/tag/v0.7.0-soulear.1). GPL.
- ✅ **[king-cake/otoscope-windows](https://github.com/king-cake/otoscope-windows)** – Python/Windows-Viewer.
  **`docs/i4season-protocol.md`** = beste Protokollbeschreibung für Soulear (aus `libWifiCamera.so` per
  Ghidra, verifiziert an einer Find T am 2026-09-30). `tools/i4season_probe.py` = Testclient nur mit der
  Python-Standardbibliothek. GPL.
- ✅ **[rico001/open-web-soulear](https://github.com/rico001/open-web-soulear)** – Node.js/TypeScript-Backend
  + React-Frontend, Kamera → UDP → Backend → HTTP/Browser. Deutsche Doku inkl. **statischer Analyse der
  Soulear-App** (`docs/README-statische-analyse.md`: Cloud-Lizenzreport `yun.simicloud.com`, Sperrmöglichkeit
  per Server, SSID-Präfixe). Architektonisch am nächsten an der Proxy-Idee. GPL.
- ✅ **[The-Dorkknight/earscope-app](https://github.com/The-Dorkknight/earscope-app)** – Ear-Scope-App mit
  eigener Protokoll-Erklärung: [HOW_IT_WORKS.md](https://github.com/The-Dorkknight/earscope-app/blob/main/HOW_IT_WORKS.md)
- ✅ **[raunak51299/Endoscope-Hacking](https://github.com/raunak51299/Endoscope-Hacking)** – iTiMO-Endoskop,
  UDP 8031/50000, Akku `0x1017`.

### 3.2 WLAN-Mikroskope (MaxSee / MAX-VIEW / JoyHonest)

- ✅ [czietz/wifimicroscope](https://github.com/czietz/wifimicroscope) – Ursprungs-PoC zum chzsoft-Artikel (BSD-2-Clause), Basis für Beispiel 5.6
- ✅ [loehnertj/maxsee_viewer](https://github.com/loehnertj/maxsee_viewer) – Viewer für MaxSee
- ✅ [slofo82/MaxSee_wifiMicroscope](https://github.com/slofo82/MaxSee_wifiMicroscope) – C#-Implementierung
- ✅ [fbetancourt-dev/microscope-viewer](https://github.com/fbetancourt-dev/microscope-viewer) – Linux-Viewer, JoyHonest MS5B
- 📄 [CHZ-Soft: Reverse-engineering a Wifi microscope](https://www.chzsoft.de/site/hardware/reverse-engineering-a-wifi-microscope/) – Ports 20000/10900, `JHCMD`, Methodik
- 📄 [Hackaday: Reverse Engineering a Wifi Microscope MS5](https://hackaday.io/project/206057-reverse-engineering-a-wifi-microscope-ms5)
- ❔ [Gist TheCrazyT: wifi_microscope_dump.py](https://gist.github.com/TheCrazyT/364ff5d6e893905af9d950f70daa2f29)
- ❔ [Gist sspathak: angepasster czietz-Viewer](https://gist.github.com/sspathak/33cc4bf69cc642eee067dd4eb93b9032)
- App-Stores: [MAX-VIEW iOS](https://apps.apple.com/us/app/max-view/id1629406651) ·
  [Max-see iOS (alt)](https://apps.apple.com/us/app/max-see/id1387691074) ·
  [MAX-VIEW Android `com.i4season.maxview`](https://play.google.com/store/apps/details?id=com.i4season.maxview)

### 3.3 Weitere Endoskop-/Kamera-Projekte

- ✅ [hypeapps/Endoscope](https://github.com/hypeapps/Endoscope) – WiFi-Endoskop-App
- ✅ [hardcodedjoy/udp-camera-iototoy](https://github.com/hardcodedjoy/udp-camera-iototoy) – einfaches UDP-Kamera-Streaming
- ✅ [Kosmonova/esp32s3-uvc](https://github.com/Kosmonova/esp32s3-uvc) – ESP32-S3 + USB-Endoskop (UVC), interessant für eine ESP32-Variante
- ❔ [Gist gitfvb: Notes on Quelima R3 WiFi Camera](https://gist.github.com/gitfvb/09085fd0cd4993549feb7470430d40e9) – andere Familie (Embedded Linux, HTTP 8080)
- 📄 [mplough: Rewriting the video stream from a wi-fi borescope](https://mplough.github.io/2019/12/14/borescope.html) – TCP 7060, JPEG-Reparatur (DRI-Marker)
- 📄 [n8henrie: Reverse Engineering My WiFi Endoscope, Part 4](https://n8henrie.com/2019/02/reverse-engineering-my-wifi-endoscope-part-4/)

### 3.4 Werkzeuge fürs eigene Reverse Engineering

- ✅ [gh2o/rvi_capture](https://github.com/gh2o/rvi_capture) – iPhone-Traffic mitschneiden ohne Mac (für MAX-VIEW iOS)
- 📄 [iOS Packet Capture Tutorial (rvictl)](https://nickhuangcyh.github.io/blog/tools/how-to-capture-network-packet-on-ios/)
- 📄 [Go deep on iOS packet analysis](https://medium.com/@MikeFurtak/go-deep-on-ios-packet-analysis-6a7542eeffb3)
- Android-Weg (einfacher): APK → `jadx`, native `.so` → Ghidra (so ist `i4season-protocol.md` entstanden)

---

## 4. Architektur-Skizze (Vorschlag, noch nichts entschieden)

### 4.1 Wichtigste Randbedingung

**Jede Kamera ist ihr eigener Access Point** (meist `192.168.1.1` bzw. `192.168.29.1`, oft gleiche IP!).
Eine WLAN-Schnittstelle im Client-Modus kann zur selben Zeit nur mit **einem** AP verbunden sein. Daraus folgt:

- **ESP32 (ZB-GW03)** → realistisch **eine Kamera gleichzeitig** (umschaltbar). Vorteil: hat BLE für die
  JEGOAT/Xylla/iTiMO-Discovery. Nachteil: wenig RAM, keine Bildrotation per JPEG-Neukodierung sinnvoll.
- **Linux-Host (Debian-VM im Proxmox-Cluster mit durchgereichten USB-WLAN-Sticks)** → **mehrere Kameras
  gleichzeitig**: ein Stick pro Kamera, jeweils in eigenem Network Namespace (wegen gleicher Kamera-IPs).
  Das ist der eigentliche „Multi“-Proxy.
- Beide Varianten können dieselbe **Ausgabe-Schnittstelle** haben → Clients (Browser, MagicMirror-Modul,
  Home Assistant später) merken keinen Unterschied.

### 4.2 Bausteine

```
Discovery            Session/Adapter (pro Familie)          Ausgabe
─────────            ─────────────────────────────          ───────
WLAN-Scan (SSID)  ─┐  i4season · EarFairy · JEGOAT ·        /cams                   (Liste, JSON)
BLE-Scan (Advert) ─┼─▶ Xylla · iTiMO · MaxSee · …     ─▶    /cams/<id>/mjpeg        (Video)
Manuell (IP)      ─┘  connect · keepalive · Frame-          /cams/<id>/telemetry    (SSE: Winkel, Akku)
                      Reassembly · Telemetrie               /cams/<id>/led          (Steuerung)
```

- **Erkennung über SSID-Präfix / BLE-Advert statt Port-Scan.** UDP-Ports antworten nicht zuverlässig auf
  Testpakete. rbeilvert macht es genauso (`CameraVendor.parseAdvert` / SSID-Filter); die Reihenfolge der
  Vendors ist relevant (iTiMO vor Xylla). Für unbekannte SSIDs als Fallback gezielte Probes je Familie
  (z. B. i4season `DevinfoGet` per Broadcast an :10005).
- **Rotation nicht im Proxy rechnen**, sondern Winkel mitliefern und im Client per CSS/Canvas drehen
  (Beispiel 5.7). So entfällt das JPEG-Neukodieren, was auf dem ESP32 entscheidend ist.
- **Keepalives beachten:** i4season sendet jede Sekunde OpenVideo erneut, wenn kein Video kam; EarFairy
  braucht jede Sekunde einen Heartbeat, JEGOAT Video-Keepalive 1 s + Control 5 s, Xylla 800 ms,
  MaxSee einen periodischen `JHCMD d0 01`.
- **Frame-Reassembly** robust bauen: Sequenzlücken führen zum Verwerfen des Frames, JPEG muss mit `FF D8`
  beginnen; Chunks enden mit Null-Padding nach `FF D9`.

### 4.3 Sprache/Stack (offen)

- Linux-Proxy: Python (asyncio) oder Node.js/TypeScript (passt zu rico001 und zu eigenen
  MagicMirror-Modulen).
- ESP32: ESP-IDF/Arduino mit PlatformIO; nur Familien, die ohne großen Puffer auskommen.

---

## 5. Code-Beispiele (Lagesensor, Akku, Licht, Keepalive)

Alle Beispiele: **Python 3, nur Standardbibliothek**, als Bausteine gedacht (kein fertiges Programm).
Jeder Block nennt seine Quelle. Es sind eigene Nachimplementierungen der Protokollbeschreibungen, keine
kopierten GPL-Codestücke. Vor dem Einsatz am eigenen Gerät prüfen.

### 5.0 Gemeinsames Datenmodell

Jeder Adapter liefert dasselbe Format an die Ausgabe-Schicht (Abschnitt 4.2).

```python
from dataclasses import dataclass
from typing import Optional

@dataclass
class Telemetry:
    angle_deg: Optional[float] = None   # Drehung um die Längsachse, schon inkl. Montage-Offset
    battery_pct: Optional[int] = None   # 0..100
    charging: Optional[bool] = None
    led_on: Optional[bool] = None       # letzter bekannter/gesetzter Zustand
```

### 5.1 i4season / Soulear (Hopefox Find T) – UDP 10005/10006/10007

Quelle: king-cake/otoscope-windows, `docs/i4season-protocol.md` + `tools/i4season_probe.py`.

```python
import math, socket, struct, time

MAGIC = 0xFFEEFFEE
CMD_PORT, VIDEO_CMD_PORT, NOTIFY_PORT = 10005, 10006, 10007
CMD_DEVINFO, CMD_OPEN_VIDEO, CMD_LED = 0x0001, 0x0004, 0x000A

# --- Steuerpaket: 12-Byte-Header (little-endian) + Payload ---------------------
# magic(u32) seq(u16) cmd(u16) 0x01(u8, "Request") status(u8) len(u16) payload
def build_request(seq: int, cmd: int, payload: bytes = b"") -> bytes:
    return struct.pack("<IHHBBH", MAGIC, seq & 0xFFFF, cmd, 1, 0, len(payload)) + payload

def request(cam_ip: str, seq: int, cmd: int, payload=b"", port=CMD_PORT, retries=20):
    """Senden + auf Antwort mit gleichem magic/seq warten (wie die Hersteller-App: 20× 100 ms)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)  # erlaubt Discovery an 192.168.1.255
    s.settimeout(0.1)
    try:
        for _ in range(retries):
            s.sendto(build_request(seq, cmd, payload), (cam_ip, port))
            try:
                data, addr = s.recvfrom(4096)
            except socket.timeout:
                continue
            magic, rseq, rcmd, _dir, status, _ln = struct.unpack_from("<IHHBBH", data)
            if magic == MAGIC and rseq == seq and rcmd == cmd:
                return addr[0], status, data[12:]   # Absender-IP = Kamera-IP
        return None
    finally:
        s.close()

# --- Akku aus DevinfoGet (Payload 0x80 Byte) -----------------------------------
def battery_from_devinfo(p: bytes) -> int:
    status = struct.unpack_from("<H", p, 0x77)[0]
    return status >> 9            # Bits 9..15 = Akku in %, beobachtet: 0xC801 -> 100 %

# --- Licht (LED) -------------------------------------------------------------
# Payload: op, status, brightness. op = LED-Nr. 1 | 0x10 (= schreiben)
# status: 0 aus, 1 an, 2 blinken, 3 atmen. Find T: Helligkeit wird gespeichert, LED dimmt aber nicht.
def led_payload(on: bool) -> bytes:
    return bytes([0x11, 1, 100]) if on else bytes([0x11, 0, 0])
# Anwendung: request(cam_ip, seq, CMD_LED, led_payload(True))   # Antwort = resultierender Zustand

# --- Video öffnen (+ Keepalive) ------------------------------------------------
# picport = lokaler UDP-Port, auf dem wir Video empfangen; client_id = time() einmal pro Session.
def open_video_payload(picport: int, client_id: int) -> bytes:
    return struct.pack("<HHI", picport, 0, client_id)
# Keepalive: Kam > 1 s kein Videopaket, OpenVideo einfach erneut an Port 10006 senden.

# --- Lagesensor aus dem Video-Header ------------------------------------------
# Byte 0 = Typ (1 -> 16-Byte-Header, 6 -> 28-Byte-Header), Byte 5 Bit0 = "hat G-Sensor",
# Bytes 6..9 = gepackter Beschleunigungssensor: je Achse 9 Bit Betrag + 1 Vorzeichenbit.
def decode_i4season_angle(header: bytes, product: str = "") -> float | None:
    if not header[5] & 0x01:
        return None
    g = struct.unpack_from("<I", header, 6)[0]
    def axis(shift_val, shift_sign):
        v = (g >> shift_val) & 0x1FF
        return -v if (g >> shift_sign) & 1 else v
    y = axis(10, 19)
    z = axis(0, 9)
    if z == 0:
        return None                          # Hersteller-App behält dann den alten Winkel
    angle = math.degrees(math.atan2(y, z)) % 360   # Drehung um die Längsachse (x)
    if "FBPRO" in product or "R1" in product:      # Find T meldet "BK7231U-XRH-FBPRO"
        angle = (angle + 180) % 360
    if "w50" in product:
        angle = (angle + 90) % 360
    return angle

# --- Status-Push auf UDP 10007 (ca. 1×/s, ohne Anfrage) ------------------------
# 29-Byte-Paket, cmd 0x0009, Payload-Typ 0x02; Payload-Byte 1 >> 1 = Akku in %.
def battery_from_status_push(pkt: bytes) -> int | None:
    payload = pkt[12:]
    if len(payload) >= 2 and payload[0] == 0x02:
        return payload[1] >> 1
    return None
```

### 5.2 EarFairy (`Cooleer_…`) – UDP 7099 + RTSP

Quelle: rbeilvert/otoscope, `stream/earfairy/EarFairyControlClient.kt`, `EarFairyVideoClient.kt`.

```python
EARFAIRY_CTRL_PORT = 7099
EARFAIRY_RTSP_URL = "rtsp://{ip}:7070/webcam"   # Video läuft über RTSP/RTP (JPEG), z. B. per ffmpeg

HEARTBEAT = b"\x01\x01"        # jede 1 s senden; sonst stoppt die Telemetrie nach ~3 s
LED_ON, LED_OFF = b"\x05\x02", b"\x05\x01"   # fire-and-forget, keine Bestätigung -> Zustand selbst merken

MOUNT_OFFSET_DEG = 90          # Y-201-Hardware: Sensor-Null liegt 90° gegen die Linse verdreht

class EarFairyDecoder:
    def __init__(self):
        self.flip_latched = False      # Byte 7 == 1 wird einmal beim Boot gesetzt -> für die Session merken

    def decode(self, buf: bytes) -> Telemetry | None:
        if len(buf) < 4:
            return None
        low = buf[1] - 256 if buf[1] > 127 else buf[1]          # Byte 1 ist vorzeichenbehaftet
        raw = low + 255 if (buf[0] == 1 or low < 0) else low      # Byte 0 = "High-Bit"
        if len(buf) >= 8 and buf[7] == 1:
            self.flip_latched = True
        angle = (raw + MOUNT_OFFSET_DEG + (90 if self.flip_latched else 0)) % 360

        b = buf[2]                                                # 1..100 = %, 101 = lädt
        if b == 101:
            return Telemetry(angle_deg=angle, battery_pct=100, charging=True)
        return Telemetry(angle_deg=angle, battery_pct=min(max(b, 0), 100), charging=False)

# Byte 3 = Gerätefamilie (0x5A), Byte 4 = Auslöser-Taste ('M' Foto, 'X' Video) – nützlich für
# einen "Foto"-Button am Gerät, der im Proxy einen Snapshot auslöst.
```

### 5.3 JEGOAT – BLE-Discovery, UDP 61501 (Video) / 61500 (Telemetrie)

Quelle: rbeilvert/otoscope, `vendor/JegoatVendor.kt`, `stream/jegoat/JegoatSession.kt`, `FrameAssembler.kt`.

```python
import json, struct

VIDEO_PORT, CTRL_PORT = 61501, 61500
VIDEO_START, VIDEO_STOP = b"\x20\x01", b"\x20\x02"   # Start 10× als Burst, dann jede 1 s als Keepalive
CTRL_POLL = b"\x10\x01"                              # alle 5 s -> Antwort enthält JSON

# WLAN-Zugang aus dem BLE-Advert: Payload-Bytes 6..11 = BSSID; Passphrase = BSSID als Hex klein
def jegoat_wifi_credentials(ble_payload: bytes):
    bssid = ble_payload[6:12]
    return ":".join(f"{b:02X}" for b in bssid), bssid.hex()

# Video-Header (24 Byte): frame_id, chunk_seq (0 = erster), chunks_total, float32 Winkel (LE), 17 B Padding
def parse_jegoat_chunk(pkt: bytes):
    frame_id, chunk_seq, chunks_total = pkt[0], pkt[1], pkt[2]
    angle = struct.unpack_from("<f", pkt, 3)[0]       # schon in Grad, keine Umrechnung nötig
    return frame_id, chunk_seq, chunks_total, angle, pkt[24:]
# Reassembly: Chunks nach chunk_seq sortieren (UDP kann umsortieren); unvollständige Frames verwerfen.

def parse_jegoat_telemetry(reply: bytes) -> Telemetry | None:
    text = reply.decode("utf-8", "replace")
    start = text.find("{")                            # vor dem JSON stehen ein paar Echo-Bytes
    if start < 0:
        return None
    obj = json.loads(text[start:])
    pct = obj.get("battery_percentage")
    return Telemetry(battery_pct=pct if isinstance(pct, int) and 0 <= pct <= 100 else None,
                     charging=bool(obj.get("battery_charging", False)))
    # weitere Felder: device_chip, device_version, width, height, fps, debug_rssi
```

### 5.4 Xylla / iTiMO – UDP 8032 (Video) / 50000 (Steuerung)

Quelle: rbeilvert/otoscope, `stream/xylla/XyllaControlClient.kt`, `XyllaCameraClient.kt`,
`FrameAssembler.kt`, `RotationFilter.kt`.

```python
import math, statistics, struct
from collections import deque

CMD_START_PREVIEW, CMD_STOP_PREVIEW = 1, 2        # an UDP 8032: 11× Burst, dann alle 800 ms
CMD_GET_BATTERY, CMD_GET_BOARD_INFO, CMD_GET_VERSION = 0x1017, 0x1060, 0x1002   # an UDP 50000

# Befehlspaket (24 Byte, LE): magic 0x9999, cmd(u16), seq(u32), 16 Null-Bytes.
# Antwort beginnt mit demselben magic + cmd -> daran zuordnen; bis zu 4 Versuche à 600 ms.
def xylla_command(cmd: int, seq: int) -> bytes:
    return struct.pack("<HHI", 0x9999, cmd, seq) + bytes(16)

def parse_xylla_battery(reply: bytes) -> Telemetry | None:
    if len(reply) < 20:
        return None
    main = struct.unpack_from("<I", reply, 8)[0]
    extra = struct.unpack_from("<I", reply, 16)[0]
    pct = min(max(extra & 0xFFFF, 0), 100)
    status = main >> 16
    plugged = bool(status & 0x01)
    full = plugged and bool(status & 0x02)
    return Telemetry(battery_pct=pct, charging=plugged and not full)

# Lagesensor: Video-Header (24 B, magic 0x66) Bytes 16..19 = 3×10 Bit, 512 = Nullpunkt.
# Mit Rauschfilter: nur aktualisieren, wenn sich das Gerät bewegt (Stdabw. > 3 über 20 Samples).
class XyllaRotationFilter:
    WINDOW = 20

    def __init__(self):
        self.hist = [deque(maxlen=self.WINDOW) for _ in range(3)]
        self.last_angle = 0.0

    def update(self, accel: int) -> float:
        reg = [(accel >> 20) & 0x3FF, (accel >> 10) & 0x3FF, accel & 0x3FF]   # X, Y, Z
        mag = [1024 - r if r >= 512 else r for r in reg]                       # an 512 spiegeln
        for h, m in zip(self.hist, mag):
            h.append(m)
        if len(self.hist[0]) < self.WINDOW:
            return self.last_angle
        if not any(statistics.pstdev(h) > 3 for h in self.hist):
            return self.last_angle            # Gerät liegt still -> Bild nicht "wandern" lassen
        reg_y, reg_z = reg[1], reg[2]
        mag_y, mag_z = mag[1], mag[2]
        if mag_z == 0:
            return self.last_angle
        theta = math.atan(mag_y / mag_z)
        if reg_z > 512:
            theta = math.pi - theta
        if reg_y > 512:
            theta = 2 * math.pi - theta
        theta = (theta + math.pi) % (2 * math.pi)   # Linse ist 180° zum Sensor montiert
        self.last_angle = math.degrees(theta)
        return self.last_angle
```

### 5.5 Telemetrie-Unterschiede auf einen Blick

| Familie | Winkel kommt aus | Akku kommt aus | Licht |
|---|---|---|---|
| i4season | Video-Header Bytes 6..9 (nur wenn Flag-Bit 0) | Devinfo `0x77` und Push auf 10007 | Cmd `0x000A`, mit Bestätigung |
| EarFairy | Telemetrie auf 7099, Bytes 0..1 | Telemetrie Byte 2 | `05 02` / `05 01`, ohne Bestätigung |
| JEGOAT | Video-Header Float32 an Offset 3 | JSON auf 61500 | nicht bekannt |
| Xylla/iTiMO | Video-Header Bytes 16..19 (+ Filter) | Cmd `0x1017` auf 50000 (aktiv abfragen) | nicht bekannt |
| MaxSee | – | – | nicht bekannt (Mikroskop hat Hardware-Dimmer) |

### 5.6 MaxSee / JoyHonest – UDP 20000 (Befehle) / 10900 (Video)

Quelle: czietz/wifimicroscope, `wifi_microscope_dump.py` (BSD-2-Clause, Stand 2020).

```python
import socket

HOST, CMD_PORT, VIDEO_PORT = "192.168.29.1", 20000, 10900   # IP der Kamera ist fest

INIT = [b"JHCMD\x10\x00", b"JHCMD\x20\x00"]   # wie naInit_Re() in der Hersteller-Lib
START = b"JHCMD\xd0\x01"                      # Heartbeat = startet/hält den Datenstrom
STOP = b"JHCMD\xd0\x02"

def run(on_frame):
    cmd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for c in INIT + [START, START]:
        cmd.sendto(c, (HOST, CMD_PORT))
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("", VIDEO_PORT))                 # Kamera sendet an festen Port 10900
    rx.settimeout(1.0)
    buf, frame_no = bytearray(), None
    try:
        while True:
            try:
                data = rx.recv(1450)
            except socket.timeout:
                cmd.sendto(START, (HOST, CMD_PORT))   # Strom eingeschlafen -> neu anstoßen
                continue
            if len(data) <= 8:
                continue
            fno = data[0] | (data[1] << 8)    # Header: Frame-Nr. (u16 LE), Byte 3 = Paket-Nr. im Frame
            pkt_no = data[3]
            if pkt_no == 0:                   # neuer Frame beginnt -> alten ausgeben
                if buf[:2] == b"\xff\xd8":
                    on_frame(bytes(buf))
                buf, frame_no = bytearray(), fno
                if fno % 50 == 0:
                    cmd.sendto(START, (HOST, CMD_PORT))   # periodischer Heartbeat
            if fno == frame_no:
                buf += data[8:]
    finally:
        cmd.sendto(STOP, (HOST, CMD_PORT))
```

### 5.7 Rotation im Client statt im Proxy

Der Proxy liefert JPEG unverändert als MJPEG und den Winkel per Server-Sent Events. Der Browser dreht.

```html
<img id="cam" src="/cams/soulear-1/mjpeg" style="transition: transform 80ms linear">
<script>
  const img = document.getElementById("cam");
  const ev = new EventSource("/cams/soulear-1/telemetry");
  ev.onmessage = (e) => {
    const t = JSON.parse(e.data);            // {"angle_deg": 123.4, "battery_pct": 87, ...}
    if (t.angle_deg != null) img.style.transform = `rotate(${-t.angle_deg}deg)`;
  };
</script>
```

Vorzeichen und eventuelle Zusatz-Offsets pro Gerät am echten Bild prüfen.

---

## 6. Nächste Schritte

1. **Eigenes Soulear-Gerät verifizieren:** SSID notieren, `tools/i4season_probe.py` aus
   king-cake/otoscope-windows gegen das Gerät laufen lassen (Devinfo, OpenVideo, LED).
2. **MAX-VIEW-Mikroskop identifizieren:** SSID + Kamera-IP notieren. Wenn `192.168.29.1` → Beispiel 5.6
   probieren. Sonst Mitschnitt per `rvi_capture`/Wireshark
   (`udp port 10900 or udp port 20000 or udp portrange 10005-10007`) → MaxSee- oder i4season-Familie?
3. **ZB-GW03-V1.3 prüfen:** Modul-Aufdruck und Flashgröße (`esptool.py flash_id`). Vor jedem Flashen
   die Original-Firmware sichern (`esptool.py read_flash`).
4. **Entscheidung Plattform:** zuerst Linux-Proxy (schneller iterierbar, Multi-Cam möglich), danach
   ggf. ESP32 als Einzel-Bridge?
5. Repo-Gerüst: Adapter-Interface (Telemetry-Modell aus 5.0), i4season-Adapter als erster (eigene
   Hardware), MJPEG-/SSE-Ausgabe.
6. Danach MaxSee-Adapter (zweites eigenes Gerät), dann EarFairy/JEGOAT/Xylla nach rbeilvert.

---

## 7. Vorsicht / bekannte Fehler aus der Recherche-Session

- **Falsche Zuordnung korrigiert:** In der Session wurde „Soulear = EarFairy (UDP 7099)“ angenommen und
  darauf ein ESP32-MVP gebaut. Laut verifizierter Doku spricht die Soulear/Hopefox Find T das
  **i4season-Protokoll (UDP 10005/10006/10007, Magic `0xFFEEFFEE`)**. EarFairy (`Cooleer_`) ist eine andere
  Familie. Den alten MVP-Prototyp nicht wiederverwenden.
- Der Prototyp nutzte außerdem einen **UDP-Port-Scan** zur Erkennung – das funktioniert so nicht verlässlich
  (siehe 4.2).
- Aussage „alle senden 640×480“ war zu pauschal (siehe Abschnitt 2, Auflösung).
- Ältere Notizen zu MaxSee (Heartbeat-Intervall, 1280×720) stammen aus Suchzusammenfassungen. Die Befehle
  in 5.6 sind dagegen aus dem czietz-Code; dieser ist von 2020 und für Windows (`msvcrt`) geschrieben.
- **Lizenz:** rbeilvert/otoscope, king-cake/otoscope, king-cake/otoscope-windows und rico001/open-web-soulear
  stehen unter **GPL**. Wer Code übernimmt (nicht nur Protokollwissen), muss das eigene Projekt ebenfalls
  unter GPL stellen. Protokollbeschreibungen nachzuimplementieren ist davon nicht betroffen.
  czietz/wifimicroscope ist BSD-2-Clause.

---

## 8. Hinweise für Claude in VS Code

- Arbeite in kleinen, kommentierten Schritten und erkläre, was passiert (Wunsch des Projektinhabers).
- Bei Änderungen an Code/YAML immer die **komplette Datei** ausgeben, keine Teilstücke zum Einfügen.
- Protokolldetails **nicht raten**: erst in den verlinkten Repos (Abschnitt 3) im Quellcode nachsehen,
  bei Widerspruch gilt die zuletzt am Gerät verifizierte Quelle (aktuell: `i4season-protocol.md` für Soulear).
- Die Beispiele in Abschnitt 5 sind Bausteine, keine getestete Bibliothek – beim Übernehmen Unit-Tests
  mit echten Mitschnitten (pcap / gespeicherte Pakete) dazuschreiben.
- Homelab-Kontext: Proxmox-Cluster mit Debian-VMs, Home Assistant, MagicMirror² mit eigenen Modulen –
  spätere Integrationen bitte dorthin denken, aber erst nach einem funktionierenden Proxy-Kern.