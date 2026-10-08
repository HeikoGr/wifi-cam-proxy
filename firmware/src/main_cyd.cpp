/*
 * WiFi-Cam-Viewer for the CYD "Cheap Yellow Display" (ESP32-2432S028R) and the
 * Freenove ESP32-S3 Display FNK0115 (800x480)
 *
 * Instead of Ethernet and a web server, the board shows the camera image directly on
 * its display (CYD: 320x240). Camera detection, protocols and frame store are the same
 * as in the Ethernet bridge (camera.cpp, cam_*.cpp, frame.cpp).
 *
 *   Video task (core 1, prio 10)    receives and assembles JPEGs (as before)
 *   Display task (core 1, prio 1)   always decodes the newest frame and shows it
 *                                   (CYD_DISPLAY_CORE; core 0 has Wi-Fi and lwIP).
 *                                   If it is slower than the camera, frames drop
 *                                   out by themselves (there is only "the newest").
 *   loop()                          camera scan and connection (cameraLoop)
 *
 * Zoom "1:1" (default): the centre crop at full resolution, JPEGDEC skips the
 * blocks outside it (setCropArea). Zoom "fit": decoded directly at reduced size
 * (480x480 -> 240x240, 1280x720 -> 320x180). In both cases it reads from the packet
 * list without copying the frame into one piece. Zoom 2x/4x: the centre crop of a
 * display divided by 2 or 4, every pixel enlarged in jpgDraw(). Freeze keeps one frame.
 *
 * Usage: tapping the image opens the menu (LED, zoom, choose camera, brightness).
 * There is no orientation correction: in 90° steps (all that is possible without a
 * frame buffer, i.e. without PSRAM) the image jumped back and forth in the hand.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>

#if defined(BOARD_FNK0115)
#include "lgfx_fnk0115.h"
#else
#define LGFX_ESP32_2432S028  // detect only the CYD variants, not all boards
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>
#endif
#include <JPEGDEC.h>

#include "camera.h"
#include "config.h"
#include "cpuload.h"
#include "crashlog.h"
#include "jpeg_crop.h"
#include "jpeg_reader.h"
#include "settings.h"

// Expected by camera.cpp. The display boards have no rescue mode and no OTA.
volatile bool rescueMode = false;
std::atomic<bool> updating{false};

void wifiApplyMode() {
  // b/g without 11n as in the bridge (most robust with the otoscope). Transmit power
  // stays at maximum: there is no Ethernet clock here it could disturb.
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
}

static LGFX lcd;
static JPEGDEC *jpeg = nullptr;  // ~18 KB, allocated once
static int ui = 1;  // size of texts and buttons: 1 on the CYD's 320x240, 2 on 800x480

// --- Settings (NVS) ----------------------------------------------------------------
static uint8_t brightness = 160;
enum ZoomLevel : uint8_t { Z_FIT, Z_1TO1, Z_2X, Z_4X, Z_COUNT };
static const char *const ZOOM_NAMES[Z_COUNT] = {"fit", "1:1", "2x", "4x"};
static uint8_t zoomLevel = Z_1TO1;  // 2x/4x: the 1:1 pixels of a smaller centre crop, enlarged
static bool frozen = false;         // still image: the held frame stays on the display
static Frame held;
static bool redrawHeld = false;

static void loadSettings() {
  nvsRead([](Preferences &p) {
    brightness = p.getUChar("cyd_bright", 160);
    zoomLevel = p.getUChar("cyd_zl", p.getBool("cyd_zoom", true) ? Z_1TO1 : Z_FIT);
    if (zoomLevel >= Z_COUNT) zoomLevel = Z_1TO1;
  });
}

static void saveSettings() {
  nvsWrite([](Preferences &p) {
    p.putUChar("cyd_bright", brightness);
    p.putUChar("cyd_zl", zoomLevel);
  });
}

// --- Read the JPEG from the packet list (include/jpeg_reader.h) ----------------------
static FrameReader reader;

static int32_t jpgRead(JPEGFILE *f, uint8_t *buf, int32_t len) {
  if (len > f->iSize - f->iPos) len = f->iSize - f->iPos;
  if (len <= 0) return 0;
  int32_t n = static_cast<FrameReader *>(f->fHandle)->read(f->iPos, buf, len);
  f->iPos += n;
  return n;
}
static int32_t jpgSeek(JPEGFILE *f, int32_t pos) {
  f->iPos = pos;
  return pos;
}
static void jpgClose(void *) {}
// With DMA (JPEG_USES_DMA, see jpeg_crop.h) JPEGDEC alternates between the two halves of
// its pixel buffer: one goes to the display by DMA while the next group is decoded into
// the other. pushImageDMA waits for the previous transfer before it starts the next one.
static bool dmaDraw = false;
static std::atomic<uint32_t> spiUsSum{0};  // time in jpgDraw() and waiting for DMA, for [stats]
static int zoomK = 1;  // enlargement of the decoded pixels (2x, 4x)

#if CYD_PAGE_FLIP
// Pixels straight into the hidden frame buffer, without LovyanGFX: two decoders write at
// the same time. Turned as setRotation() turns LovyanGFX's drawing
// (Panel_FrameBufferBase::drawPixelPreclipped) and cut to the visible part.
static struct {
  uint8_t **rows;     // frame buffer rows (panel orientation)
  int rot, w, h;      // rotation and the display size in it
  int x0, y0, x1, y1; // visible part
  int ks;             // every pixel 1 << ks times (zoom 2x, 4x)
} fb;

static void putPixels(const JPEGDRAW *d) {
  int ks = fb.ks, bx = d->x << ks, by = d->y << ks;
  int xa = max(bx, fb.x0), xb = min(bx + (d->iWidth << ks), fb.x1);
  if (xa >= xb) return;
  for (int y = max(by, fb.y0), ye = min(by + (d->iHeight << ks), fb.y1); y < ye; y++) {
    const uint16_t *src = d->pPixels + ((y - by) >> ks) * d->iWidth;
    switch (fb.rot) {
      case 0: {
        uint16_t *p = (uint16_t *)fb.rows[y];
        if (!ks) memcpy(p + xa, src + (xa - bx), (xb - xa) * sizeof(uint16_t));
        else for (int x = xa; x < xb; x++) p[x] = src[(x - bx) >> ks];
        break;
      }
      case 1: {
        int px = fb.h - 1 - y;
        for (int x = xa; x < xb; x++) ((uint16_t *)fb.rows[x])[px] = src[(x - bx) >> ks];
        break;
      }
      case 2: {
        uint16_t *p = (uint16_t *)fb.rows[fb.h - 1 - y] + fb.w - 1;
        for (int x = xa; x < xb; x++) p[-x] = src[(x - bx) >> ks];
        break;
      }
      case 3:
        for (int x = xa; x < xb; x++) ((uint16_t *)fb.rows[fb.w - 1 - x])[y] = src[(x - bx) >> ks];
        break;
    }
  }
}
#endif

static int jpgDraw(JPEGDRAW *d) {
  uint32_t t0 = micros();
#if CYD_PAGE_FLIP
  putPixels(d);
  spiUsSum += micros() - t0;
  return 1;
#endif
  if (zoomK > 1) {  // every pixel k times, per source row k display rows in one DMA transfer
    static uint16_t buf[2][4 * CYD_MAX_WIDTH];  // alternating: one is on its way while the next is built
    static int cur = 0;
    int per = CYD_MAX_WIDTH / zoomK;
    for (int row = 0; row < d->iHeight; row++) {
      const uint16_t *src = d->pPixels + row * d->iWidth;
      for (int c0 = 0; c0 < d->iWidth; c0 += per) {
        int n = min(per, d->iWidth - c0), bw = n * zoomK;
        uint16_t *b = buf[cur ^= 1];
        for (int i = 0; i < n; i++)
          for (int j = 0; j < zoomK; j++) b[i * zoomK + j] = src[c0 + i];
        for (int j = 1; j < zoomK; j++) memcpy(b + j * bw, b, bw * sizeof(uint16_t));
        lcd.pushImageDMA((d->x + c0) * zoomK, (d->y + row) * zoomK, bw, zoomK, (const lgfx::swap565_t *)b);
      }
    }
    spiUsSum += micros() - t0;
    return 1;
  }
  if (dmaDraw) lcd.pushImageDMA(d->x, d->y, d->iWidth, d->iHeight, (const lgfx::swap565_t *)d->pPixels);
  else lcd.pushImage(d->x, d->y, d->iWidth, d->iHeight, (const lgfx::swap565_t *)d->pPixels);
  spiUsSum += micros() - t0;
  return 1;
}

// --- Image rotation -------------------------------------------------------------
// As in the browser: per camera model (cameraImageRotation(), e.g. -90 for the otoscope,
// whose camera sits turned in the probe). The display turns in quarter turns only;
// CYD_ROTATE_DIR is the direction of setRotation() for +90 degrees.
static int imageRotation() {
  int quarters = (cameraImageRotation() % 360 + 405) / 90 % 4;  // clockwise, rounded: 0..3
  return ((CYD_UI_ROT + quarters * CYD_ROTATE_DIR) % 4 + 4) % 4;
}

// --- Display ----------------------------------------------------------------------
enum class Screen { Live, Menu, Choose };
static Screen screen = Screen::Live;
static uint32_t screenSince = 0;
static int lastX = 0, lastY = 0, lastW = 0, lastH = 0, lastRot = -1;  // image geometry
static int ovlSideW = 38;  // overlay in the side border: needs this much width (x ui)
static int ovlStrip = 10;  // otherwise a strip this high above the image (x ui)
static bool overlaySide = false;
static char overlayShown[72] = "";  // last drawn overlay text
static uint32_t drawnFrames = 0;
static std::atomic<uint32_t> drawMsSum{0}, drawMsMax{0};  // decode + SPI time, for [stats]
static std::atomic<uint32_t> decodeErrors{0};  // JPEGDEC stopped midway: rest of the image is old
static uint32_t lastFrameAt = 0;  // for the "no signal" hint
static float shownFps = 0;
static char statusShown[64] = "";
#if CYD_PAGE_FLIP
// Each frame buffer keeps what was drawn into it: a new geometry clears them one by one
static int buffersToClear = 0;
#endif

static void drawOverlay();

static bool openJpeg() {
  if (!jpeg->open(&reader, (int)reader.size(), jpgClose, jpgRead, jpgSeek, jpgDraw)) return false;
  jpeg->setPixelType(RGB565_BIG_ENDIAN);
  return true;
}

#if CYD_PAGE_FLIP
// Second decoder on core 0 for the lower rows (planSplit, include/jpeg_crop.h): the decode
// takes ~400 ms for 800x440 of the MAX-VIEW's 720p on one core. Needs restart markers.
static JPEGDEC *jpeg2 = nullptr;
static FrameReader reader2;
static TaskHandle_t decoder2 = nullptr;
static struct {
  int dx, dy, opt;
  TaskHandle_t waiter;
  bool ok;
  uint32_t us;
} job2;
static float topShare = 0.5f;  // rows for core 1; core 0 also runs Wi-Fi
static std::atomic<uint32_t> splitFrames{0};

static bool openJpeg2() {
  if (!jpeg2->open(&reader2, (int)reader2.size(), jpgClose, jpgRead, jpgSeek, jpgDraw)) return false;
  jpeg2->setPixelType(RGB565_BIG_ENDIAN);
  return true;
}

static void decoder2Task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint32_t t0 = micros();
    job2.ok = jpeg2->decode(job2.dx, job2.dy, job2.opt);
    job2.us = micros() - t0;
    xTaskNotifyGive(job2.waiter);
  }
}
#endif

static bool drawFrame(const Frame &f) {
  reader.reset(f);
  if (!openJpeg()) {
    reader.release();
    return false;
  }

  int rot = imageRotation();
  lcd.setRotation(rot);

  int W = jpeg->getWidth(), H = jpeg->getHeight();
  int dw = lcd.width(), dh = lcd.height();
  int k = zoomLevel >= Z_2X ? 1 << (zoomLevel - 1) : 1;
  zoomK = k;
  // Crop or scale, MCU groups, DMA ping-pong (include/jpeg_crop.h, host test jpeg_crop_test);
  // enlarged: the plan is made for the display divided by k, without DMA (jpgDraw enlarges)
  DecodePlan plan = planDecode(*jpeg, reader, dw / k, dh / k, zoomLevel != Z_FIT, CYD_USE_DMA && k == 1,
                               [] { return openJpeg(); });
  if (!plan.ok) {
    reader.release();
    lcd.setRotation(CYD_UI_ROT);
    return false;
  }
  int dx = plan.dx, dy = plan.dy, opt = plan.opt, x = plan.x * k, y = plan.y * k, w = plan.w * k, h = plan.h * k;
  dmaDraw = opt & JPEG_USES_DMA;
#if CYD_PAGE_FLIP
  SplitPlan split = {};
  if (decoder2) {
    reader2.reset(f);
    split = planSplit(*jpeg, plan, *jpeg2, reader2, topShare, [] { return openJpeg2(); });
    if (!split.ok) reader2.release();
  }
#endif

  // Room for the overlay (top left in UI orientation): in the side border, in the top
  // border, or else a black strip of ovlStrip pixels that the image leaves out. The
  // strip is cut at both opposite edges, so it does not matter in which direction
  // setRotation() turns: one of them is the UI top.
  int turn = ((rot - CYD_UI_ROT) % 4 + 4) % 4;
  int uiW = turn & 1 ? dh : dw, uiH = turn & 1 ? dw : dh;  // display in UI orientation
  int imgW = turn & 1 ? h : w, imgH = turn & 1 ? w : h;    // image in UI orientation
  overlaySide = (uiW - imgW) / 2 >= ovlSideW;
  if (!overlaySide && (uiH - imgH) / 2 < ovlStrip) {
    if (turn & 1) {
      int x1 = min(x + w, dw - ovlStrip);
      x = max(x, ovlStrip);
      w = x1 - x;
    } else {
      int y1 = min(y + h, dh - ovlStrip);
      y = max(y, ovlStrip);
      h = y1 - y;
    }
  }

  // Log every change of the geometry (to analyse jumps of the image)
  static int geo[8] = {};
  int now[8] = {W, H, rot, zoomLevel, dx, dy, w, h};
  if (memcmp(geo, now, sizeof(geo))) {
    memcpy(geo, now, sizeof(geo));
    Serial.printf("[geo] jpeg %dx%d, rot %d, %s, decode at %d,%d, visible %d,%d %dx%d\r\n", W, H, rot,
                  ZOOM_NAMES[zoomLevel], dx, dy, x, y, w, h);
  }

  bool clear = x != lastX || y != lastY || w != lastW || h != lastH || rot != lastRot;
  if (clear) {  // geometry changed -> clear the border
    lastX = x;
    lastY = y;
    lastW = w;
    lastH = h;
    lastRot = rot;
    statusShown[0] = 0;
    overlayShown[0] = 0;
  }
#if CYD_PAGE_FLIP
  if (clear) buffersToClear = LGFX::FBS;
  clear = buffersToClear > 0;
  if (clear) buffersToClear--;
  lcd.beginFrame();
#endif
  lcd.startWrite();
  if (clear) lcd.fillScreen(TFT_BLACK);
  lcd.setClipRect(x, y, w, h);  // do not paint edge blocks beyond the image
#if CYD_PAGE_FLIP
  fb = {lcd.frameRows(), rot, dw, dh, x, y, x + w, y + h, k == 4 ? 2 : k - 1};
  if (split.ok) {
    job2 = {split.dx, split.dy, split.opt, xTaskGetCurrentTaskHandle(), false, 0};
    xTaskNotifyGive(decoder2);
  }
  uint32_t t0 = micros();
  bool decoded = jpeg->decode(dx, dy, opt);
  if (split.ok) {
    uint32_t topUs = micros() - t0;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    decoded &= job2.ok;
    jpeg2->close();
    reader2.release();
    // Move the split so both cores take as long: share by the time per row of each
    float top = (float)topUs / split.topRows, bot = (float)job2.us / split.botRows;
    topShare = constrain(0.8f * topShare + 0.2f * bot / (top + bot), 0.2f, 0.8f);
    splitFrames++;
  }
  if (!decoded) decodeErrors++;
#else
  if (!jpeg->decode(dx, dy, opt)) decodeErrors++;
#endif
  if (dmaDraw || zoomK > 1) {  // the last group may still be on its way
    uint32_t t0 = micros();
    lcd.waitDMA();
    spiUsSum += micros() - t0;
  }
  lcd.clearClipRect();
  lcd.endWrite();
  jpeg->close();
  lcd.setRotation(CYD_UI_ROT);
  reader.release();
#if CYD_PAGE_FLIP
  overlayShown[0] = 0;  // the hidden buffer has an older frame's overlay
  drawOverlay();
  lcd.endFrame();
#endif
  return true;
}

// Battery and fps in the free border (left of square images, above wide ones) or in
// the strip the image leaves out. The image never paints there, so only redraw when
// the text changes (no flicker).
// Items: battery, fps, zoom, LED (only what the camera reports). Side border: short
// labels, one per line; strip: one line with the full labels.
static void drawOverlay() {
  const bool side = overlaySide;
  char item[4][16] = {"", "", "", ""};
  char shown[72];
  bool stale = !frozen && millis() - lastFrameAt > 3000;  // last frame is old
  if (telemetry.battery >= 0) snprintf(item[0], sizeof(item[0]), "%s%d%%", side ? "B:" : "Bat ", (int)telemetry.battery);
  snprintf(item[1], sizeof(item[1]), frozen ? "hold" : stale ? "old" : "%.0ffps", shownFps);
  snprintf(item[2], sizeof(item[2]), "%s%s", side ? "Z:" : "Zoom ", ZOOM_NAMES[zoomLevel]);
  if (telemetry.ledSupported && telemetry.led >= 0) {
    char v[8];
    if (telemetry.led != 1) strlcpy(v, "off", sizeof(v));
    else if (telemetry.ledDimmable) snprintf(v, sizeof(v), "%d%%", (int)ledLevel);
    else strlcpy(v, "on", sizeof(v));
    snprintf(item[3], sizeof(item[3]), "%s%s", side ? "L:" : "LED ", v);
  }
  snprintf(shown, sizeof(shown), "%d|%s|%s|%s|%s", side, item[0], item[1], item[2], item[3]);
  if (!strcmp(shown, overlayShown)) return;
  strlcpy(overlayShown, shown, sizeof(overlayShown));
  lcd.setFont(&fonts::Font0);
  lcd.setTextSize(ui);
  lcd.setTextDatum(top_left);
  lcd.setTextPadding(0);
  lcd.fillRect(0, 0, side ? ovlSideW : lcd.width(), side ? 44 * ui : ovlStrip, TFT_BLACK);
  int x = 2 * ui, y = side ? 2 * ui : ui;
  for (int i = 0; i < 4; i++) {
    if (!item[i][0]) continue;
    lcd.setTextColor(i == 1 && stale ? TFT_RED : i == 1 && frozen ? TFT_YELLOW : TFT_LIGHTGREY, TFT_BLACK);
    lcd.drawString(item[i], x, y);
    if (side) y += 10 * ui;
    else x += lcd.textWidth(item[i]) + 8 * ui;
  }
  lcd.setTextSize(1);
}

static const lgfx::IFont *fontLarge() { return ui > 1 ? &fonts::DejaVu40 : &fonts::DejaVu18; }
static const lgfx::IFont *fontSmall() {
  if (ui > 1) return &fonts::DejaVu24;
  return &fonts::Font2;
}

static void drawStatus(const char *text) {
  if (!strcmp(text, statusShown)) return;
  strlcpy(statusShown, text, sizeof(statusShown));
  lastW = lastH = 0;
  lastRot = -1;
  lcd.fillScreen(TFT_BLACK);
  lcd.setFont(fontLarge());
  lcd.setTextDatum(middle_center);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.drawString(text, lcd.width() / 2, lcd.height() / 2 - 12 * ui);
  lcd.setFont(fontSmall());
  lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  lcd.drawString("Tap: menu", lcd.width() / 2, lcd.height() / 2 + 18 * ui);
}

// --- Menu ---------------------------------------------------------------------------
// Laid out from the display size (lcd.width()/height() in UI orientation), so a larger
// display gets larger buttons and more rows in the camera choice. Buttons are found by
// their id, not by their position in the list.
static int gap = 6;    // margin and space between buttons (x ui)
static int rowH = 40;  // height of a row in the camera choice (x ui)
static const int MAX_NETS = 8;   // networks shown at most (as many as fit)

enum ButtonId : int8_t { B_LED, B_ZOOM, B_CHOOSE, B_BRIGHT, B_BACK, B_RESCAN, B_PROTO, B_FREEZE, B_NET0 };

struct Button {
  int16_t x, y, w, h;
  int8_t id;
  bool enabled;
};
static Button buttons[B_NET0 + MAX_NETS];
static int buttonCount = 0;

static void addButton(int id, int x, int y, int w, int h, const char *label, bool enabled = true) {
  buttons[buttonCount++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (int8_t)id, enabled};
  lcd.fillRoundRect(x, y, w, h, 8 * ui, enabled ? 0x2945 : 0x1082);
  lcd.setFont(fontLarge());
  lcd.setTextDatum(middle_center);
  lcd.setTextColor(enabled ? TFT_WHITE : TFT_DARKGREY);
  char text[40];  // shortened to the button width (long SSIDs)
  strlcpy(text, label, sizeof(text));
  for (size_t n = strlen(text); n > 1 && lcd.textWidth(text) > w - 12 * ui;) text[--n] = 0;
  lcd.drawString(text, x + w / 2, y + h / 2);
}

static int hitButton(int tx, int ty) {  // id of the touched button, -1 = none
  for (int i = 0; i < buttonCount; i++) {
    const Button &b = buttons[i];
    if (b.enabled && tx >= b.x && tx < b.x + b.w && ty >= b.y && ty < b.y + b.h) return b.id;
  }
  return -1;
}

static void clearForScreen(Screen s) {
  screen = s;
  screenSince = millis();
  statusShown[0] = 0;
  lastW = lastH = 0;
  lastRot = -1;
  lcd.fillScreen(TFT_BLACK);
  buttonCount = 0;
}

// Two columns, three rows: LED, zoom / camera, light / freeze, back
static void showMenu() {
  clearForScreen(Screen::Menu);
  int dw = lcd.width(), dh = lcd.height();
  int w = (dw - 3 * gap) / 2, h = (dh - 4 * gap) / 3;
  int x0 = gap, x1 = 2 * gap + w;
  auto y = [&](int row) { return gap + row * (h + gap); };
  bool led = telemetry.ledSupported;
  char ledLabel[24];
  if (!led) strlcpy(ledLabel, "LED -", sizeof(ledLabel));
  else if (telemetry.ledDimmable && telemetry.led == 1) snprintf(ledLabel, sizeof(ledLabel), "LED %d%%", (int)ledLevel);
  else strlcpy(ledLabel, telemetry.led == 1 ? "LED off" : "LED on", sizeof(ledLabel));
  addButton(B_LED, x0, y(0), w, h, ledLabel, led);
  char zoom[24];
  snprintf(zoom, sizeof(zoom), "Zoom: %s", ZOOM_NAMES[zoomLevel]);
  addButton(B_ZOOM, x1, y(0), w, h, zoom);
  addButton(B_CHOOSE, x0, y(1), w, h, "Camera");
  char bright[24];
  snprintf(bright, sizeof(bright), "Light %d%%", brightness * 100 / 255);
  addButton(B_BRIGHT, x1, y(1), w, h, bright);
  addButton(B_FREEZE, x0, y(2), w, h, frozen ? "Resume" : "Freeze");
  addButton(B_BACK, x1, y(2), w, h, "Back");
}

static ScanEntry nets[MAX_NETS];
static int netCount = 0;
static int protoIndex = 0;  // protoChoice(protoIndex) for the camera choice

static int barY() { return lcd.height() - gap - rowH; }  // row with rescan, protocol, back

// One row per open network (as many as fit above the bottom bar), then the bar
static void showChoose() {
  clearForScreen(Screen::Choose);
  int dw = lcd.width();
  int rows = min(MAX_NETS, (barY() - gap) / (rowH + gap));
  // Only open networks (no keyboard, no password), recognised cameras first
  ScanEntry all[16];
  int n = cameraNetworks(all, 16);
  netCount = 0;
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < n && netCount < rows; i++)
      if (all[i].open && (all[i].proto != CamProto::None) == (pass == 0)) nets[netCount++] = all[i];
  char cur[33];
  cameraCurrentSsid(cur, sizeof(cur));
  for (int i = 0; i < netCount; i++) {
    char label[40];
    snprintf(label, sizeof(label), "%s%s", strcmp(nets[i].ssid, cur) ? "" : "> ", nets[i].ssid);
    addButton(B_NET0 + i, gap, gap + i * (rowH + gap), dw - 2 * gap, rowH, label);
  }
  if (!netCount) {
    lcd.setFont(fontLarge());
    lcd.setTextDatum(middle_center);
    lcd.setTextColor(TFT_LIGHTGREY);
    lcd.drawString("No open networks", dw / 2, barY() / 2);
  }
  int w = (dw - 4 * gap) / 3;
  addButton(B_RESCAN, gap, barY(), w, rowH, "Rescan");
  addButton(B_PROTO, 2 * gap + w, barY(), w, rowH, protoKey(protoChoice(protoIndex)));  // protocol for the next connect
  addButton(B_BACK, 3 * gap + 2 * w, barY(), w, rowH, "Back");
}

static void showLive() {
  clearForScreen(Screen::Live);
}

static void setFrozen(bool on) {
  if (on) {
    getFrame(held);  // keeps the newest frame out of the store's reuse
    frozen = (bool)held;
    redrawHeld = frozen;
  } else {
    held.reset();
    frozen = false;
  }
  overlayShown[0] = 0;
}

static void setZoom(int level) {
  zoomLevel = constrain(level, 0, Z_COUNT - 1);
  redrawHeld = frozen;
  saveSettings();
}

static void onTouch(int tx, int ty) {
  if (screen == Screen::Live) return showMenu();
  int b = hitButton(tx, ty);
  if (screen == Screen::Menu) {
    switch (b) {
      case B_LED:
        if (telemetry.ledDimmable) {  // off -> 100 % -> 50 % -> 20 % -> off
          int now = telemetry.led == 1 ? (int)ledLevel : 0;
          int next = now == 0 ? 100 : now > 50 ? 50 : now > 20 ? 20 : 0;
          if (next) ledLevel = next;
          ledRequest = next ? 1 : 0;
          telemetry.led = next ? 1 : 0;  // no confirmation: show the new state right away
          return showMenu();
        }
        ledRequest = telemetry.led == 1 ? 0 : 1;
        return showLive();
      case B_ZOOM: setZoom((zoomLevel + 1) % Z_COUNT); return showMenu();
      case B_FREEZE: setFrozen(!frozen); return showLive();
      case B_CHOOSE: cameraRequestScan(); return showChoose();
      case B_BRIGHT:
        brightness = brightness >= 255 ? 40 : brightness >= 160 ? 255 : brightness >= 90 ? 160 : 90;
        lcd.setBrightness(brightness);
        saveSettings();
        return showMenu();
      case B_BACK: return showLive();
    }
  } else if (screen == Screen::Choose) {
    if (b >= B_NET0 && b < B_NET0 + netCount) {
      cameraSelect(nets[b - B_NET0].ssid, "", protoChoice(protoIndex));
      return showLive();
    }
    if (b == B_RESCAN) {
      cameraRequestScan();
      screenSince = millis();
      lcd.setFont(fontSmall());
      lcd.setTextDatum(middle_center);
      lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
      lcd.drawString(" scanning... ", lcd.width() / 2, barY() - 12 * ui);
    }
    if (b == B_BACK) return showLive();
    if (b == B_PROTO) {  // next protocol (auto -> i4season -> jhcmd -> ...)
      protoIndex = (protoIndex + 1) % protoChoiceCount();
      return showChoose();
    }
  }
}

static void displayTask(void *) {
  uint32_t lastSeq = 0, lastTouch = 0, lastOverlay = 0, fpsSince = millis(), fpsFrames = 0;
  bool touching = false;
  uint16_t lastKey = telemetry.keySeq;
  for (;;) {
    // Buttons of the camera (JHCMD): zoom+ / zoom- change the zoom level, photo = still image
    if (telemetry.keySeq != lastKey) {
      lastKey = telemetry.keySeq;
      int key = telemetry.key;
      if (screen == Screen::Live) {
        if (key == KEY_ZOOM_IN) setZoom(zoomLevel + 1);
        else if (key == KEY_ZOOM_OUT) setZoom(zoomLevel - 1);
        else if (key == KEY_PHOTO) setFrozen(!frozen);
      }
    }
    // Touch: react to the touch only, not to holding
    lgfx::touch_point_t tp;
    bool t = lcd.getTouch(&tp) > 0;
    if (t && !touching && millis() - lastTouch > 300) {
      lastTouch = millis();
      onTouch(tp.x, tp.y);
    }
    touching = t;

    const char *st = cameraStateKey();
    if (screen == Screen::Live && !strcmp(st, "choose")) showChoose();  // several cameras
    if (screen != Screen::Live) {
      // the menu closes by itself; the camera list refreshes after the scan
      if (millis() - screenSince > CYD_MENU_TIMEOUT_MS && strcmp(st, "choose")) showLive();
      else if (screen == Screen::Choose && millis() - screenSince > 4000 && strcmp(st, "scanning") &&
               !netCount)
        showChoose();
      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }

    Frame f;
    if (frozen) {
      if (redrawHeld || lastRot < 0) {
        redrawHeld = false;
        drawFrame(held);
      }
      if (millis() - lastOverlay >= 1000) {
        lastOverlay = millis();
        drawOverlay();
      }
      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }
    uint32_t seq = getFrame(f);
    bool drew = false;
    if (f && seq != lastSeq) {
      lastSeq = seq;
      uint32_t t0 = millis();
      if (drawFrame(f)) {
        uint32_t ms = millis() - t0;
        drawMsSum += ms;
        if (ms > drawMsMax) drawMsMax = ms;
        drew = true;
        lastFrameAt = millis();
        drawnFrames++;
        fpsFrames++;
      }
      // A new frame is almost always ready (camera faster than the display): without
      // this the task would never block, and loop() (same core and priority) and the
      // idle task would not get to run
      vTaskDelay(1);
    } else if (!f && !strcmp(st, "connected") && lastFrameAt && millis() - lastFrameAt < 3000) {
      // Store empty only for a moment: it gave its frame up for the next one (memory
      // short, 720p). Keep the last image instead of flashing the waiting screen.
      vTaskDelay(pdMS_TO_TICKS(5));
    } else if (!f) {
      char text[64];
      char ssid[33];
      cameraCurrentSsid(ssid, sizeof(ssid));
      if (!strcmp(st, "connected")) snprintf(text, sizeof(text), "Waiting for image...");
      else if (!strcmp(st, "connecting")) snprintf(text, sizeof(text), "Connecting %.20s", ssid);
      else snprintf(text, sizeof(text), "Looking for camera...");
      drawStatus(text);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));  // wait for the next frame
    }
    f.reset();

    if (millis() - fpsSince >= 2000) {
      shownFps = fpsFrames * 1000.0f / (millis() - fpsSince);
      fpsFrames = 0;
      fpsSince = millis();
    }
    if (drew || millis() - lastOverlay >= 1000) {  // after every frame, else 1x/s
      lastOverlay = millis();
      drawOverlay();
    }
  }
}

// --- Start ------------------------------------------------------------------------
// Texts and buttons grow with the display; called once, in UI orientation
static void scaleUi() {
  ui = lcd.width() >= 640 ? 2 : 1;
  ovlSideW *= ui;
  ovlStrip *= ui;
  gap *= ui;
  rowH *= ui;
}

static void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    crumb("wifi connected %s, RSSI %d", WiFi.SSID().c_str(), WiFi.RSSI());
    cameraOnWifiGotIp();
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    crumb("wifi disconnected, reason %u", info.wifi_sta_disconnected.reason);
  }
}

void setup() {
#ifdef CYD_RGB_LED_PINS
  static const int rgb[] = CYD_RGB_LED_PINS;
  for (int pin : rgb) {  // RGB LED off (active LOW)
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
#endif
  Serial.begin(115200);
  crashlogInit();
#ifndef GIT_REV
#define GIT_REV "unknown"  // set by git_rev.py
#endif
  Serial.println("\r\n[boot] WiFi-Cam-Viewer (" CYD_BOARD_NAME ") " GIT_REV);

  loadSettings();
  if (!lcd.init()) {  // RGB panel: no frame buffer without PSRAM
    Serial.println("[lcd] init failed (no PSRAM?)");
    for (;;) delay(1000);
  }
#ifdef CYD_SPI_WRITE_HZ
  if (auto bus = lcd.getPanel()->getBus(); bus && bus->busType() == lgfx::bus_type_t::bus_spi) {
    auto spi = static_cast<lgfx::Bus_SPI *>(bus);
    auto cfg = spi->config();
    Serial.printf("[lcd] SPI %u -> %u MHz\r\n", (unsigned)(cfg.freq_write / 1000000),
                  (unsigned)(CYD_SPI_WRITE_HZ / 1000000));
    cfg.freq_write = CYD_SPI_WRITE_HZ;
    spi->config(cfg);  // takes effect with the next transaction
  }
#endif
  lcd.setRotation(CYD_UI_ROT);
  lcd.setBrightness(brightness);
  lcd.fillScreen(TFT_BLACK);
  scaleUi();
  // Internal RAM: with PSRAM, a large malloc() would land there
  void *mem = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  jpeg = mem ? new (mem) JPEGDEC : nullptr;
#if CYD_PAGE_FLIP
  if (void *mem2 = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) {
    jpeg2 = new (mem2) JPEGDEC;
    xTaskCreatePinnedToCore(decoder2Task, "decoder2", 6144, nullptr, 1, &decoder2, 0);
  }
#endif
  if (!jpeg) {
    lcd.drawString("No memory for JPEG", 10, 10);
    for (;;) delay(1000);
  }

  Network.onEvent(onNetworkEvent);
  heap_caps_monitor_local_minimum_free_size_start();  // heap minimum per [stats] interval
  cameraBegin();
  xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 1, nullptr, CYD_DISPLAY_CORE);
}

void loop() {
  cameraLoop();
  static uint32_t lastStats = 0, lastFrames = 0, lastDrawn = 0, lastLost = 0, lastDamaged = 0,
                  lastIncomplete = 0, lastTooBig = 0, lastNoMem = 0, lastReleased = 0,
                  lastDecodeErr = 0;
  if (millis() - lastStats >= 5000) {
    uint32_t total = stats.framesTotal, drawn = drawnFrames, lost = stats.packetsLost,
             damaged = stats.framesDamaged, incomplete = stats.dropIncomplete,
             tooBig = stats.dropTooBig, noMem = stats.dropNoMem, released = stats.framesReleased,
             decodeErr = decodeErrors;
    // Lowest free heap in this interval (not since boot): shows whether the frames leave
    // the Wi-Fi driver enough
    unsigned minNow = heapMin();
    heap_caps_monitor_local_minimum_free_size_stop();
    heap_caps_monitor_local_minimum_free_size_start();
    float dt = (millis() - lastStats) / 1000.0f;
    cpuLoadUpdate();
    // Artifacts with "damaged" > 0: Wi-Fi (packet loss). Without: look at draw ms vs.
    // the frame interval of the camera.
    Serial.printf("[stats] received %.1f fps, shown %.1f fps | lost pkts %u, damaged %u, incomplete %u, "
                  "too big %u, no mem %u, released %u, handshakes %u, RSSI %d | zoom %s, draw avg %u ms (SPI %u ms) max %u ms, decode errors %u | battery %d%%%s | "
                  "heap %u (min %u) | largest frame %u KB | CPU %d/%d %%\r\n",
                  (total - lastFrames) / dt, (drawn - lastDrawn) / dt, lost - lastLost,
                  damaged - lastDamaged, incomplete - lastIncomplete, tooBig - lastTooBig, noMem - lastNoMem,
                  released - lastReleased,
                  (unsigned)stats.handshakes, (int)WiFi.RSSI(), ZOOM_NAMES[zoomLevel],
                  drawn > lastDrawn ? (unsigned)(drawMsSum / (drawn - lastDrawn)) : 0u,
                  drawn > lastDrawn ? (unsigned)(spiUsSum / 1000 / (drawn - lastDrawn)) : 0u,
                  (unsigned)drawMsMax, decodeErr - lastDecodeErr, (int)telemetry.battery, telemetry.charging == 1 ? " (charging?)" : "",
                  heapFree(), minNow, (unsigned)(stats.maxFrameBytes / 1024), cpuLoad(0), cpuLoad(1));
#if CYD_PAGE_FLIP
    Serial.printf("[stats] two cores %u of %u frames, core 1 decodes %d %% of the rows | PSRAM %u KB free\r\n",
                  (unsigned)splitFrames.exchange(0), drawn - lastDrawn, (int)(topShare * 100),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
#endif
    drawMsSum = 0;
    spiUsSum = 0;
    drawMsMax = 0;
    lastFrames = total;
    lastDrawn = drawn;
    lastLost = lost;
    lastDamaged = damaged;
    lastIncomplete = incomplete;
    lastTooBig = tooBig;
    lastNoMem = noMem;
    lastReleased = released;
    lastDecodeErr = decodeErr;
    lastStats = millis();
  }
  delay(10);
}
