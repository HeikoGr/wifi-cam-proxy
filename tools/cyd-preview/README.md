# CYD display preview without hardware

`screenshots.py` draws the screens of the CYD ("Cheap Yellow Display") for the
documentation: live image at 1:1 and "fit", menu, camera choice and the waiting screen,
as `docs/screenshots/cyd-*.png` (320×240, doubled). With `--board fnk0115` it draws those of
the Freenove FNK0115 (800×480) as `docs/screenshots/fnk0115-*.png`.

The screens are not mocked up: [screens.cpp](screens.cpp) includes the real display code
[firmware/src/main_cyd.cpp](../../firmware/src/main_cyd.cpp) and builds it for the host
with LovyanGFX and JPEGDEC from the cyd build. The panel is replaced by an in-memory canvas
of the same size ([stub/LGFX_AUTODETECT.hpp](stub/LGFX_AUTODETECT.hpp),
[stub/lgfx_fnk0115.h](stub/lgfx_fnk0115.h)) that turns with
`setRotation()` like the display; camera, Wi-Fi and NVS are small stand-ins. JPEGDEC is
built without SIMD, so it decodes with the same C code as on the ESP32.

```bash
(cd firmware && ../.venv/bin/pio run -e cyd)        # once: downloads LovyanGFX and JPEGDEC
.venv/bin/python tools/cyd-preview/screenshots.py
.venv/bin/python tools/cyd-preview/screenshots.py --board fnk0115
```

The picture is the current one of the device (`http://wifi-cam.local/snapshot`), with the
rotation, battery and LED type of the connected camera from `/cameras.json`. Without the
device it uses the generated test picture of the [web UI preview](../ui-preview/) as an
otoscope. `--image FILE|URL`, `--rotation DEG` and `--battery PCT` override it.

Needs `g++`/`gcc` and Pillow (`./setup-build-env.sh --ui`). The object files of the
libraries are kept in `firmware/.pio/cyd-preview/`.
