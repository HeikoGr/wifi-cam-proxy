#!/usr/bin/env python3
"""Test images for jpeg_crop_test: the captured MAX-VIEW frame as the CYD gets it, and,
with Pillow, synthetic ones in other formats (sizes, subsampling, restart intervals).

    make_test_jpegs.py OUT_DIR    -> writes *.jpg into OUT_DIR, prints the file names
"""
import sys
from pathlib import Path

out = Path(sys.argv[1])
here = Path(__file__).parent

# MAX-VIEW 1280x720 4:2:0, restart interval 160 MCUs: payloads after the 8-byte header,
# the JPEG from FF D8 FF to FF D9, runs of fill bytes in the scan cut to one FF (as
# collapseFill does on the CYD)
raw = (here / "data" / "max-view-frame.bin").read_bytes()
data = b"".join(raw[i + 8:i + 1450] for i in range(0, len(raw), 1450))
jpg = bytearray(data[data.index(b"\xff\xd8\xff"):data.rindex(b"\xff\xd9") + 2])
scan = jpg.index(b"\xff\xda")
scan += 2 + (jpg[scan + 2] << 8 | jpg[scan + 3])
while (i := jpg.find(b"\xff\xff", scan)) >= 0:
    del jpg[i]
(out / "maxview-1280x720.jpg").write_bytes(jpg)
print(out / "maxview-1280x720.jpg")

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit(0)  # only the MAX-VIEW frame


def picture(w, h):
    # smooth like a camera image (the frame store takes up to 96 KB), with edges so a
    # shifted or missing block shows up
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        d.line([(0, y), (w, y)], fill=((y * 255) // h, 90, 255 - (y * 255) // h))
    for x in range(0, w, w // 8):
        d.line([(x, 0), (w - x, h)], fill=(255, 255, 255), width=3)
    d.ellipse([w // 4, h // 4, 3 * w // 4, 3 * h // 4], outline=(0, 0, 0), width=5)
    return img


# (name, size, subsampling 0=4:4:4 1=4:2:2 2=4:2:0, restart interval in MCU rows or None)
for name, (w, h), sub, rst in [
    ("soulear-like-480x480-420-rst", (480, 480), 2, 1),
    ("640x480-422", (640, 480), 1, None),
    ("640x480-444-rst", (640, 480), 0, 2),
    ("1280x720-420-nodri", (1280, 720), 2, None),
    ("1024x768-420-rst", (1024, 768), 2, 1),
]:
    p = out / f"{name}.jpg"
    kw = {"restart_marker_rows": rst} if rst else {}
    picture(w, h).save(p, quality=75, subsampling=sub, **kw)
    assert p.stat().st_size < 96 * 1024, (p, p.stat().st_size)
    print(p)
