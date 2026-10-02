// CYD decode geometry (firmware/include/jpeg_crop.h) against the real JPEGDEC: what the
// display shows must be pixel-identical to the same region of a full decode, at 1:1
// (crop, with skipping the rows above it) and "fit", with and without the DMA ping-pong
// mode, for several image formats and display sizes. Also checks that the DMA mode
// really alternates its two buffers.
#include "jpeg_crop.h"
#include <vector>
#include <string>

static FrameReader reader;
static int32_t jRead(JPEGFILE *f, uint8_t *buf, int32_t len) {
  if (len > f->iSize - f->iPos) len = f->iSize - f->iPos;
  if (len <= 0) return 0;
  int32_t n = reader.read(f->iPos, buf, len);
  f->iPos += n;
  return n;
}
static int32_t jSeek(JPEGFILE *f, int32_t pos) { return f->iPos = pos; }
static void jClose(void *) {}

// Target of the draw callback: a W x H pixel buffer with an optional clip rectangle
struct Canvas {
  int W, H, cx, cy, cw, ch;
  std::vector<uint16_t> px;
  std::vector<uint8_t> set;
  const void *lastBuf = nullptr;
  int calls = 0, swaps = 0;
};
static Canvas *canvas;
static int jDraw(JPEGDRAW *d) {
  Canvas &c = *canvas;
  c.calls++;
  if (c.lastBuf && d->pPixels != c.lastBuf) c.swaps++;
  c.lastBuf = d->pPixels;
  for (int y = 0; y < d->iHeight; y++)
    for (int x = 0; x < d->iWidth; x++) {
      int X = d->x + x, Y = d->y + y;
      if (X < c.cx || Y < c.cy || X >= c.cx + c.cw || Y >= c.cy + c.ch) continue;
      c.px[Y * c.W + X] = d->pPixels[y * d->iWidth + x];
      c.set[Y * c.W + X] = 1;
    }
  return 1;
}

static Frame toFrame(const std::string &jpg) {
  Frame f = Frame::create();
  for (size_t o = 0; o < jpg.size(); o += 1442) f.append((const uint8_t *)jpg.data() + o, std::min<size_t>(1442, jpg.size() - o));
  return f;
}

static JPEGDEC jpeg;
static bool openJpeg() {
  if (!jpeg.open(&reader, (int)reader.size(), jClose, jRead, jSeek, jDraw)) return false;
  jpeg.setPixelType(RGB565_BIG_ENDIAN);
  return true;
}

int main(int argc, char **argv) {
  int fails = 0;
  for (int a = 1; a < argc; a++) {
    FILE *fp = fopen(argv[a], "rb");
    if (!fp) { printf("cannot open %s\n", argv[a]); return 1; }
    std::string jpg; char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, fp)) > 0) jpg.append(b, n);
    fclose(fp);
    Frame frame = toFrame(jpg);
    const char *name = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
    for (int dsp = 0; dsp < 2; dsp++) {
      int dw = dsp ? 480 : 320, dh = dsp ? 320 : 240;
      for (int full = 1; full >= 0; full--)
        for (int dma = 0; dma < 2; dma++) {
          // reference: the whole image at the plan's scale, no crop
          reader.reset(frame);
          if (!openJpeg()) { printf("%s: JPEGDEC cannot open it\n", name); return 1; }
          int W = jpeg.getWidth(), H = jpeg.getHeight();
          DecodePlan p = planDecode(jpeg, reader, dw, dh, full, dma, openJpeg);
          if (!p.ok) { printf("%s: reopen failed\n", name); fails++; continue; }
          Canvas shown = {dw, dh, p.x, p.y, p.w, p.h, std::vector<uint16_t>(dw * dh), std::vector<uint8_t>(dw * dh)};
          canvas = &shown;
          bool decoded = jpeg.decode(p.dx, p.dy, p.opt);
          jpeg.close();
          int scale = p.opt & JPEG_SCALE_EIGHTH ? 8 : p.opt & JPEG_SCALE_QUARTER ? 4 : p.opt & JPEG_SCALE_HALF ? 2 : 1;
          reader.reset(frame);
          openJpeg();
          Canvas ref = {W / scale + 16, H / scale + 16, 0, 0, W / scale + 16, H / scale + 16,
                        std::vector<uint16_t>((W / scale + 16) * (H / scale + 16)), std::vector<uint8_t>((W / scale + 16) * (H / scale + 16))};
          canvas = &ref;
          jpeg.decode(0, 0, p.opt & ~JPEG_USES_DMA);
          jpeg.close();
          int missing = 0, wrong = 0;
          for (int y = p.y; y < p.y + p.h; y++)
            for (int x = p.x; x < p.x + p.w; x++) {
              if (!shown.set[y * dw + x]) { missing++; continue; }
              int sx = x - p.dx + p.ix / scale, sy = y - p.dy + p.iy / scale;
              if (sx < 0 || sy < 0 || sx >= ref.W || sy >= ref.H || shown.px[y * dw + x] != ref.px[sy * ref.W + sx]) {
                if (getenv("VERBOSE") && wrong < 6)
                  printf("    wrong at display %d,%d = image %d,%d: %04x instead of %04x\n", x, y, sx, sy, shown.px[y * dw + x],
                         sx >= 0 && sy >= 0 && sx < ref.W && sy < ref.H ? ref.px[sy * ref.W + sx] : 0);
                wrong++;
              }
            }
          bool pingpong = !(p.opt & JPEG_USES_DMA) || shown.swaps >= shown.calls - 1;
          bool ok = decoded && !missing && !wrong && pingpong;
          printf("%-24s %dx%d %-4s %-6s %4d calls  %-7s %s\n", name, dw, dh, full ? "1:1" : "fit",
                 p.opt & JPEG_USES_DMA ? "DMA" : dma ? "no DMA" : "-", shown.calls,
                 p.opt & JPEG_USES_DMA ? (pingpong ? "ping-pong" : "SAME BUF") : "",
                 ok ? "ok" : (std::string("FAIL: ") + std::to_string(missing) + " missing, " + std::to_string(wrong) + " wrong").c_str());
          fails += !ok;
        }
    }
    reader.release();
  }
  printf(fails ? "FAIL\n" : "OK\n");
  return fails;
}
