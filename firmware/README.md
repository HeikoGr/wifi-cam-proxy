# Firmware: WiFi-Cam-Proxy on ZB-GW03 v1.4 / WT32-ETH01 (and CYD)

The ZB-GW03 (ESP32 + LAN8720, originally a Zigbee gateway) connects to a camera via Wi-Fi and delivers the image into the home network over Ethernet. The camera is recognised by its Wi-Fi name, see [Cameras](#cameras). For otoscopes with an orientation sensor, the browser offers an orientation correction that rotates the image along when the probe is turned. The CYD variant shows the image on its own display instead (`src/main_cyd.cpp`, see the [project README](../README.md)).

As of 2026-09-30: stable at 17 fps, also with a viewer. Dropouts only occur when the probe's Wi-Fi signal gets weak (from about −70 dBm).

## Usage

| Address | Purpose |
|---|---|
| `http://otoskop.local/` | live image with orientation correction, 2× zoom, battery, LED and snapshot |
| `/cameras` | cameras found, selection, rescan (JSON: `/cameras.json`) |
| `/stream` | MJPEG for VLC or Home Assistant (unrotated) |
| `/snapshot` | current single frame (JPEG) |
| `/calibrate` | calibrate orientation: circle recording, quarter turns, zero point, smoothing |
| `/update` | status, Wi-Fi mode, Ethernet speed, firmware update, restart |
| `/status` | all counters as JSON |
| `/camdiag`, `/camdiag/raw` | first packets of the camera session as hex; raw capture of one frame (call twice) |
| `/wifi-setup` | set up the home Wi-Fi for rescue mode (also via the own access point) |

![Live view](../docs/screenshots/live.png)

The screenshots in this file come from the [UI preview](../tools/ui-preview/) (simulated data, generated test image).

## Cameras

The firmware looks for cameras with a Wi-Fi scan. The name patterns are in `SSID_PATTERNS` in [src/camera.cpp](src/camera.cpp):

1. The last connected camera (NVS `cam_ssid`) is contacted directly at startup without a scan.
2. If it cannot be reached, a scan runs. If exactly one recognised, open camera is in range, that one is taken.
3. If several are in range, the start page shows a hint and you pick one under `/cameras`. Until then a new scan runs every 20 s.

![Camera selection](../docs/screenshots/cameras.png)

Under `/cameras` you can also choose an unknown network. With the protocol "automatic" the rule is: gateway `192.168.29.1` always means MaxSee/JHCMD, otherwise the SSID pattern decides, else i4season. Each network row has its own protocol choice and a "Connect"/"Reconnect" button. "Automatic scan" can be switched off: the device then only reconnects to the remembered camera and scans only on "Rescan". The selectable protocols come from `PROTO_CHOICES` in `src/camera.cpp`, so a new protocol shows up in the web UI and on the CYD by itself. A camera password is possible as well.

| Protocol | File | Video | Extras |
|---|---|---|---|
| i4season | [src/cam_i4season.cpp](src/cam_i4season.cpp) | GetDeviceInfo :10005, START :10006, 16/28-byte header | orientation sensor (if the header flag is set), battery from devinfo and status push :10007, LED (`0x0A`, payload `11 01 64` / `11 00 00`) |
| JHCMD (MaxSee, MAX-VIEW) | [src/cam_jhcmd.cpp](src/cam_jhcmd.cpp) | `JHCMD` to :20000, video to the fixed port 10900, 8-byte header; packets are sorted by number (they arrive out of order) | – |

Only the session of the active camera occupies RAM, the protocol code lives in flash.

LEDs: **green** means the firmware is running. **Red** means rescue mode.

## Configuration

- [include/config.h](include/config.h): pins, timeouts, defaults
- `include/secrets.h` (template [secrets.example.h](include/secrets.example.h)), optional: `OTA_PASSWORD`, `SETUP_AP_PASSWORD`, home Wi-Fi as a default
- **NVS** (namespace `otoskop`) stores runtime settings. They survive restarts and firmware updates:

| Key | Content | Change via |
|---|---|---|
| `calib` | orientation calibration (JSON) | `/calibrate` → "Save on device" |
| `cam_ssid`, `cam_pass`, `cam_proto` | last connected camera | `/cameras` |
| `wifimode` | `bgn`, `bg` or `b` | `/update` → "Wi-Fi to the camera" |
| `wifitx` | Wi-Fi transmit power in 0.25 dBm | `POST /wifi/tx/<8..84>` |
| `eth10` | Ethernet 10 Mbit only (default: on for ZB-GW03, off for WT32-ETH01) | `/update` or `POST /eth10/<0\|1>` |
| `home_ssid`, `home_pass` | home Wi-Fi for rescue mode | `/wifi-setup` |

Back up and restore the calibration:

```
curl http://otoskop.local/calibration > calibration.json
curl -H "Content-Type: application/json" --data-binary @calibration.json http://otoskop.local/calibration
```

## Building

```
pio run -e zb-gw03
```

The [platformio.ini](platformio.ini) uses **pioarduino** (Arduino core 3.x on ESP-IDF 5.5) with `custom_sdkconfig`. This rebuilds the Arduino libraries with custom ESP-IDF settings:

| Setting | Value | Reason |
|---|---|---|
| `CONFIG_BT_ENABLED` | `n` | Bluetooth is not used. Saves ~14 KB of IRAM, where the frame data lives, and RAM |
| `CONFIG_LWIP_UDP_RECVMBOX_SIZE` | `32` (instead of 6) | The otoscope sends the 15–30 packets of a frame as a burst. With 6 the buffer overflowed |

If `custom_sdkconfig` changes, the next build rebuilds ESP-IDF. That takes about 4 minutes, afterwards it is fast again.

## Updating the firmware

- **In the browser:** `http://otoskop.local/update`, then choose `.pio/build/zb-gw03/firmware.bin` (not `firmware.factory.bin`).
- **On the command line:** `pio run -e zb-gw03-http -t upload`, which corresponds to `curl --data-binary @firmware.bin http://otoskop.local/update`.
- **Via espota:** `pio run -e zb-gw03-ota -t upload`.

If `OTA_PASSWORD` is set, it applies to all routes. With curl you pass it as the header `X-OTA-Password`. During an update some video packets are lost briefly because the flash is being written. That is normal.

## Rescue mode

If Ethernet has no IP for 30 s, the red LED turns on and the device switches to rescue mode:

1. If a home Wi-Fi is set up, it connects to it. The web UI and OTA then stay reachable under `otoskop.local`. You set the home Wi-Fi under `/wifi-setup`, it is then stored in NVS. As a fallback the device uses `HOME_WIFI_SSID` from `secrets.h`.
2. If none is set up or it cannot be reached for 30 s, the device opens its own access point `WiFi-Cam-XXXX` (password `SETUP_AP_PASSWORD`, default `wificam-setup`). After connecting, the phone opens the setup page by itself (captive portal), otherwise open `http://192.168.4.1/wifi-setup`. There you scan for networks and store the home Wi-Fi; the device connects right away. While the AP is running, it retries the home Wi-Fi every 5 minutes as long as nobody is connected to the AP.
3. Once Ethernet has been back stably for 10 s, the device restarts into normal operation.

The camera is idle in rescue mode because Wi-Fi is then needed for reachability.

![Wi-Fi setup in rescue mode](../docs/screenshots/wifi-setup-rescue.png)

## Emergency via USB-UART (3.3 V)

1. Open the case and connect the adapter to TX, RX, GND and 3V3 (TX↔RX crossed).
2. Pull GPIO0 to GND at power-on so the ESP32 starts in the bootloader.
3. Flash with `pio run -e zb-gw03 -t upload --upload-port /dev/ttyUSB0`.
4. Serial output with `pio device monitor`.

The switch from ESPHome was done via OTA through the ESPHome port: `esphome.espota2.run_ota('zb-gw03.local', 3232, None, Path('.pio/build/zb-gw03/firmware.bin'))`.

## What is in the code and why

The measures were measured on the device, with captures on the home network side. Each has a comment in the code ([src/main.cpp](src/main.cpp), [src/cam_i4season.cpp](src/cam_i4season.cpp), [src/frame.cpp](src/frame.cpp)).

| Problem | Cause | Fix |
|---|---|---|
| No video despite START confirmation | The otoscope needs GetDeviceInfo first | GetDeviceInfo and START from the same socket |
| Crash while streaming | `new` threw `bad_alloc` with little heap | Frames via `malloc`, on shortage the frame is dropped |
| "Busy", frames dropped for memory | `getFreeHeap()` includes 44 KB of IRAM that is not normally usable. Really free was often only ~11 KB | Frames are stored as a packet list in the **IRAM remainder**, read and written word by word. The display now shows only real memory |
| Lost Wi-Fi packets | UDP buffer only 6 packets, `WiFi.RSSI()` in the video task slowed it down | Buffer 32, no RSSI calls in the video task any more, video task with priority 10 |
| Blocking on packet loss | 802.11n aggregates packets, one missing part holds up the whole block | Wi-Fi mode `bg` (without 11n) |
| **Stutter: 2–3 % Ethernet loss during Wi-Fi reception** | The ESP32 generates the 50 MHz clock for the LAN8720 itself (GPIO17), and Wi-Fi reception disturbs it. Independent of transmit power, buffers and Zigbee | **Ethernet 10 Mbit only** (negotiated via PHY register). Enough for 3 viewers |
| Stutter: 1 s send pauses without loss | A blocking `send()` sleeps on `ERR_MEM` until the lwIP timer (~1 s) | Send non-blocking, retry after 5 ms |
| Short stalls of the otoscope | The otoscope pauses on radio dropouts or by itself | Reconnect after 200 ms of silence, then at most every 800 ms |

Tried without effect: keepalive START every 5 s, smaller send blocks, larger TCP send buffer (11 KB), more Ethernet transmit buffers, store and forward in the Ethernet controller (still on, does no harm). The Zigbee module is held in reset although it did not cause the losses, because it is not needed and it saves power.

## Diagnostic tools

![Status page](../docs/screenshots/status.png)

| Tool | Purpose |
|---|---|
| `/status` | counters since start, among them `stalls_loss`/`stalls_clean`, `free_heap`/`min_heap`/`iram_heap`, `eth_speed`, and `last_crash` with the backtrace of the last crash |
| serial console | events (connections, stalls, camera changes) and fps and heap every 5 s |

`/log`, `/sensor`, `/crashtest` and the debug switches (`/debug/...`) were removed to save RAM. The packet header capture and the copy of the event log alone occupied ~6 KB of heap. The crash backtrace lives in RTC memory and costs no heap, so it stayed.

Resolve a backtrace from `last_crash` like this. You need the `firmware.elf` of **exactly this** firmware:

```
~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-addr2line -pfiaC \
  -e .pio/build/zb-gw03/firmware.elf 0x4008bf04 0x4008bec9 …
```

## Limitations

- **No PSRAM:** on WROVER modules GPIO16/17 are used for PSRAM, here they drive the Ethernet chip. About 120 KB of RAM and 58 KB of IRAM are available for frames and buffers.
- **VLC and Home Assistant get the raw image.** Only the browser does the orientation correction, the ESP32 cannot rotate JPEGs.
- **Dropouts with a weak signal:** in an unfavourable rotation of the probe (antenna) the signal drops to −70 dBm and below. Packets are then missing, and only the location of the ZB-GW03 can improve that.
