# HANDOVER – Multi-cam proxy for cheap Wi-Fi otoscopes & microscopes

> Handover from a research session with Claude (as of: 2026-10-01).
> Purpose: starting point for further work in VS Code with Claude.
> **Not carried over** was the ESP32 MVP prototype created in the session (adapter, detector,
> `main.cpp`, `platformio.ini`), because it was based on a wrong protocol assignment (see section 7).
> The **code examples in section 5**, on the other hand, are new and derived directly from the source
> code or the verified documentation of the reference repos.
>
> Since then the ESP32 firmware in this repo implements the i4season and MaxSee families; see the
> [README](../README.md).

---

## 1. The idea

Cheap Wi-Fi otoscopes, ear-cleaner cameras, endoscopes and Wi-Fi microscopes all open their own
Wi-Fi and speak proprietary but **unencrypted UDP protocols**. Each vendor needs its own, mostly
Chinese app (some with a cloud licence check, see 3.1).

**Goal:** a **multi-cam proxy** that

1. detects cameras nearby (Wi-Fi scan for SSID prefix **and** BLE scan),
2. connects to the respective camera,
3. speaks the matching protocol via an **adapter/vendor module**,
4. outputs everything **uniformly**: MJPEG over HTTP + telemetry (orientation angle, battery, light) as JSON/SSE,
5. works without the vendor app and without the cloud.

Audience: own homelab use, in perspective maker labs (one device that can show "all the cheap stuff").

### Own devices (test hardware)

| Device | App | Presumed family | Status |
|---|---|---|---|
| Hopefox ear cleaner (Soulear app) | Soulear | **i4season** (10005/10006) | docs verified on a *Hopefox Find T* by king-cake, still to be checked on the own device |
| Zoom camera/microscope | MAX-VIEW 1.0.12(46), iOS | i4season app, protocol **unclear** (MaxSee 10900/20000 or i4season) | check via capture |
| Tuya ZB-GW03-V1.3 (ESP32) | – | candidate for proxy hardware | verify flash/chip on the device |

---

## 2. Known camera families (protocol overview)

Sources in section 3. "verified" = read in the source code or the documentation of the repo.

| Family | Detection | Control | Video | Orientation/battery/light | Source | Status |
|---|---|---|---|---|---|---|
| **i4season / Soulear** | SSID `Soulear-…` (app prefixes also `i4season`, `SUEAR`, `inskam`, `Yanxuan`); broadcast to `192.168.1.255:10005` | UDP 10005 (req/resp), 10006 (OpenVideo), status push to client port 10007 | UDP to a self-chosen `picport`, JPEG in chunks, header 16 B (type 1) / 28 B (type 6) | accel packed (3× 9 bits + sign) in the video header; battery in devinfo and status push; LED via cmd `0x000A` | king-cake/otoscope-windows `docs/i4season-protocol.md`, rico001/open-web-soulear | verified (docs) |
| **EarFairy** | SSID prefix `Cooleer_` (Wi-Fi scan) | UDP 7099 (heartbeat `01 01` every 1 s) | **RTSP** `rtsp://<ip>:7070/webcam` (RTP/JPEG) | angle bytes 0–1, battery byte 2 (101 = charging), flip flag byte 7; LED `05 02` on / `05 01` off | rbeilvert/otoscope | verified (code) |
| **JEGOAT** | **BLE** advert with BSSID; WPA2 passphrase = BSSID as hex (lower case) | UDP 61500 (`10 01` every 5 s → JSON telemetry) | UDP 61501: start `20 01`, stop `20 02`, keepalive 1 s; 24-B header, float32 angle at offset 3 | angle in the video header, battery via JSON | rbeilvert/otoscope | verified (code) |
| **Xylla** | **BLE** advert, manufacturer data `66 99` + BSSID | UDP 50000, magic `0x9999`: `0x1017` battery, `0x1060` board info, `0x1002` version | UDP 8032: cmd 1 = start preview (11× burst, then every 800 ms), cmd 2 = stop; 24-B header, magic `0x66` | accel 3×10 bits at offset 16 in the video header | rbeilvert/otoscope | verified (code) |
| **iTiMO / jetion** | BLE name `iTiMO-` / `jetion_` (check before Xylla, same BLE magic) | UDP 50000 | UDP 8031/8032 | like Xylla | rbeilvert/otoscope, raunak51299/Endoscope-Hacking | verified (code) |
| **MaxSee / JoyHonest ("JHCMD")** | SSID e.g. `Maxsee_xxxx`, camera fixed at `192.168.29.1` | UDP 20000: `JHCMD` + 2 bytes (`10 00`, `20 00` init; `d0 01` heartbeat/start; `d0 02` stop) | UDP 10900 (client binds), 8-B header, JPEG | not known | czietz/wifimicroscope (code), chzsoft.de | verified (code, as of 2020) |
| **Borescope (TCP)** | – | – | TCP 7060, own frame format | – | mplough.github.io, n8henrie.com | from research |
| i4season sub-families B, C, Novatek HTTP | – | B: 44506/52219/52220; C: 6080/6090; Novatek: HTTP `192.168.1.254` | – | – | end of `i4season-protocol.md` | only mentioned, not analysed |

**Resolution, realistically:** the Hopefox Find T delivers **480×480 JPEGs** although the header reports 640×480
(~17 fps, ~12–14 chunks of ≤1400 B, ~7 % frame loss over Wi-Fi). MaxSee microscopes are said to deliver
1280×720 according to research. Amazon claims like "1080p/5 MP" usually refer to the sensor or are marketing.
Measure per device, do not assume.

---

## 3. Links – projects & sources

All GitHub repos checked for existence via `git ls-remote` on 2026-10-01 (✅).
Gists could not be checked from the session environment (❔).

### 3.1 Core references (otoscopes)

- ✅ **[rbeilvert/otoscope](https://github.com/rbeilvert/otoscope)** – Android app, **the** reference for
  the multi-vendor approach: `vendor/CameraVendor.kt` + `CameraVendors` registry, discovery via BLE **or**
  Wi-Fi scan, sessions for EarFairy, JEGOAT, Xylla, iTiMO. GPL. Last commit in the clone: 2026-09-15.
  - [Issue #38 – i4season/Soulear support](https://github.com/rbeilvert/otoscope/issues/38)
  - [Issue #41 – AIR-ES-xxxxxx devices](https://github.com/rbeilvert/otoscope/issues/41)
- ✅ **[king-cake/otoscope](https://github.com/king-cake/otoscope)** – fork of rbeilvert with Soulear/i4season support,
  [release v0.7.0-soulear.1](https://github.com/king-cake/otoscope/releases/tag/v0.7.0-soulear.1). GPL.
- ✅ **[king-cake/otoscope-windows](https://github.com/king-cake/otoscope-windows)** – Python/Windows viewer.
  **`docs/i4season-protocol.md`** = best protocol description for Soulear (from `libWifiCamera.so` via
  Ghidra, verified on a Find T on 2026-09-30). `tools/i4season_probe.py` = test client using only the
  Python standard library. GPL.
- ✅ **[rico001/open-web-soulear](https://github.com/rico001/open-web-soulear)** – Node.js/TypeScript backend
  + React frontend, camera → UDP → backend → HTTP/browser. German documentation including a **static analysis
  of the Soulear app** (`docs/README-statische-analyse.md`: cloud licence report `yun.simicloud.com`, possibility
  of blocking via server, SSID prefixes). Architecturally closest to the proxy idea. GPL.
- ✅ **[The-Dorkknight/earscope-app](https://github.com/The-Dorkknight/earscope-app)** – ear scope app with
  its own protocol explanation: [HOW_IT_WORKS.md](https://github.com/The-Dorkknight/earscope-app/blob/main/HOW_IT_WORKS.md)
- ✅ **[raunak51299/Endoscope-Hacking](https://github.com/raunak51299/Endoscope-Hacking)** – iTiMO endoscope,
  UDP 8031/50000, battery `0x1017`.

### 3.2 Wi-Fi microscopes (MaxSee / MAX-VIEW / JoyHonest)

- ✅ [czietz/wifimicroscope](https://github.com/czietz/wifimicroscope) – original PoC for the chzsoft article (BSD-2-Clause), basis for example 5.6
- ✅ [loehnertj/maxsee_viewer](https://github.com/loehnertj/maxsee_viewer) – viewer for MaxSee
- ✅ [slofo82/MaxSee_wifiMicroscope](https://github.com/slofo82/MaxSee_wifiMicroscope) – C# implementation
- ✅ [fbetancourt-dev/microscope-viewer](https://github.com/fbetancourt-dev/microscope-viewer) – Linux viewer, JoyHonest MS5B
- 📄 [CHZ-Soft: Reverse-engineering a Wifi microscope](https://www.chzsoft.de/site/hardware/reverse-engineering-a-wifi-microscope/) – ports 20000/10900, `JHCMD`, methodology
- 📄 [Hackaday: Reverse Engineering a Wifi Microscope MS5](https://hackaday.io/project/206057-reverse-engineering-a-wifi-microscope-ms5)
- ❔ [Gist TheCrazyT: wifi_microscope_dump.py](https://gist.github.com/TheCrazyT/364ff5d6e893905af9d950f70daa2f29)
- ❔ [Gist sspathak: adapted czietz viewer](https://gist.github.com/sspathak/33cc4bf69cc642eee067dd4eb93b9032)
- App stores: [MAX-VIEW iOS](https://apps.apple.com/us/app/max-view/id1629406651) ·
  [Max-see iOS (old)](https://apps.apple.com/us/app/max-see/id1387691074) ·
  [MAX-VIEW Android `com.i4season.maxview`](https://play.google.com/store/apps/details?id=com.i4season.maxview)

### 3.3 Other endoscope/camera projects

- ✅ [hypeapps/Endoscope](https://github.com/hypeapps/Endoscope) – Wi-Fi endoscope app
- ✅ [hardcodedjoy/udp-camera-iototoy](https://github.com/hardcodedjoy/udp-camera-iototoy) – simple UDP camera streaming
- ✅ [Kosmonova/esp32s3-uvc](https://github.com/Kosmonova/esp32s3-uvc) – ESP32-S3 + USB endoscope (UVC), interesting for an ESP32 variant
- ❔ [Gist gitfvb: Notes on Quelima R3 WiFi Camera](https://gist.github.com/gitfvb/09085fd0cd4993549feb7470430d40e9) – different family (embedded Linux, HTTP 8080)
- 📄 [mplough: Rewriting the video stream from a wi-fi borescope](https://mplough.github.io/2019/12/14/borescope.html) – TCP 7060, JPEG repair (DRI marker)
- 📄 [n8henrie: Reverse Engineering My WiFi Endoscope, Part 4](https://n8henrie.com/2019/02/reverse-engineering-my-wifi-endoscope-part-4/)

### 3.4 Tools for your own reverse engineering

- ✅ [gh2o/rvi_capture](https://github.com/gh2o/rvi_capture) – capture iPhone traffic without a Mac (for MAX-VIEW iOS)
- 📄 [iOS Packet Capture Tutorial (rvictl)](https://nickhuangcyh.github.io/blog/tools/how-to-capture-network-packet-on-ios/)
- 📄 [Go deep on iOS packet analysis](https://medium.com/@MikeFurtak/go-deep-on-ios-packet-analysis-6a7542eeffb3)
- Android route (easier): APK → `jadx`, native `.so` → Ghidra (that is how `i4season-protocol.md` was created)

---

## 4. Architecture sketch (proposal, nothing decided yet)

### 4.1 Most important constraint

**Every camera is its own access point** (usually `192.168.1.1` or `192.168.29.1`, often the same IP!).
A Wi-Fi interface in client mode can only be connected to **one** AP at a time. It follows:

- **ESP32 (ZB-GW03)** → realistically **one camera at a time** (switchable). Advantage: has BLE for the
  JEGOAT/Xylla/iTiMO discovery. Disadvantage: little RAM, no sensible image rotation via JPEG re-encoding.
- **Linux host (Debian VM in the Proxmox cluster with passed-through USB Wi-Fi sticks)** → **several cameras
  at once**: one stick per camera, each in its own network namespace (because of identical camera IPs).
  That is the actual "multi" proxy.
- Both variants can have the same **output interface** → clients (browser, MagicMirror module,
  Home Assistant later) notice no difference.

### 4.2 Building blocks

```
Discovery            Session/adapter (per family)           Output
─────────            ────────────────────────────           ──────
Wi-Fi scan (SSID) ─┐  i4season · EarFairy · JEGOAT ·        /cams                   (list, JSON)
BLE scan (advert) ─┼─▶ Xylla · iTiMO · MaxSee · …     ─▶    /cams/<id>/mjpeg        (video)
Manual (IP)       ─┘  connect · keepalive · frame           /cams/<id>/telemetry    (SSE: angle, battery)
                      reassembly · telemetry                /cams/<id>/led          (control)
```

- **Detection via SSID prefix / BLE advert instead of a port scan.** UDP ports do not reliably answer test
  packets. rbeilvert does the same (`CameraVendor.parseAdvert` / SSID filter); the order of the vendors
  matters (iTiMO before Xylla). For unknown SSIDs, targeted probes per family as a fallback
  (e.g. i4season `DevinfoGet` via broadcast to :10005).
- **Do not compute the rotation in the proxy**, but deliver the angle and rotate it in the client via CSS/canvas
  (example 5.7). That avoids JPEG re-encoding, which is crucial on the ESP32.
- **Mind the keepalives:** i4season sends OpenVideo again every second if no video arrived; EarFairy
  needs a heartbeat every second, JEGOAT a video keepalive 1 s + control 5 s, Xylla 800 ms,
  MaxSee a periodic `JHCMD d0 01`.
- Build **frame reassembly** robustly: sequence gaps lead to dropping the frame, JPEG must start with
  `FF D8`; chunks end with zero padding after `FF D9`.

### 4.3 Language/stack (open)

- Linux proxy: Python (asyncio) or Node.js/TypeScript (fits rico001 and own MagicMirror modules).
- ESP32: ESP-IDF/Arduino with PlatformIO; only families that get by without a large buffer.

---

## 5. Code examples (orientation sensor, battery, light, keepalive)

All examples: **Python 3, standard library only**, meant as building blocks (not a finished program).
Each block names its source. They are own re-implementations of the protocol descriptions, not copied
GPL code. Check on your own device before use.

### 5.0 Common data model

Every adapter delivers the same format to the output layer (section 4.2).

```python
from dataclasses import dataclass
from typing import Optional

@dataclass
class Telemetry:
    angle_deg: Optional[float] = None   # rotation around the long axis, already incl. mounting offset
    battery_pct: Optional[int] = None   # 0..100
    charging: Optional[bool] = None
    led_on: Optional[bool] = None       # last known/set state
```

### 5.1 i4season / Soulear (Hopefox Find T) – UDP 10005/10006/10007

Source: king-cake/otoscope-windows, `docs/i4season-protocol.md` + `tools/i4season_probe.py`.

```python
import math, socket, struct, time

MAGIC = 0xFFEEFFEE
CMD_PORT, VIDEO_CMD_PORT, NOTIFY_PORT = 10005, 10006, 10007
CMD_DEVINFO, CMD_OPEN_VIDEO, CMD_LED = 0x0001, 0x0004, 0x000A

# --- Control packet: 12-byte header (little-endian) + payload --------------------
# magic(u32) seq(u16) cmd(u16) 0x01(u8, "request") status(u8) len(u16) payload
def build_request(seq: int, cmd: int, payload: bytes = b"") -> bytes:
    return struct.pack("<IHHBBH", MAGIC, seq & 0xFFFF, cmd, 1, 0, len(payload)) + payload

def request(cam_ip: str, seq: int, cmd: int, payload=b"", port=CMD_PORT, retries=20):
    """Send + wait for a reply with the same magic/seq (like the vendor app: 20× 100 ms)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)  # allows discovery to 192.168.1.255
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
                return addr[0], status, data[12:]   # sender IP = camera IP
        return None
    finally:
        s.close()

# --- Battery from DevinfoGet (payload 0x80 bytes) ----------------------------------
def battery_from_devinfo(p: bytes) -> int:
    status = struct.unpack_from("<H", p, 0x77)[0]
    return status >> 9            # bits 9..15 = battery in %, observed: 0xC801 -> 100 %

# --- Light (LED) ---------------------------------------------------------------
# Payload: op, status, brightness. op = LED no. 1 | 0x10 (= write)
# status: 0 off, 1 on, 2 blink, 3 breathe. Find T: brightness is stored, but the LED does not dim.
def led_payload(on: bool) -> bytes:
    return bytes([0x11, 1, 100]) if on else bytes([0x11, 0, 0])
# Usage: request(cam_ip, seq, CMD_LED, led_payload(True))   # reply = resulting state

# --- Open video (+ keepalive) ----------------------------------------------------
# picport = local UDP port on which we receive video; client_id = time() once per session.
def open_video_payload(picport: int, client_id: int) -> bytes:
    return struct.pack("<HHI", picport, 0, client_id)
# Keepalive: if no video packet came for > 1 s, simply send OpenVideo to port 10006 again.

# --- Orientation sensor from the video header ------------------------------------
# Byte 0 = type (1 -> 16-byte header, 6 -> 28-byte header), byte 5 bit0 = "has G-sensor",
# bytes 6..9 = packed accelerometer: per axis 9 bits magnitude + 1 sign bit.
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
        return None                          # the vendor app then keeps the old angle
    angle = math.degrees(math.atan2(y, z)) % 360   # rotation around the long axis (x)
    if "FBPRO" in product or "R1" in product:      # Find T reports "BK7231U-XRH-FBPRO"
        angle = (angle + 180) % 360
    if "w50" in product:
        angle = (angle + 90) % 360
    return angle

# --- Status push on UDP 10007 (about 1×/s, without request) ------------------------
# 29-byte packet, cmd 0x0009, payload type 0x02; payload byte 1 >> 1 = battery in %.
def battery_from_status_push(pkt: bytes) -> int | None:
    payload = pkt[12:]
    if len(payload) >= 2 and payload[0] == 0x02:
        return payload[1] >> 1
    return None
```

### 5.2 EarFairy (`Cooleer_…`) – UDP 7099 + RTSP

Source: rbeilvert/otoscope, `stream/earfairy/EarFairyControlClient.kt`, `EarFairyVideoClient.kt`.

```python
EARFAIRY_CTRL_PORT = 7099
EARFAIRY_RTSP_URL = "rtsp://{ip}:7070/webcam"   # video runs over RTSP/RTP (JPEG), e.g. via ffmpeg

HEARTBEAT = b"\x01\x01"        # send every 1 s; otherwise telemetry stops after ~3 s
LED_ON, LED_OFF = b"\x05\x02", b"\x05\x01"   # fire-and-forget, no confirmation -> remember the state yourself

MOUNT_OFFSET_DEG = 90          # Y-201 hardware: sensor zero is rotated 90° against the lens

class EarFairyDecoder:
    def __init__(self):
        self.flip_latched = False      # byte 7 == 1 is set once at boot -> remember for the session

    def decode(self, buf: bytes) -> Telemetry | None:
        if len(buf) < 4:
            return None
        low = buf[1] - 256 if buf[1] > 127 else buf[1]          # byte 1 is signed
        raw = low + 255 if (buf[0] == 1 or low < 0) else low      # byte 0 = "high bit"
        if len(buf) >= 8 and buf[7] == 1:
            self.flip_latched = True
        angle = (raw + MOUNT_OFFSET_DEG + (90 if self.flip_latched else 0)) % 360

        b = buf[2]                                                # 1..100 = %, 101 = charging
        if b == 101:
            return Telemetry(angle_deg=angle, battery_pct=100, charging=True)
        return Telemetry(angle_deg=angle, battery_pct=min(max(b, 0), 100), charging=False)

# Byte 3 = device family (0x5A), byte 4 = trigger button ('M' photo, 'X' video) – useful for
# a "photo" button on the device that triggers a snapshot in the proxy.
```

### 5.3 JEGOAT – BLE discovery, UDP 61501 (video) / 61500 (telemetry)

Source: rbeilvert/otoscope, `vendor/JegoatVendor.kt`, `stream/jegoat/JegoatSession.kt`, `FrameAssembler.kt`.

```python
import json, struct

VIDEO_PORT, CTRL_PORT = 61501, 61500
VIDEO_START, VIDEO_STOP = b"\x20\x01", b"\x20\x02"   # start 10× as a burst, then every 1 s as keepalive
CTRL_POLL = b"\x10\x01"                              # every 5 s -> reply contains JSON

# Wi-Fi access from the BLE advert: payload bytes 6..11 = BSSID; passphrase = BSSID as hex, lower case
def jegoat_wifi_credentials(ble_payload: bytes):
    bssid = ble_payload[6:12]
    return ":".join(f"{b:02X}" for b in bssid), bssid.hex()

# Video header (24 bytes): frame_id, chunk_seq (0 = first), chunks_total, float32 angle (LE), 17 B padding
def parse_jegoat_chunk(pkt: bytes):
    frame_id, chunk_seq, chunks_total = pkt[0], pkt[1], pkt[2]
    angle = struct.unpack_from("<f", pkt, 3)[0]       # already in degrees, no conversion needed
    return frame_id, chunk_seq, chunks_total, angle, pkt[24:]
# Reassembly: sort chunks by chunk_seq (UDP may reorder); drop incomplete frames.

def parse_jegoat_telemetry(reply: bytes) -> Telemetry | None:
    text = reply.decode("utf-8", "replace")
    start = text.find("{")                            # a few echo bytes precede the JSON
    if start < 0:
        return None
    obj = json.loads(text[start:])
    pct = obj.get("battery_percentage")
    return Telemetry(battery_pct=pct if isinstance(pct, int) and 0 <= pct <= 100 else None,
                     charging=bool(obj.get("battery_charging", False)))
    # further fields: device_chip, device_version, width, height, fps, debug_rssi
```

### 5.4 Xylla / iTiMO – UDP 8032 (video) / 50000 (control)

Source: rbeilvert/otoscope, `stream/xylla/XyllaControlClient.kt`, `XyllaCameraClient.kt`,
`FrameAssembler.kt`, `RotationFilter.kt`.

```python
import math, statistics, struct
from collections import deque

CMD_START_PREVIEW, CMD_STOP_PREVIEW = 1, 2        # to UDP 8032: 11× burst, then every 800 ms
CMD_GET_BATTERY, CMD_GET_BOARD_INFO, CMD_GET_VERSION = 0x1017, 0x1060, 0x1002   # to UDP 50000

# Command packet (24 bytes, LE): magic 0x9999, cmd(u16), seq(u32), 16 zero bytes.
# The reply starts with the same magic + cmd -> match by that; up to 4 attempts of 600 ms each.
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

# Orientation sensor: video header (24 B, magic 0x66) bytes 16..19 = 3×10 bits, 512 = zero point.
# With noise filter: only update when the device moves (std dev > 3 over 20 samples).
class XyllaRotationFilter:
    WINDOW = 20

    def __init__(self):
        self.hist = [deque(maxlen=self.WINDOW) for _ in range(3)]
        self.last_angle = 0.0

    def update(self, accel: int) -> float:
        reg = [(accel >> 20) & 0x3FF, (accel >> 10) & 0x3FF, accel & 0x3FF]   # X, Y, Z
        mag = [1024 - r if r >= 512 else r for r in reg]                       # mirror at 512
        for h, m in zip(self.hist, mag):
            h.append(m)
        if len(self.hist[0]) < self.WINDOW:
            return self.last_angle
        if not any(statistics.pstdev(h) > 3 for h in self.hist):
            return self.last_angle            # device lies still -> do not let the image "wander"
        reg_y, reg_z = reg[1], reg[2]
        mag_y, mag_z = mag[1], mag[2]
        if mag_z == 0:
            return self.last_angle
        theta = math.atan(mag_y / mag_z)
        if reg_z > 512:
            theta = math.pi - theta
        if reg_y > 512:
            theta = 2 * math.pi - theta
        theta = (theta + math.pi) % (2 * math.pi)   # the lens is mounted at 180° to the sensor
        self.last_angle = math.degrees(theta)
        return self.last_angle
```

### 5.5 Telemetry differences at a glance

| Family | Angle comes from | Battery comes from | Light |
|---|---|---|---|
| i4season | video header bytes 6..9 (only if flag bit 0) | devinfo `0x77` and push on 10007 | cmd `0x000A`, with confirmation |
| EarFairy | telemetry on 7099, bytes 0..1 | telemetry byte 2 | `05 02` / `05 01`, without confirmation |
| JEGOAT | video header float32 at offset 3 | JSON on 61500 | not known |
| Xylla/iTiMO | video header bytes 16..19 (+ filter) | cmd `0x1017` on 50000 (actively queried) | not known |
| MaxSee | – | – | not known (microscope has a hardware dimmer) |

### 5.6 MaxSee / JoyHonest – UDP 20000 (commands) / 10900 (video)

Source: czietz/wifimicroscope, `wifi_microscope_dump.py` (BSD-2-Clause, as of 2020).

```python
import socket

HOST, CMD_PORT, VIDEO_PORT = "192.168.29.1", 20000, 10900   # the camera's IP is fixed

INIT = [b"JHCMD\x10\x00", b"JHCMD\x20\x00"]   # like naInit_Re() in the vendor library
START = b"JHCMD\xd0\x01"                      # heartbeat = starts/keeps the data stream
STOP = b"JHCMD\xd0\x02"

def run(on_frame):
    cmd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for c in INIT + [START, START]:
        cmd.sendto(c, (HOST, CMD_PORT))
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("", VIDEO_PORT))                 # the camera sends to the fixed port 10900
    rx.settimeout(1.0)
    buf, frame_no = bytearray(), None
    try:
        while True:
            try:
                data = rx.recv(1450)
            except socket.timeout:
                cmd.sendto(START, (HOST, CMD_PORT))   # stream fell asleep -> kick it again
                continue
            if len(data) <= 8:
                continue
            fno = data[0] | (data[1] << 8)    # header: frame no. (u16 LE), byte 3 = packet no. in frame
            pkt_no = data[3]
            if pkt_no == 0:                   # new frame starts -> output the old one
                if buf[:2] == b"\xff\xd8":
                    on_frame(bytes(buf))
                buf, frame_no = bytearray(), fno
                if fno % 50 == 0:
                    cmd.sendto(START, (HOST, CMD_PORT))   # periodic heartbeat
            if fno == frame_no:
                buf += data[8:]
    finally:
        cmd.sendto(STOP, (HOST, CMD_PORT))
```

### 5.7 Rotation in the client instead of the proxy

The proxy delivers JPEG unchanged as MJPEG and the angle via server-sent events. The browser rotates.

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

Check the sign and any additional offsets per device against the real image.

---

## 6. Next steps

1. **Verify the own Soulear device:** note the SSID, run `tools/i4season_probe.py` from
   king-cake/otoscope-windows against the device (devinfo, OpenVideo, LED).
2. **Identify the MAX-VIEW microscope:** note SSID + camera IP. If `192.168.29.1` → try example 5.6.
   Otherwise capture via `rvi_capture`/Wireshark
   (`udp port 10900 or udp port 20000 or udp portrange 10005-10007`) → MaxSee or i4season family?
3. **Check the ZB-GW03-V1.3:** module marking and flash size (`esptool.py flash_id`). Before every flash,
   back up the original firmware (`esptool.py read_flash`).
4. **Platform decision:** Linux proxy first (faster to iterate, multi-cam possible), then
   possibly the ESP32 as a single bridge?
5. Repo scaffold: adapter interface (telemetry model from 5.0), i4season adapter first (own
   hardware), MJPEG/SSE output.
6. Then the MaxSee adapter (second own device), then EarFairy/JEGOAT/Xylla following rbeilvert.

---

## 7. Caution / known errors from the research session

- **Wrong assignment corrected:** the session assumed "Soulear = EarFairy (UDP 7099)" and built an
  ESP32 MVP on it. According to the verified documentation the Soulear/Hopefox Find T speaks the
  **i4season protocol (UDP 10005/10006/10007, magic `0xFFEEFFEE`)**. EarFairy (`Cooleer_`) is a different
  family. Do not reuse the old MVP prototype.
- The prototype also used a **UDP port scan** for detection – that does not work reliably
  (see 4.2).
- The statement "all send 640×480" was too sweeping (see section 2, resolution).
- Older notes on MaxSee (heartbeat interval, 1280×720) come from search summaries. The commands
  in 5.6, on the other hand, are from the czietz code; it is from 2020 and written for Windows (`msvcrt`).
- **Licence:** rbeilvert/otoscope, king-cake/otoscope, king-cake/otoscope-windows and rico001/open-web-soulear
  are under the **GPL**. Anyone taking over code (not just protocol knowledge) must put their own project
  under the GPL as well. Re-implementing protocol descriptions is not affected by this.
  czietz/wifimicroscope is BSD-2-Clause.

---

## 8. Notes for Claude in VS Code

- Work in small, commented steps and explain what happens (wish of the project owner).
- For changes to code/YAML always output the **complete file**, no fragments to paste.
- **Do not guess** protocol details: first look in the source code of the linked repos (section 3),
  in case of contradiction the source last verified on the device wins (currently: `i4season-protocol.md` for Soulear).
- The examples in section 5 are building blocks, not a tested library – when adopting them, write unit tests
  with real captures (pcap / stored packets).
- Homelab context: Proxmox cluster with Debian VMs, Home Assistant, MagicMirror² with own modules –
  please think of later integrations there, but only after a working proxy core.
