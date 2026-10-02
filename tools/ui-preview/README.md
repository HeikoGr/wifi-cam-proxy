# Web UI preview without hardware

`mock_server.py` serves the web UI exactly as the firmware does: the pages are compiled
straight from [firmware/include/web_ui.h](../../firmware/include/web_ui.h) with the host
C++ compiler. The device API (`/status`, `/cameras.json`, `/stream`, `/orientation`, …) is
simulated with plausible values. The camera image is a generated test picture marked
"SIMULATED IMAGE" unless `--image` gives a real one (JPEG file or URL, e.g.
`--image http://otoskop.local/snapshot`), and the orientation sensor turns slowly so the
calibration page works.

```bash
./setup-build-env.sh --ui                        # once: .venv with Pillow, Playwright, Chromium
.venv/bin/python tools/ui-preview/mock_server.py # http://127.0.0.1:8080/
.venv/bin/python tools/ui-preview/mock_server.py --scenario rescue   # or: choose
```

Regenerate the screenshots in [docs/screenshots/](../../docs/screenshots/):

```bash
.venv/bin/python tools/ui-preview/screenshots.py
```

The screenshots show the current picture of the device (`http://otoskop.local/snapshot`)
when it is reachable, otherwise the test picture; `--image` takes another file or URL.

Needs `c++` (g++ or clang++) on the host.
