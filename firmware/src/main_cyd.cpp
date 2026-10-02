/*
 * WiFi-Cam-Viewer for the CYD "Cheap Yellow Display" (ESP32-2432S028R)
 *
 * Instead of Ethernet and a web server, the CYD shows the camera image directly on
 * its 320x240 display. Camera detection, protocols and frame store are the same as
 * in the Ethernet bridge (camera.cpp, cam_*.cpp, frame.cpp).
 *
 *   Video task (core 1, prio 10)    receives and assembles JPEGs (as before)
 *   Display task (core 0, prio 1)   always decodes the newest frame and shows it.
 *                                   If it is slower than the camera, frames drop
 *                                   out by themselves (there is only "the newest").
 *   loop()                          camera scan and connection (cameraLoop)
 *
 * Zoom "1:1" (default): the centre crop at full resolution, JPEGDEC skips the
 * blocks outside it (setCropArea). Zoom "fit": decoded directly at reduced size
 * (480x480 -> 240x240, 1280x720 -> 320x180). In both cases it reads from the packet
 * list without copying the frame into one piece.
 *
 * Usage: tapping the image opens the menu (LED, zoom, choose camera, brightness).
 * There is no orientation correction: in 90° steps (all that is possible without a
 * frame buffer, i.e. without PSRAM) the image jumped back and forth in the hand.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#define LGFX_ESP32_2432S028  // detect only the CYD variants, not all boards
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>
#include <JPEGDEC.h>

#include "camera.h"
#include "config.h"
#include "crashlog.h"
#include "jpeg_reader.h"
#include "settings.h"

// Expected by camera.cpp. The CYD has no rescue mode and no OTA.
volatile bool rescueMode = false;
std::atomic<bool> updating{false};

void wifiApplyMode() {
  // b/g without 11n as in the bridge (most robust with the otoscope). Transmit power
  // stays at maximum: there is no Ethernet clock here it could disturb.
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
}

static LGFX lcd;
static JPEGDEC *jpeg = nullptr;  // ~18 KB, allocated once

static const int UI_ROT = 1;  // landscape 320x240 for menus and touch

// --- Settings (NVS) ----------------------------------------------------------------
static uint8_t brightness = 160;
static bool zoomFull = true;  // 1:1 crop instead of the reduced full image

static void loadSettings() {
  nvsRead([](Preferences &p) {
    brightness = p.getUChar("cyd_bright", 160);
    zoomFull = p.getBool("cyd_zoom", true);
  });
}

static void saveSettings() {
  nvsWrite([](Preferences &p) {
    p.putUChar("cyd_bright", brightness);
    p.putBool("cyd_zoom", zoomFull);
  });
}

// --- Read the JPEG from the packet list (include/jpeg_reader.h) ----------------------
static FrameReader reader;

static int32_t jpgRead(JPEGFILE *f, uint8_t *buf, int32_t len) {
  if (len > f->iSize - f->iPos) len = f->iSize - f->iPos;
  if (len <= 0) return 0;
  int32_t n = reader.read(f->iPos, buf, len);
  f->iPos += n;
  return n;
}
static int32_t jpgSeek(JPEGFILE *f, int32_t pos) {
  f->iPos = pos;
  return pos;
}
static void jpgClose(void *) {}
static int jpgDraw(JPEGDRAW *d) {
  lcd.pushImage(d->x, d->y, d->iWidth, d->iHeight, (const lgfx::swap565_t *)d->pPixels);
  return 1;
}

// --- Image rotation -------------------------------------------------------------
// As in the browser: per camera model (cameraImageRotation(), e.g. -90 for the otoscope,
// whose camera sits turned in the probe). The display turns in quarter turns only;
// CYD_ROTATE_DIR is the direction of setRotation() for +90 degrees.
static int imageRotation() {
  int quarters = (cameraImageRotation() % 360 + 405) / 90 % 4;  // clockwise, rounded: 0..3
  return ((UI_ROT + quarters * CYD_ROTATE_DIR) % 4 + 4) % 4;
}

// --- Display ----------------------------------------------------------------------
enum class Screen { Live, Menu, Choose };
static Screen screen = Screen::Live;
static uint32_t screenSince = 0;
static int lastX = 0, lastY = 0, lastW = 0, lastH = 0, lastRot = -1;  // image geometry
static const int OVL_SIDE_W = 38;  // overlay in the side border: needs this much width
static const int OVL_STRIP = 10;   // otherwise a strip this high above the image
static bool overlaySide = false;
static char overlayShown[40] = "";  // last drawn overlay text
static uint32_t drawnFrames = 0;
static std::atomic<uint32_t> drawMsSum{0}, drawMsMax{0};  // decode + SPI time, for [stats]
static std::atomic<uint32_t> decodeErrors{0};  // JPEGDEC stopped midway: rest of the image is old
static uint32_t lastFrameAt = 0;  // for the "no signal" hint
static float shownFps = 0;
static char statusShown[64] = "";

// JPEGDEC hands over the decoded MCUs of a row in groups (up to MAX_BUFFERED_PIXELS)
// and only draws a group once it is full: with a crop, the last, partly filled group of
// each row was never drawn (MAX-VIEW 1280x720 at 1:1: 21 MCUs per row in groups of 8,
// the right 64 pixels stayed black). So choose a group size that divides the number of
// MCUs per row; if that number has no useful divisor, widen the crop by a few MCUs (the
// clip rectangle hides them). JPEGDEC decodes MCU columns ax/mw rounded up to
// (ax+aw)/mw inclusive, and caps a group at aw/mw MCUs.
static void fitGroups(int W, int ax, int ay, int aw, int ah) {
  int sub = jpeg->getSubSample();
  int mw = (sub >> 4) == 2 ? 16 : 8, mh = (sub & 15) == 2 ? 16 : 8;
  int first = (ax + mw - 1) / mw, cols = (W + mw - 1) / mw;
  int maxGroup = MAX_BUFFERED_PIXELS / (mw * mh);
  int bestGroup = 1, bestW = aw;
  for (int extra = 0; extra < maxGroup && (ax + aw) / mw + extra < cols; extra++) {
    int w = aw + extra * mw;
    int mcus = (ax + w) / mw - first + 1;
    int g = min(maxGroup, w / mw);
    while (g > 1 && mcus % g) g--;
    if (g > bestGroup) {
      bestGroup = g;
      bestW = w;
    }
    if (g * 2 > maxGroup) break;  // good enough
  }
  if (bestW != aw) jpeg->setCropArea(ax, ay, bestW, ah);
  jpeg->setMaxOutputSize(bestGroup);
}

static bool drawFrame(const Frame &f) {
  reader.reset(f);
  if (!jpeg->open(&reader, (int)reader.size(), jpgClose, jpgRead, jpgSeek, jpgDraw)) {
    reader.release();
    return false;
  }
  jpeg->setPixelType(RGB565_BIG_ENDIAN);

  int rot = imageRotation();
  lcd.setRotation(rot);

  int W = jpeg->getWidth(), H = jpeg->getHeight();
  int dw = lcd.width(), dh = lcd.height();
  int w, h, opt, dx, dy;  // visible size, decode option, position for decode()
  if (zoomFull && (W > dw || H > dh)) {
    // 1:1: centre crop. JPEGDEC moves the crop start down to a block edge (8/16
    // pixels) but keeps the width, so the crop would end that many pixels too early
    // (black bar on one side). Hence a second call that starts at the block edge and
    // reaches the wanted right/bottom end. decode() gets the screen position of that
    // block edge, so the image centre lands exactly in the display centre.
    w = min(W, dw);
    h = min(H, dh);
    int cx = (W - w) / 2, cy = (H - h) / 2, ax, ay, aw, ah;
    jpeg->setCropArea(cx, cy, w, h);
    jpeg->getCropArea(&ax, &ay, &aw, &ah);
    jpeg->setCropArea(ax, ay, min(W - ax, cx + w - ax), min(H - ay, cy + h - ay));
    jpeg->getCropArea(&ax, &ay, &aw, &ah);
    dx = dw / 2 - W / 2 + ax;
    dy = dh / 2 - H / 2 + ay;
    // Do not decode the rows above the crop if restart markers allow it: open again as
    // the smaller image that starts there, the crop moves up by as many rows
    if (int skipped = reader.skipAbove(ay)) {
      jpeg->close();
      if (!jpeg->open(&reader, (int)reader.size(), jpgClose, jpgRead, jpgSeek, jpgDraw)) {
        reader.release();
        lcd.setRotation(UI_ROT);
        return false;
      }
      jpeg->setPixelType(RGB565_BIG_ENDIAN);
      ay -= skipped;
      jpeg->setCropArea(ax, ay, aw, ah);
    }
    fitGroups(W, ax, ay, aw, ah);
    opt = 0;
  } else {
    // Fit: largest scale (1, 1/2, 1/4, 1/8) at which the image fits the display
    static const int OPTS[] = {0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH};
    int i = 0;
    while (i < 3 && (W >> i > dw || H >> i > dh)) i++;
    w = W >> i;
    h = H >> i;
    dx = (dw - w) / 2;
    dy = (dh - h) / 2;
    opt = OPTS[i];
  }
  int x = (dw - w) / 2, y = (dh - h) / 2;

  // Room for the overlay (top left in UI orientation): in the side border, in the top
  // border, or else a black strip of OVL_STRIP pixels that the image leaves out. The
  // strip is cut at both opposite edges, so it does not matter in which direction
  // setRotation() turns: one of them is the UI top.
  int turn = ((rot - UI_ROT) % 4 + 4) % 4;
  int uiW = turn & 1 ? dh : dw, uiH = turn & 1 ? dw : dh;  // display in UI orientation
  int imgW = turn & 1 ? h : w, imgH = turn & 1 ? w : h;    // image in UI orientation
  overlaySide = (uiW - imgW) / 2 >= OVL_SIDE_W;
  if (!overlaySide && (uiH - imgH) / 2 < OVL_STRIP) {
    if (turn & 1) {
      int x1 = min(x + w, dw - OVL_STRIP);
      x = max(x, OVL_STRIP);
      w = x1 - x;
    } else {
      int y1 = min(y + h, dh - OVL_STRIP);
      y = max(y, OVL_STRIP);
      h = y1 - y;
    }
  }

  // Log every change of the geometry (to analyse jumps of the image)
  static int geo[8] = {};
  int now[8] = {W, H, rot, zoomFull, dx, dy, w, h};
  if (memcmp(geo, now, sizeof(geo))) {
    memcpy(geo, now, sizeof(geo));
    Serial.printf("[geo] jpeg %dx%d, rot %d, %s, decode at %d,%d, visible %d,%d %dx%d\r\n", W, H, rot,
                  zoomFull ? "1:1" : "fit", dx, dy, x, y, w, h);
  }

  lcd.startWrite();
  if (x != lastX || y != lastY || w != lastW || h != lastH || rot != lastRot) {
    lcd.fillScreen(TFT_BLACK);  // geometry changed -> clear the border
    lastX = x;
    lastY = y;
    lastW = w;
    lastH = h;
    lastRot = rot;
    statusShown[0] = 0;
    overlayShown[0] = 0;
  }
  lcd.setClipRect(x, y, w, h);  // do not paint edge blocks beyond the image
  if (!jpeg->decode(dx, dy, opt)) decodeErrors++;
  lcd.clearClipRect();
  lcd.endWrite();
  jpeg->close();
  lcd.setRotation(UI_ROT);
  reader.release();
  return true;
}

// Battery and fps in the free border (left of square images, above wide ones) or in
// the strip the image leaves out. The image never paints there, so only redraw when
// the text changes (no flicker).
static void drawOverlay() {
  char line1[16] = "", line2[16], shown[40];
  if (telemetry.battery >= 0) snprintf(line1, sizeof(line1), "%d%%", (int)telemetry.battery);
  bool stale = millis() - lastFrameAt > 3000;  // last frame is old
  snprintf(line2, sizeof(line2), stale ? "old" : "%.0ffps", shownFps);
  snprintf(shown, sizeof(shown), "%d|%s|%s", overlaySide, line1, line2);
  if (!strcmp(shown, overlayShown)) return;
  strlcpy(overlayShown, shown, sizeof(overlayShown));
  lcd.setFont(&fonts::Font0);
  lcd.setTextDatum(top_left);
  lcd.setTextPadding(30);  // background behind the whole field: erases longer old text
  lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  if (overlaySide) {
    lcd.drawString(line1, 2, 2);
    lcd.setTextColor(stale ? TFT_RED : TFT_LIGHTGREY, TFT_BLACK);
    lcd.drawString(line2, 2, 12);
  } else {
    lcd.drawString(line1, 2, 1);
    lcd.setTextColor(stale ? TFT_RED : TFT_LIGHTGREY, TFT_BLACK);
    lcd.drawString(line2, 34, 1);
  }
  lcd.setTextPadding(0);
}

static void drawStatus(const char *text) {
  if (!strcmp(text, statusShown)) return;
  strlcpy(statusShown, text, sizeof(statusShown));
  lastW = lastH = 0;
  lastRot = -1;
  lcd.fillScreen(TFT_BLACK);
  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextDatum(middle_center);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.drawString(text, lcd.width() / 2, lcd.height() / 2 - 12);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
  lcd.drawString("Tap: menu", lcd.width() / 2, lcd.height() / 2 + 18);
}

// --- Menu ---------------------------------------------------------------------------
// Laid out from the display size (lcd.width()/height() in UI orientation), so a larger
// display gets larger buttons and more rows in the camera choice. Buttons are found by
// their id, not by their position in the list.
static const int GAP = 6;        // margin and space between buttons
static const int ROW_H = 40;     // height of a row in the camera choice
static const int MAX_NETS = 8;   // networks shown at most (as many as fit)

enum ButtonId : int8_t { B_LED, B_ZOOM, B_CHOOSE, B_BRIGHT, B_BACK, B_RESCAN, B_PROTO, B_NET0 };

struct Button {
  int16_t x, y, w, h;
  int8_t id;
  bool enabled;
};
static Button buttons[B_NET0 + MAX_NETS];
static int buttonCount = 0;

static void addButton(int id, int x, int y, int w, int h, const char *label, bool enabled = true) {
  buttons[buttonCount++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (int8_t)id, enabled};
  lcd.fillRoundRect(x, y, w, h, 8, enabled ? 0x2945 : 0x1082);
  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextDatum(middle_center);
  lcd.setTextColor(enabled ? TFT_WHITE : TFT_DARKGREY);
  char text[40];  // shortened to the button width (long SSIDs)
  strlcpy(text, label, sizeof(text));
  for (size_t n = strlen(text); n > 1 && lcd.textWidth(text) > w - 12;) text[--n] = 0;
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

// Two columns, three rows: LED, zoom / camera, light / back over the whole width
static void showMenu() {
  clearForScreen(Screen::Menu);
  int dw = lcd.width(), dh = lcd.height();
  int w = (dw - 3 * GAP) / 2, h = (dh - 4 * GAP) / 3;
  int x0 = GAP, x1 = 2 * GAP + w;
  auto y = [&](int row) { return GAP + row * (h + GAP); };
  bool led = telemetry.ledSupported;
  char ledLabel[24];
  if (!led) strlcpy(ledLabel, "LED -", sizeof(ledLabel));
  else if (telemetry.ledDimmable && telemetry.led == 1) snprintf(ledLabel, sizeof(ledLabel), "LED %d%%", (int)ledLevel);
  else strlcpy(ledLabel, telemetry.led == 1 ? "LED off" : "LED on", sizeof(ledLabel));
  addButton(B_LED, x0, y(0), w, h, ledLabel, led);
  addButton(B_ZOOM, x1, y(0), w, h, zoomFull ? "Zoom: 1:1" : "Zoom: fit");
  addButton(B_CHOOSE, x0, y(1), w, h, "Camera");
  char bright[24];
  snprintf(bright, sizeof(bright), "Light %d%%", brightness * 100 / 255);
  addButton(B_BRIGHT, x1, y(1), w, h, bright);
  addButton(B_BACK, x0, y(2), dw - 2 * GAP, h, "Back");
}

static ScanEntry nets[MAX_NETS];
static int netCount = 0;
static int protoIndex = 0;  // protoChoice(protoIndex) for the camera choice

static int barY() { return lcd.height() - GAP - ROW_H; }  // row with rescan, protocol, back

// One row per open network (as many as fit above the bottom bar), then the bar
static void showChoose() {
  clearForScreen(Screen::Choose);
  int dw = lcd.width();
  int rows = min(MAX_NETS, (barY() - GAP) / (ROW_H + GAP));
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
    addButton(B_NET0 + i, GAP, GAP + i * (ROW_H + GAP), dw - 2 * GAP, ROW_H, label);
  }
  if (!netCount) {
    lcd.setFont(&fonts::DejaVu18);
    lcd.setTextDatum(middle_center);
    lcd.setTextColor(TFT_LIGHTGREY);
    lcd.drawString("No open networks", dw / 2, barY() / 2);
  }
  int w = (dw - 4 * GAP) / 3;
  addButton(B_RESCAN, GAP, barY(), w, ROW_H, "Rescan");
  addButton(B_PROTO, 2 * GAP + w, barY(), w, ROW_H, protoKey(protoChoice(protoIndex)));  // protocol for the next connect
  addButton(B_BACK, 3 * GAP + 2 * w, barY(), w, ROW_H, "Back");
}

static void showLive() {
  clearForScreen(Screen::Live);
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
      case B_ZOOM: zoomFull = !zoomFull; saveSettings(); return showMenu();
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
      lcd.setFont(&fonts::Font2);
      lcd.setTextDatum(middle_center);
      lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
      lcd.drawString(" scanning... ", lcd.width() / 2, barY() - 12);
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
  for (;;) {
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
      // this the task would never block and IDLE0 would trip the task watchdog
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
static void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    crumb("wifi connected %s, RSSI %d", WiFi.SSID().c_str(), WiFi.RSSI());
    cameraOnWifiGotIp();
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    crumb("wifi disconnected, reason %u", info.wifi_sta_disconnected.reason);
  }
}

void setup() {
  static const int rgb[] = CYD_RGB_LED_PINS;
  for (int pin : rgb) {  // RGB LED off (active LOW)
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
  Serial.begin(115200);
  crashlogInit();
#ifndef GIT_REV
#define GIT_REV "unknown"  // set by git_rev.py
#endif
  Serial.println("\r\n[boot] WiFi-Cam-Viewer (CYD) " GIT_REV);

  loadSettings();
  lcd.init();
  if (auto bus = lcd.getPanel()->getBus(); bus && bus->busType() == lgfx::bus_type_t::bus_spi) {
    auto spi = static_cast<lgfx::Bus_SPI *>(bus);
    auto cfg = spi->config();
    Serial.printf("[lcd] SPI %u -> %u MHz\r\n", (unsigned)(cfg.freq_write / 1000000),
                  (unsigned)(CYD_SPI_WRITE_HZ / 1000000));
    cfg.freq_write = CYD_SPI_WRITE_HZ;
    spi->config(cfg);  // takes effect with the next transaction
  }
  lcd.setRotation(UI_ROT);
  lcd.setBrightness(brightness);
  lcd.fillScreen(TFT_BLACK);
  jpeg = new (std::nothrow) JPEGDEC;
  if (!jpeg) {
    lcd.drawString("No memory for JPEG", 10, 10);
    for (;;) delay(1000);
  }

  Network.onEvent(onNetworkEvent);
  heap_caps_monitor_local_minimum_free_size_start();  // heap minimum per [stats] interval
  cameraBegin();
  xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 1, nullptr, 0);
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
    // Artifacts with "damaged" > 0: Wi-Fi (packet loss). Without: look at draw ms vs.
    // the frame interval of the camera.
    Serial.printf("[stats] received %.1f fps, shown %.1f fps | lost pkts %u, damaged %u, incomplete %u, "
                  "too big %u, no mem %u, released %u, handshakes %u, RSSI %d | draw avg %u ms max %u ms, decode errors %u | battery %d%%%s | "
                  "heap %u (min %u) | largest frame %u KB\r\n",
                  (total - lastFrames) / dt, (drawn - lastDrawn) / dt, lost - lastLost,
                  damaged - lastDamaged, incomplete - lastIncomplete, tooBig - lastTooBig, noMem - lastNoMem,
                  released - lastReleased,
                  (unsigned)stats.handshakes, (int)WiFi.RSSI(),
                  drawn > lastDrawn ? (unsigned)(drawMsSum / (drawn - lastDrawn)) : 0u,
                  (unsigned)drawMsMax, decodeErr - lastDecodeErr, (int)telemetry.battery, telemetry.charging == 1 ? " (charging?)" : "",
                  heapFree(), minNow, (unsigned)(stats.maxFrameBytes / 1024));
    drawMsSum = 0;
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
