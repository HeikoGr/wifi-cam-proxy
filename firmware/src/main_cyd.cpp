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
 * Usage: tapping the image opens the menu (LED, orientation correction, zoom, choose
 * camera, brightness). Orientation correction rotates in 90° steps; arbitrary angles
 * would need a frame buffer, for which there is not enough memory without PSRAM.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <math.h>

#define LGFX_ESP32_2432S028  // detect only the CYD variants, not all boards
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>
#include <JPEGDEC.h>

#include "camera.h"
#include "config.h"
#include "crashlog.h"

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
static bool oriOn = true;     // orientation correction on
static float oriZero = 0;     // sensor angle in the normal position ("Set upright")
static uint8_t brightness = 160;
static bool zoomFull = true;  // 1:1 crop instead of the reduced full image

static void loadSettings() {
  Preferences p;
  if (p.begin("otoskop", true)) {
    oriOn = p.getBool("cyd_ori", true);
    oriZero = p.getFloat("cyd_zero", 0);
    brightness = p.getUChar("cyd_bright", 160);
    zoomFull = p.getBool("cyd_zoom", true);
    p.end();
  }
}

static void saveSettings() {
  Preferences p;
  if (p.begin("otoskop", false)) {
    p.putBool("cyd_ori", oriOn);
    p.putFloat("cyd_zero", oriZero);
    p.putUChar("cyd_bright", brightness);
    p.putBool("cyd_zoom", zoomFull);
    p.end();
  }
}

// --- Read the JPEG from the packet list ---------------------------------------------
struct FrameReader {
  Frame frame;
  int chunk = 0;      // chunk containing the last read position
  size_t start = 0;   // byte position at which this chunk starts
};
static FrameReader reader;

static int32_t jpgRead(JPEGFILE *f, uint8_t *buf, int32_t len) {
  FrameReader *r = (FrameReader *)f->fHandle;
  const Frame &fr = r->frame;
  if ((size_t)f->iPos < r->start) {  // jumped back -> search from the start
    r->chunk = 0;
    r->start = 0;
  }
  int32_t done = 0;
  while (done < len && f->iPos < f->iSize) {
    while (r->chunk < fr.chunks() && (size_t)f->iPos >= r->start + fr.chunkLen(r->chunk)) {
      r->start += fr.chunkLen(r->chunk);
      r->chunk++;
    }
    if (r->chunk >= fr.chunks()) break;
    size_t off = f->iPos - r->start;
    size_t k = min((size_t)(len - done), fr.chunkLen(r->chunk) - off);
    copyFromChunk(buf + done, fr.chunk(r->chunk), off, k);
    done += k;
    f->iPos += k;
  }
  return done;
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

// --- Orientation -> rotation in 90° steps --------------------------------------------
static float norm180(float a) { return fmodf(fmodf(a, 360) + 540, 360) - 180; }

static float sensorAngle() {
  return atan2f((float)telemetry.accX, (float)telemetry.accY) * 180 / M_PI;
}

// Desired image rotation in degrees (clockwise), like imageRotation() in the browser:
// base rotation -90° (mounting of the camera in the otoscope), with correction -angle
static float wantedRotation() {
  if (!telemetry.hasOrientation) return 0;  // microscope etc.: image as delivered
  float rot = -90;
  if (oriOn) rot -= norm180(sensorAngle() - oriZero);
  return rot;
}

// Quarter turns with hysteresis: switch only at 55° deviation, otherwise the image
// would flicker at the 45° boundary
static int quarterTurns(float rot, int current) {
  if (current >= 0 && fabsf(norm180(rot - current * 90)) < 55) return current;
  return ((int)lroundf(rot / 90) % 4 + 4) % 4;
}

// --- Display ----------------------------------------------------------------------
enum class Screen { Live, Menu, Choose };
static Screen screen = Screen::Live;
static uint32_t screenSince = 0;
static int quarter = -1;              // current rotation of the image
static int lastW = 0, lastH = 0, lastRot = -1;  // image geometry, for clearing the border
static uint32_t drawnFrames = 0;
static std::atomic<uint32_t> drawMsSum{0}, drawMsMax{0};  // decode + SPI time, for [stats]
static uint32_t lastFrameAt = 0;  // for the "no signal" hint
static float shownFps = 0;
static char statusShown[64] = "";

static bool drawFrame(const Frame &f) {
  reader.frame = f;
  reader.chunk = 0;
  reader.start = 0;
  if (!jpeg->open(&reader, (int)f.size(), jpgClose, jpgRead, jpgSeek, jpgDraw)) {
    reader.frame.reset();
    return false;
  }
  jpeg->setPixelType(RGB565_BIG_ENDIAN);

  quarter = quarterTurns(wantedRotation(), quarter);
  int rot = (UI_ROT + CYD_ROTATE_DIR * quarter + 8) % 4;
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

  lcd.startWrite();
  if (w != lastW || h != lastH || rot != lastRot) {  // geometry changed -> clear the border
    lcd.fillScreen(TFT_BLACK);
    lastW = w;
    lastH = h;
    lastRot = rot;
    statusShown[0] = 0;
  }
  lcd.setClipRect(x, y, w, h);  // do not paint edge blocks beyond the image
  jpeg->decode(dx, dy, opt);
  lcd.clearClipRect();
  lcd.endWrite();
  jpeg->close();
  lcd.setRotation(UI_ROT);
  reader.frame.reset();
  return true;
}

// Battery and fps in the free border (left of square images, above wide ones); at
// 1:1 without a border, top left into the image (redrawn after every frame)
static void drawOverlay() {
  char line1[16] = "", line2[16];
  if (telemetry.battery >= 0) snprintf(line1, sizeof(line1), "%d%%", (int)telemetry.battery);
  bool stale = millis() - lastFrameAt > 3000;  // last frame is old
  snprintf(line2, sizeof(line2), stale ? "old " : "%.0ffps", shownFps);
  lcd.setFont(&fonts::Font0);
  lcd.setTextDatum(top_left);
  lcd.setTextColor(stale ? TFT_RED : TFT_LIGHTGREY, TFT_BLACK);
  bool side = lastW < lcd.width() - 30;  // side border wide enough?
  if (side) {
    lcd.fillRect(0, 0, 38, 20, TFT_BLACK);
    lcd.drawString(line1, 2, 2);
    lcd.drawString(line2, 2, 12);
  } else {
    lcd.fillRect(0, 0, 76, 10, TFT_BLACK);
    lcd.drawString(line1, 2, 1);
    lcd.drawString(line2, 34, 1);
  }
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
struct Button {
  int16_t x, y, w, h;
  char label[24];
  bool enabled;
};
static Button buttons[8];  // menu: 7, camera choice: 4 networks + 2
static int buttonCount = 0;

static void addButton(int x, int y, int w, int h, const char *label, bool enabled = true) {
  Button &b = buttons[buttonCount++];
  b = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, "", enabled};
  strlcpy(b.label, label, sizeof(b.label));
  lcd.fillRoundRect(x, y, w, h, 8, enabled ? 0x2945 : 0x1082);
  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextDatum(middle_center);
  lcd.setTextColor(enabled ? TFT_WHITE : TFT_DARKGREY);
  lcd.drawString(label, x + w / 2, y + h / 2);
}

static int hitButton(int tx, int ty) {
  for (int i = 0; i < buttonCount; i++) {
    const Button &b = buttons[i];
    if (b.enabled && tx >= b.x && tx < b.x + b.w && ty >= b.y && ty < b.y + b.h) return i;
  }
  return -1;
}

enum { B_LED, B_ORI, B_ZERO, B_ZOOM, B_CHOOSE, B_BRIGHT, B_BACK };

static void showMenu() {
  screen = Screen::Menu;
  screenSince = millis();
  statusShown[0] = 0;
  lastW = lastH = 0;
  lastRot = -1;
  lcd.fillScreen(TFT_BLACK);
  buttonCount = 0;
  bool led = telemetry.ledSupported, ori = telemetry.hasOrientation;
  const int W = 152, H = 52, X0 = 6, X1 = 162, Y[] = {6, 64, 122, 180};
  addButton(X0, Y[0], W, H, !led ? "LED -" : telemetry.led == 1 ? "LED off" : "LED on", led);
  addButton(X1, Y[0], W, H, oriOn ? "Rotate: on" : "Rotate: off", ori);
  addButton(X0, Y[1], W, H, "Set upright", ori && oriOn);
  addButton(X1, Y[1], W, H, zoomFull ? "Zoom: 1:1" : "Zoom: fit");
  addButton(X0, Y[2], W, H, "Camera");
  char bright[24];
  snprintf(bright, sizeof(bright), "Light %d%%", brightness * 100 / 255);
  addButton(X1, Y[2], W, H, bright);
  addButton(X0, Y[3], 308, H, "Back");
}

static ScanEntry nets[5];
static int netCount = 0;

static void showChoose() {
  screen = Screen::Choose;
  screenSince = millis();
  statusShown[0] = 0;
  lastW = lastH = 0;
  lastRot = -1;
  lcd.fillScreen(TFT_BLACK);
  buttonCount = 0;
  // Only open networks (no keyboard, no password), recognised cameras first
  ScanEntry all[16];
  int n = cameraNetworks(all, 16);
  netCount = 0;
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < n && netCount < 4; i++)
      if (all[i].open && (all[i].proto != CamProto::None) == (pass == 0)) nets[netCount++] = all[i];
  char cur[33];
  cameraCurrentSsid(cur, sizeof(cur));
  for (int i = 0; i < netCount; i++) {
    char label[24];
    snprintf(label, sizeof(label), "%s%.20s", strcmp(nets[i].ssid, cur) ? "" : "> ", nets[i].ssid);
    addButton(6, 6 + i * 46, 308, 40, label, true);
  }
  if (!netCount) {
    lcd.setFont(&fonts::DejaVu18);
    lcd.setTextDatum(middle_center);
    lcd.setTextColor(TFT_LIGHTGREY);
    lcd.drawString("No open networks", 160, 90);
  }
  // Fixed slots for "Rescan"/"Back" so the network indices stay 0..3
  while (buttonCount < 4) buttons[buttonCount++] = {0, 0, 0, 0, "", false};
  addButton(6, 194, 152, 40, "Rescan");
  addButton(162, 194, 152, 40, "Back");
}

static void showLive() {
  screen = Screen::Live;
  statusShown[0] = 0;
  lastW = lastH = 0;
  lastRot = -1;
  lcd.fillScreen(TFT_BLACK);
}

static void onTouch(int tx, int ty) {
  if (screen == Screen::Live) return showMenu();
  int b = hitButton(tx, ty);
  if (screen == Screen::Menu) {
    switch (b) {
      case B_LED: ledRequest = telemetry.led == 1 ? 0 : 1; return showLive();
      case B_ORI: oriOn = !oriOn; saveSettings(); return showMenu();
      case B_ZERO: oriZero = sensorAngle(); quarter = -1; saveSettings(); return showLive();
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
    if (b >= 0 && b < netCount) {
      cameraSelect(nets[b].ssid, "", CamProto::Auto);
      return showLive();
    }
    if (b == 4) {
      cameraRequestScan();
      screenSince = millis();
      lcd.setFont(&fonts::Font2);
      lcd.setTextDatum(middle_center);
      lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
      lcd.drawString(" scanning... ", 160, 182);
    }
    if (b == 5) return showLive();
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
  Serial.println("\r\n[boot] WiFi-Cam-Viewer (CYD)");

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
  cameraBegin();
  xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  cameraLoop();
  static uint32_t lastStats = 0, lastFrames = 0, lastDrawn = 0, lastLost = 0, lastDamaged = 0,
                  lastIncomplete = 0;
  if (millis() - lastStats >= 5000) {
    uint32_t total = stats.framesTotal, drawn = drawnFrames, lost = stats.packetsLost,
             damaged = stats.framesDamaged, incomplete = stats.dropIncomplete;
    float dt = (millis() - lastStats) / 1000.0f;
    // Artifacts with "damaged" > 0: Wi-Fi (packet loss). Without: look at draw ms vs.
    // the frame interval of the camera.
    Serial.printf("[stats] received %.1f fps, shown %.1f fps | lost pkts %u, damaged %u, incomplete %u, "
                  "RSSI %d | draw avg %u ms max %u ms | battery %d%%%s | heap %u (min %u)\r\n",
                  (total - lastFrames) / dt, (drawn - lastDrawn) / dt, lost - lastLost,
                  damaged - lastDamaged, incomplete - lastIncomplete, (int)WiFi.RSSI(),
                  drawn > lastDrawn ? (unsigned)(drawMsSum / (drawn - lastDrawn)) : 0u,
                  (unsigned)drawMsMax, (int)telemetry.battery, telemetry.charging == 1 ? " (charging?)" : "",
                  heapFree(), heapMin());
    drawMsSum = 0;
    drawMsMax = 0;
    lastFrames = total;
    lastDrawn = drawn;
    lastLost = lost;
    lastDamaged = damaged;
    lastIncomplete = incomplete;
    lastStats = millis();
  }
  delay(10);
}
