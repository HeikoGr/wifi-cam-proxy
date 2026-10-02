#!/usr/bin/env python3
"""
Takes the web UI screenshots for the documentation (docs/screenshots/) with a headless
Chromium against the mock server (mock_server.py).

    pip install playwright pillow && playwright install chromium
    python3 tools/ui-preview/screenshots.py [--image SOURCE]

The camera picture is the current one of the device (http://wifi-cam.local/snapshot) when it
is reachable, otherwise the generated test picture. --image takes another JPEG file or URL.
"""

import argparse
import time
from pathlib import Path

from playwright.sync_api import sync_playwright

import mock_server

OUT = mock_server.ROOT / "docs" / "screenshots"
DESKTOP = {"width": 1100, "height": 860}
PHONE = {"width": 390, "height": 844}
DEVICE_SNAPSHOT = "http://wifi-cam.local/snapshot"
# Live view with "Round" switched on, as usual with the otoscope (setting kept in the browser)
ROUND = "localStorage.setItem('round','true')"


def shoot(page, url, name, wait=2.0, full=False, action=None):
    page.goto(url, wait_until="domcontentloaded")
    time.sleep(wait)
    if action:
        action(page)
    page.screenshot(path=str(OUT / name), full_page=full)
    print("  ", name)


def record_circle(page):
    # Simulated probe turns once every 4 s; recording stops by itself at full coverage
    page.click("#recBtn")
    time.sleep(5)
    page.evaluate("window.scrollTo(0,0)")


def camera_image(source):
    try:
        image = mock_server.load_image(source)
        print("camera picture:", source)
        return image
    except Exception as e:  # device off or not reachable
        print(f"camera picture: test picture ({source}: {e})")
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", default=DEVICE_SNAPSHOT, help="camera picture: JPEG file or URL")
    args = ap.parse_args()
    image = camera_image(args.image)
    OUT.mkdir(parents=True, exist_ok=True)
    normal = mock_server.serve(8091, "normal", image)
    rescue = mock_server.serve(8092, "rescue", image)
    base, rbase = "http://127.0.0.1:8091", "http://127.0.0.1:8092"
    with sync_playwright() as p:
        browser = p.chromium.launch()
        desk = browser.new_page(viewport=DESKTOP, device_scale_factor=1)
        desk.add_init_script(ROUND)
        shoot(desk, base + "/", "live.png")
        shoot(desk, base + "/cameras", "cameras.png", full=True)
        shoot(desk, base + "/info", "status.png", full=True)
        shoot(desk, base + "/settings", "settings.png", full=True)
        shoot(desk, base + "/calibrate", "calibrate.png", full=True, action=record_circle)
        shoot(desk, rbase + "/wifi-setup", "wifi-setup-rescue.png", full=True)
        phone = browser.new_page(viewport=PHONE, device_scale_factor=2, is_mobile=True, has_touch=True)
        phone.add_init_script(ROUND)
        shoot(phone, base + "/", "live-phone.png")
        browser.close()
    normal.shutdown()
    rescue.shutdown()


if __name__ == "__main__":
    main()
