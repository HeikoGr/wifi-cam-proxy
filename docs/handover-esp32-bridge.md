# Handover: Soulear otoscope without the vendor app

As of: 2026-09-30, evening (ESP32 bridge in use). This document describes the state before the
multi-camera extension; the current state is in the [README](../README.md).

## Goal

Show the live image of the Wi-Fi ear cleaner (brand Hopefox, vendor app "Soulear") without the original Chinese app. It should work in the browser, in VLC or Home Assistant and be reachable from the whole home network.

## Hardware (verified)

The data comes from the device's `GetDeviceInfo` reply:

| Field | Value |
|---|---|
| Brand / model | Hopefox, model name probably "Find T" |
| Vendor app | Soulear (`com.i4season.bkCamera_soulear`) |
| Chip / product | `BK7231U-XRH-FBPRO` (Beken BK7231U, firmware namespace `XRH`) |
| Firmware | `HKV41B` |
| SSID | `Soulear-6b1c9` (open, without password) |
| Device IP | `192.168.1.1` (DHCP server in the device) |
| Client IP | `192.168.1.10` (assigned by the device via DHCP) |

The vendor field only returns `YPC` plus garbage (`ota ok`, `sys_cfgs.license =`) because the firmware does not clear the buffer properly. That is harmless.

## Network setup (only for the Python viewer on the VM)

Not needed for the ESP32 bridge. The VM is then only in the home network.

- Debian VM in the Proxmox cluster with two interfaces:
  - `eth0`: home network, gateway `192.168.178.1` (Fritzbox), static. Internet goes through it.
  - `wls16`: Wi-Fi adapter in the VM, connected to `Soulear-6b1c9` via DHCP (metric 600).
- Both default routes coexist without problems. `192.168.1.0/24` goes through the direct network route on `wls16`.
- Connect: `nmcli device wifi connect "Soulear-6b1c9"`

## Protocol (i4season / libWifiCamera family)

Everything runs over UDP. Header: 12 bytes, little-endian.

```
magic  u32 = 0xFFEEFFEE   (on the wire: EE FF EE FF)
id     u16   running number, echoed back
type   u16   command type
unk    u8  = 1 in requests
err    u8  = 0 = OK
length u16   payload length
```

| Step | Direction | Port | Status |
|---|---|---|---|
| GetDeviceInfo (type 0x01) | client → device | UDP 10005 | ✅ verified (140-byte reply) |
| START / OpenVideo (type 0x04) | client → device | UDP 10006, payload = own receive port (u16 LE) + `00 00` | ✅ verified (ACK `eeffeeff 02000400 01000000`) |
| Video data | device → client | comes from UDP 10006 to the announced port | ✅ verified, 17–18 fps, 480×480 |

**Important:** GetDeviceInfo must come from the same socket **before** START. Without this step the device acknowledges START but sends no video.

Every video packet has a 16-byte header (see below), followed by JPEG bytes. A frame starts with `FF D8` and ends with `FF D9`, possibly followed by padding zeros. The resolution is 480×480 (MJPEG). The 15–30 packets of a frame arrive as a fast burst.

**Important:** The first packet after idle is often swallowed. Therefore always send requests several times. After a START the otoscope needs a few hundred milliseconds to start up. A new START during that time restarts it again, so there should be at least ~800 ms between two START commands.

### Video packet header (16 bytes, verified on the device)

| Byte | Meaning |
|---|---|
| 0 | always `01` |
| 1 | running packet number (8 bits, wraps around) |
| 2 | frame number (8 bits) |
| 3 | `00`, in the reference capture `01` on the last packet of a frame |
| 4 | number of packets in the frame |
| 5 | always `01` |
| 6–9 | accelerometer, u32 little-endian: x = bits 0–9, y = 10–19, z = 20–29; each 10-bit sign-magnitude (bit 9 = sign), ~128 = 1 g |
| 10–11 | constant `66 90` |
| 12–15 | `80 02 e0 01` (640/480 LE), although the frames are 480×480 |

Roll angle of the probe = `atan2(x, y)`. The orientation is in **every** packet of a frame. The axes have small offsets (x ≈ −7, y ≈ +6) and slightly different sensitivity, hence the bridge's calibration page. The camera is mounted rotated by 90° in the probe, so the image is always rotated by −90°. Frames are 5–41 KB, the device delivers 17–18 fps.

## Files

| File | Purpose | Status |
|---|---|---|
| `probe-soulear.py` | Sends GetDeviceInfo and decodes the reply | ✅ works |
| `soulear-viewer.py` | Python viewer for the VM: GetDeviceInfo + START, assemble frames, MJPEG on port 45100 | ✅ works, superseded by the ESP32 bridge |
| `firmware/` | Firmware for the ZB-GW03 (ESP32 + LAN8720): Wi-Fi to the otoscope, MJPEG and web UI in the LAN | ✅ in use, details in [firmware/README.md](../firmware/README.md) |

The Python scripts only need the standard library, the camera IP can be passed as an argument (default `192.168.1.1`).

## ESP32 bridge (as of 2026-09-30)

A ZB-GW03 v1.4 (originally a Zigbee gateway, ESP32 without PSRAM + LAN8720) is connected to the otoscope via Wi-Fi and to the home network via LAN. The ESPHome firmware was replaced via OTA.

- **Reachable:** `http://otoskop.local/` (Ethernet IP last `192.168.178.130`), stream under `/stream`
- **Features:** MJPEG stream, snapshot, orientation correction in the browser with calibration page, firmware update in the browser, rescue mode via the home Wi-Fi, crash capture and event log
- **Performance:** 17 fps without dropouts with a good signal (measured: 516 of 517 frames, longest pause 92 ms)

The most important findings (in detail in the firmware README):

1. **Wi-Fi reception disturbs Ethernet.** The ESP32 generates the 50 MHz clock for the LAN8720 itself (GPIO17). At 100 Mbit 2–3 % of the Ethernet packets were lost, at 10 Mbit none. Therefore Ethernet runs fixed at **10 Mbit**.
2. **`getFreeHeap()` is misleading:** it counts ~44 KB of IRAM that cannot be used for `malloc` and task stacks. The frame data is therefore deliberately stored in the IRAM remainder and read and written word by word.
3. **The Arduino defaults are too tight:** the UDP buffer held only 6 packets. Via `custom_sdkconfig` (pioarduino) it is increased to 32, and Bluetooth is removed.
4. **lwIP waits up to 1 s:** a blocking `send()` sleeps until the timer on a brief memory shortage. The firmware therefore sends non-blocking.
5. **Wi-Fi mode b/g without 11n:** without packet aggregation a missing sub-packet no longer blocks the whole block.

## Next steps

1. **Home Assistant:** set up an MJPEG camera with `http://otoskop.local/stream`. The image arrives unrotated there.
2. **Everyday test** with rotating and changing distances. The counters in `/status` (`stalls_loss`, `packets_lost`) show how often the signal drops. With frequent dropouts, place the ZB-GW03 closer to where it is used.
3. **Clean-up, optional:** remove the test switches (`/debug/...`, `/crashtest`, `/sensor`) or keep them for diagnostics. The experimental switches at the end of `firmware/include/config.h` can be hard-coded. *(Done since then: removed to save RAM.)*

## Open ideas

- **LED control on the otoscope:** a command type `0x0A` (SetLed) is documented in the i4season family, untested on the device. *(Implemented since then.)*
- **Resolution:** query the supported modes with `0x0D` (GetCameraConfig). Caution: on the MS5 a mode change at runtime could block the encoder until the device was restarted.
- **Regular pauses of the otoscope:** about every 25 s there were short pauses without packet loss. A keepalive START did not help. Since the latest changes they have not occurred any more, still being observed.
- **Hardware:** an ESP32 board with its own 50 MHz oscillator for Ethernet (e.g. WT32-ETH01, clock on GPIO0) would allow 100 Mbit despite Wi-Fi. For one stream 10 Mbit is enough, though.
- **Custom firmware for the otoscope:** for the BK7231U there is community firmware (OpenBeken/LibreTiny ecosystem), but no camera driver. For this project that is currently not a realistic option.

## Sources / references

- pedrodinisf/otoscope-viewer: exactly the same hardware (AiSee, BK7231U, XRH), macOS only. Contains the protocol cheat sheet and test fixtures. https://github.com/pedrodinisf/otoscope-viewer
- Fyfar/ms5-wifi-microscope: the most detailed protocol documentation of the i4season family (header, command types, resolution). https://github.com/Fyfar/ms5-wifi-microscope
- SeanPesce/Suear-Web-Viewer: MJPEG mirror for Suear devices (same family). https://github.com/SeanPesce/Suear-Web-Viewer
- Elektroda thread on Taixen TXW816 otoscopes: UART/firmware dump of other hardware, background only. https://www.elektroda.com/news/news4129331.html
