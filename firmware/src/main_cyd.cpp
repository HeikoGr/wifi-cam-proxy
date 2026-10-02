/*
 * WiFi-Cam-Viewer für das CYD "Cheap Yellow Display" (ESP32-2432S028R)
 *
 * Statt Ethernet und Webserver zeigt das CYD das Kamerabild direkt auf seinem
 * 320x240-Display. Kameraerkennung, Protokolle und Bildspeicher sind dieselben wie
 * bei der Ethernet-Bridge (camera.cpp, cam_*.cpp, frame.cpp).
 *
 *   Video-Task (Core 1, Prio 10)    empfängt und setzt JPEGs zusammen (wie gehabt)
 *   Display-Task (Core 0, Prio 1)   dekodiert immer das neueste Bild und zeigt es an.
 *                                   Ist er langsamer als die Kamera, fallen Bilder
 *                                   von selbst weg (es gibt nur "das neueste").
 *   loop()                          Kamerasuche und -verbindung (cameraLoop)
 *
 * Zoom "1:1" (Standard): der mittlere Ausschnitt in voller Auflösung, JPEGDEC
 * überspringt die Blöcke außerhalb (setCropArea). Zoom "Ganz": direkt verkleinert
 * dekodiert (480x480 -> 240x240, 1280x720 -> 320x180). Gelesen wird in beiden Fällen
 * aus der Paketliste, ohne das Bild am Stück zu kopieren.
 *
 * Bedienung: Tippen aufs Bild öffnet das Menü (LED, Lagekorrektur, Zoom, Kamera
 * wählen, Helligkeit). Die Lagekorrektur dreht in 90°-Schritten; beliebige Winkel bräuchten
 * einen Bildpuffer, für den ohne PSRAM der Speicher fehlt.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <math.h>

#define LGFX_ESP32_2432S028  // nur die CYD-Varianten erkennen, nicht alle Boards
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>
#include <JPEGDEC.h>

#include "camera.h"
#include "config.h"
#include "crashlog.h"

// Von camera.cpp erwartet. Das CYD hat keinen Notfall-Modus und kein OTA.
volatile bool rescueMode = false;
std::atomic<bool> updating{false};

void wifiApplyMode() {
  // b/g ohne 11n wie bei der Bridge (am Otoskop am robustesten). Die Sendeleistung
  // bleibt auf Maximum: hier gibt es keinen Ethernet-Takt, den sie stören könnte.
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
}

static LGFX lcd;
static JPEGDEC *jpeg = nullptr;  // ~18 KB, einmal angelegt

static const int UI_ROT = 1;  // Querformat 320x240 für Menüs und Touch

// --- Einstellungen (NVS) ---------------------------------------------------------
static bool oriOn = true;     // Lagekorrektur an
static float oriZero = 0;     // Sensorwinkel in der Normallage ("Lage = oben")
static uint8_t brightness = 160;
static bool zoomFull = true;  // 1:1-Ausschnitt statt verkleinertem Gesamtbild

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

// --- JPEG aus der Paketliste lesen ------------------------------------------------
struct FrameReader {
  Frame frame;
  int chunk = 0;      // Stück, in dem die letzte Leseposition lag
  size_t start = 0;   // Byte-Position, an der dieses Stück beginnt
};
static FrameReader reader;

static int32_t jpgRead(JPEGFILE *f, uint8_t *buf, int32_t len) {
  FrameReader *r = (FrameReader *)f->fHandle;
  const Frame &fr = r->frame;
  if ((size_t)f->iPos < r->start) {  // zurückgesprungen -> von vorn suchen
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

// --- Lage -> Drehung in 90°-Schritten -----------------------------------------------
static float norm180(float a) { return fmodf(fmodf(a, 360) + 540, 360) - 180; }

static float sensorAngle() {
  return atan2f((float)telemetry.accX, (float)telemetry.accY) * 180 / M_PI;
}

// Gewünschte Bilddrehung in Grad (im Uhrzeigersinn), wie imageRotation() im Browser:
// Grunddrehung -90° (Einbaulage der Kamera im Otoskop), mit Lagekorrektur -Lage
static float wantedRotation() {
  if (!telemetry.hasOrientation) return 0;  // Mikroskop o.ä.: Bild wie geliefert
  float rot = -90;
  if (oriOn) rot -= norm180(sensorAngle() - oriZero);
  return rot;
}

// Viertelumdrehungen mit Hysterese: erst bei 55° Abweichung umschalten, sonst würde
// das Bild an der 45°-Grenze flackern
static int quarterTurns(float rot, int current) {
  if (current >= 0 && fabsf(norm180(rot - current * 90)) < 55) return current;
  return ((int)lroundf(rot / 90) % 4 + 4) % 4;
}

// --- Anzeige ----------------------------------------------------------------------
enum class Screen { Live, Menu, Choose };
static Screen screen = Screen::Live;
static uint32_t screenSince = 0;
static int quarter = -1;              // aktuelle Drehung des Bildes
static int lastW = 0, lastH = 0, lastRot = -1;  // Bildgeometrie, für Rand löschen
static uint32_t drawnFrames = 0;
static uint32_t lastFrameAt = 0;  // für den Hinweis "kein Signal"
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
  int w, h, opt, dx, dy;  // sichtbare Größe, Dekodier-Option, Position für decode()
  if (zoomFull && (W > dw || H > dh)) {
    // 1:1: mittlerer Ausschnitt. JPEGDEC richtet den Ausschnitt auf ganze Blöcke
    // (8/16 Pixel) aus; decode() bekommt die Bildschirmposition dieser Blockkante,
    // damit die Bildmitte genau in der Displaymitte liegt.
    w = min(W, dw);
    h = min(H, dh);
    jpeg->setCropArea((W - w) / 2, (H - h) / 2, w, h);
    int ax, ay, aw, ah;
    jpeg->getCropArea(&ax, &ay, &aw, &ah);
    dx = dw / 2 - W / 2 + ax;
    dy = dh / 2 - H / 2 + ay;
    opt = 0;
  } else {
    // Ganz: größte Verkleinerung (1, 1/2, 1/4, 1/8), bei der das Bild aufs Display passt
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
  if (w != lastW || h != lastH || rot != lastRot) {  // Geometrie geändert -> Rand löschen
    lcd.fillScreen(TFT_BLACK);
    lastW = w;
    lastH = h;
    lastRot = rot;
    statusShown[0] = 0;
  }
  lcd.setClipRect(x, y, w, h);  // Randblöcke nicht über das Bild hinaus malen
  jpeg->decode(dx, dy, opt);
  lcd.clearClipRect();
  lcd.endWrite();
  jpeg->close();
  lcd.setRotation(UI_ROT);
  reader.frame.reset();
  return true;
}

// Akku und fps im freien Rand (links neben quadratischen, über breiten Bildern),
// bei 1:1 ohne Rand oben links ins Bild (nach jedem Bild neu, sonst übermalt)
static void drawOverlay() {
  char line1[16] = "", line2[16];
  if (telemetry.battery >= 0) snprintf(line1, sizeof(line1), "%d%%", (int)telemetry.battery);
  bool stale = millis() - lastFrameAt > 3000;  // letztes Bild ist alt
  snprintf(line2, sizeof(line2), stale ? "alt " : "%.0ffps", shownFps);
  lcd.setFont(&fonts::Font0);
  lcd.setTextDatum(top_left);
  lcd.setTextColor(stale ? TFT_RED : TFT_LIGHTGREY, TFT_BLACK);
  bool side = lastW < lcd.width() - 30;  // seitlicher Rand breit genug?
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
  lcd.drawString("Tippen: Menue", lcd.width() / 2, lcd.height() / 2 + 18);
}

// --- Menü ---------------------------------------------------------------------------
struct Button {
  int16_t x, y, w, h;
  char label[24];
  bool enabled;
};
static Button buttons[8];  // Menü: 7, Kamerawahl: 4 Netze + 2
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
  addButton(X0, Y[0], W, H, !led ? "LED -" : telemetry.led == 1 ? "LED aus" : "LED an", led);
  addButton(X1, Y[0], W, H, oriOn ? "Lage: an" : "Lage: aus", ori);
  addButton(X0, Y[1], W, H, "Lage = oben", ori && oriOn);
  addButton(X1, Y[1], W, H, zoomFull ? "Zoom: 1:1" : "Zoom: ganz");
  addButton(X0, Y[2], W, H, "Kamera");
  char bright[24];
  snprintf(bright, sizeof(bright), "Licht %d%%", brightness * 100 / 255);
  addButton(X1, Y[2], W, H, bright);
  addButton(X0, Y[3], 308, H, "Zurueck");
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
  // Nur offene Netze (ohne Tastatur kein Passwort), erkannte Kameras zuerst
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
    lcd.drawString("Keine offenen Netze", 160, 90);
  }
  // Feste Plätze für "Suchen"/"Zurück", damit die Indizes der Netze 0..3 bleiben
  while (buttonCount < 4) buttons[buttonCount++] = {0, 0, 0, 0, "", false};
  addButton(6, 194, 152, 40, "Neu suchen");
  addButton(162, 194, 152, 40, "Zurueck");
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
      lcd.drawString("  suche...  ", 160, 182);
    }
    if (b == 5) return showLive();
  }
}

static void displayTask(void *) {
  uint32_t lastSeq = 0, lastTouch = 0, lastOverlay = 0, fpsSince = millis(), fpsFrames = 0;
  bool touching = false;
  for (;;) {
    // Touch: nur auf die Berührung reagieren, nicht aufs Halten
    lgfx::touch_point_t tp;
    bool t = lcd.getTouch(&tp) > 0;
    if (t && !touching && millis() - lastTouch > 300) {
      lastTouch = millis();
      onTouch(tp.x, tp.y);
    }
    touching = t;

    const char *st = cameraStateKey();
    if (screen == Screen::Live && !strcmp(st, "choose")) showChoose();  // mehrere Kameras
    if (screen != Screen::Live) {
      // Menü schließt sich von selbst; die Kameraliste frischt sich nach dem Scan auf
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
      if (drawFrame(f)) {
        drew = true;
        lastFrameAt = millis();
        drawnFrames++;
        fpsFrames++;
      }
    } else if (!f) {
      char text[64];
      char ssid[33];
      cameraCurrentSsid(ssid, sizeof(ssid));
      if (!strcmp(st, "connected")) snprintf(text, sizeof(text), "Warte auf Bild...");
      else if (!strcmp(st, "connecting")) snprintf(text, sizeof(text), "Verbinde %.20s", ssid);
      else snprintf(text, sizeof(text), "Suche Kamera...");
      drawStatus(text);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));  // auf das nächste Bild warten
    }
    f.reset();

    if (millis() - fpsSince >= 2000) {
      shownFps = fpsFrames * 1000.0f / (millis() - fpsSince);
      fpsFrames = 0;
      fpsSince = millis();
    }
    if (drew || millis() - lastOverlay >= 1000) {  // nach jedem Bild, sonst 1x/s
      lastOverlay = millis();
      drawOverlay();
    }
  }
}

// --- Start ------------------------------------------------------------------------
static void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    crumb("wifi verbunden %s, RSSI %d", WiFi.SSID().c_str(), WiFi.RSSI());
    cameraOnWifiGotIp();
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    crumb("wifi getrennt, Grund %u", info.wifi_sta_disconnected.reason);
  }
}

void setup() {
  static const int rgb[] = CYD_RGB_LED_PINS;
  for (int pin : rgb) {  // RGB-LED aus (active LOW)
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }
  Serial.begin(115200);
  crashlogInit();
  Serial.println("\n[boot] WiFi-Cam-Viewer (CYD)");

  loadSettings();
  lcd.init();
  lcd.setRotation(UI_ROT);
  lcd.setBrightness(brightness);
  lcd.fillScreen(TFT_BLACK);
  jpeg = new (std::nothrow) JPEGDEC;
  if (!jpeg) {
    lcd.drawString("Kein Speicher fuer JPEG", 10, 10);
    for (;;) delay(1000);
  }

  Network.onEvent(onNetworkEvent);
  cameraBegin();
  xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  cameraLoop();
  static uint32_t lastStats = 0, lastFrames = 0, lastDrawn = 0;
  if (millis() - lastStats >= 5000) {
    uint32_t total = stats.framesTotal;
    float dt = (millis() - lastStats) / 1000.0f;
    Serial.printf("[stats] empfangen %.1f fps, angezeigt %.1f fps, Heap %u (min %u)\n",
                  (total - lastFrames) / dt, (drawnFrames - lastDrawn) / dt, heapFree(), heapMin());
    lastFrames = total;
    lastDrawn = drawnFrames;
    lastStats = millis();
  }
  delay(10);
}
