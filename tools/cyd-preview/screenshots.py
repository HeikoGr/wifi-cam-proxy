#!/usr/bin/env python3
"""
Screenshots of the CYD display for the documentation (docs/screenshots/cyd-*.png).

The screens are drawn by the real display code (firmware/src/main_cyd.cpp) compiled for
the host, into an in-memory canvas instead of the panel (screens.cpp, build.sh): live
image at 1:1 and "fit", menu, camera choice, waiting screen.

    python3 tools/cyd-preview/screenshots.py [--image SOURCE] [--rotation DEG] [--battery PCT]

The picture is the current one of the device (http://wifi-cam.local/snapshot) with that
camera's rotation, battery and LED from /cameras.json when it is reachable, otherwise the
generated test picture of the web UI preview (as an otoscope). Needs g++, Pillow and the
cyd build's libraries (cd firmware && pio run -e cyd).
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path
from urllib.request import urlopen

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / "docs" / "screenshots"
BUILD = ROOT / "firmware" / ".pio" / "cyd-preview"  # object files are kept between runs
DEVICE = "http://wifi-cam.local"
SCALE = 2  # 320x240 is tiny on a web page: double it, pixel for pixel


def device_camera():
    """Picture and camera data of the device, or None if it is not reachable."""
    try:
        with urlopen(DEVICE + "/cameras.json", timeout=5) as r:
            cam = json.load(r)
        with urlopen(DEVICE + "/snapshot", timeout=5) as r:
            jpg = r.read()
    except OSError as e:
        print(f"device not reachable ({e}): test picture")
        return None
    led = "dim" if cam.get("led_dimmable") else "switch" if cam.get("led_supported") else "none"
    print(f"camera picture: {DEVICE}/snapshot ({cam.get('ssid')}, rotation {cam.get('rotation')})")
    return jpg, cam.get("rotation", 0), cam.get("battery", -1), cam.get("ssid", ""), led


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", help="JPEG file or URL instead of the device's picture")
    ap.add_argument("--rotation", type=int, help="image rotation of the camera model (otoscope: -90)")
    ap.add_argument("--battery", type=int, help="battery in %%")
    args = ap.parse_args()

    from PIL import Image

    cam = None if args.image else device_camera()
    if cam:
        jpg, rotation, battery, ssid, led = cam
    elif args.image:
        if "://" in args.image:
            with urlopen(args.image, timeout=5) as r:
                jpg = r.read()
        else:
            jpg = Path(args.image).read_bytes()
        rotation, battery, ssid, led = 0, 80, "MAXVIEW-7762", "dim"
    else:
        sys.path.insert(0, str(ROOT / "tools" / "ui-preview"))
        import mock_server  # already turned for the otoscope's -90°

        jpg, rotation, battery, ssid, led = mock_server.test_image(), -90, 80, "Soulear-6b1c9", "switch"
    if args.rotation is not None:
        rotation = args.rotation
    if args.battery is not None:
        battery = args.battery

    subprocess.run([str(HERE / "build.sh"), str(BUILD)], check=True)
    frame = BUILD / "frame.jpg"
    frame.write_bytes(jpg)
    res = subprocess.run([str(BUILD / "screens"), str(frame), str(rotation), str(battery), ssid, led, str(BUILD)],
                         check=True, capture_output=True, text=True)
    OUT.mkdir(parents=True, exist_ok=True)
    for line in res.stdout.splitlines():
        if not line.endswith(".ppm"):
            continue
        img = Image.open(line)
        name = Path(line).with_suffix(".png").name
        img.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST).save(OUT / name)
        print("  ", name)


if __name__ == "__main__":
    main()
