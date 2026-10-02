/*
 * WiFi-Cam-Proxy für ESP32 + LAN8720 (ZB-GW03 v1.4, WT32-ETH01)
 *
 * - WLAN (Client): sucht WLAN-Kameras (Otoskope, Mikroskope) am SSID-Namen und
 *   verbindet sich mit einer davon (src/camera.cpp, Protokolle in src/cam_*.cpp)
 * - Ethernet (DHCP): stellt im Heimnetz einen MJPEG-Server bereit
 *     http://otoskop.local/          Browser
 *     http://otoskop.local/stream    VLC / Home Assistant
 *     http://otoskop.local/snapshot  Einzelbild
 *     http://otoskop.local/cameras   Kamera wählen
 *     http://otoskop.local/status    JSON mit Statistik
 *     http://otoskop.local/update    Firmware-Update im Browser
 *
 * Notfall-Modus (src/rescue.cpp): Hat Ethernet RESCUE_TIMEOUT_MS lang keine IP, geht
 * das WLAN ins Heim-WLAN oder öffnet einen eigenen Access Point mit Einrichtungsseite.
 */

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ETH.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <soc/emac_dma_struct.h>
#include <lwip/sockets.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <new>

#include "camera.h"
#include "config.h"
#include "crashlog.h"
#include "rescue.h"

static const char FW_VERSION[] = __DATE__ " " __TIME__;

// Überlebt Neustarts (nicht aber Stromausfall) -> zählt Resets seit dem Einschalten
RTC_NOINIT_ATTR static uint32_t bootMagic;
RTC_NOINIT_ATTR static uint32_t bootCount;

static const char *resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_EXT: return "ext";
    default: return "unknown";
  }
}
// --- Status ---------------------------------------------------------------------
static volatile bool ethUp = false;
static volatile bool ethStarted = false;  // LAN8720 initialisiert
static bool ethBeginOk = false;
volatile bool rescueMode = false;
std::atomic<bool> updating{false};
static std::atomic<int> streamClients{0};
static std::atomic<int> sseClients{0};
// WLAN-Modus zur Kamera (Index in WIFI_MODES), siehe wifiApplyMode()
static const char *const WIFI_MODES[] = {"bgn", "bg", "b"};
static int wifiModeIndex(const String &m) {
  for (int i = 0; i < 3; i++)
    if (m == WIFI_MODES[i]) return i;
  return -1;
}
static std::atomic<int> wifiMode{wifiModeIndex(WIFI_MODE_DEFAULT)};
// WLAN-Sendeleistung in 0,25 dBm (8..84). Hohe Leistung stört den Ethernet-Takt, den
// der ESP32 selbst auf GPIO17 erzeugt -> verlorene Ethernet-Pakete (am Gerät gemessen)
static std::atomic<int> wifiTxQdbm{WIFI_TX_QDBM_DEFAULT};

static std::atomic<bool> eth10{ETH_10MBIT_DEFAULT};  // Ethernet nur 10 Mbit, siehe ethApplySpeed()
static void ethApplySpeed();

// Zigbee-Modul (nur ZB-GW03) wird nicht gebraucht: im Reset halten, spart Strom
static void setZigbee(bool on) {
  if (ZIGBEE_NRST_GPIO < 0) return;
  pinMode(ZIGBEE_NRST_GPIO, OUTPUT);
  digitalWrite(ZIGBEE_NRST_GPIO, on ? HIGH : LOW);
}

static std::atomic<int> clientTasks{0};
static float currentFps = 0;

#include "web_ui.h"  // Startseite, Kalibrierung, Update-Seite, gemeinsames JS

// --- HTTP-Server ----------------------------------------------------------------
// Nicht blockierend senden und selbst nach 5 ms erneut versuchen. Ein blockierendes
// send() schläft bei kurzem Speichermangel in lwIP (ERR_MEM) bis zum nächsten
// Abfrage-Timer der Verbindung, und der läuft nur ~1x pro Sekunde -> 1 s Standbild
// (gemessen: alles bestätigt, Fenster offen, trotzdem Sendepause).
static bool sendAll(int fd, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t lastProgress = millis();
  while (len > 0) {
    int n = send(fd, p, len, MSG_DONTWAIT);
    if (n > 0) {
      p += n;
      len -= n;
      lastProgress = millis();
      continue;
    }
    bool busy = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM);
    if (!busy || millis() - lastProgress > 5000) return false;  // Fehler oder 5 s nichts
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return true;
}

static void sendResponse(int fd, int code, const char *reason, const char *ctype,
                         const void *body, size_t len) {
  char hdr[192];
  int h = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                   "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
                   code, reason, ctype, (unsigned)len);
  if (sendAll(fd, hdr, h)) sendAll(fd, body, len);
}

static void sendText(int fd, int code, const char *reason, const char *text) {
  sendResponse(fd, code, reason, "text/plain; charset=utf-8", text, strlen(text));
}

// Wert eines Request-Headers (ohne Groß-/Kleinschreibung), "" wenn nicht vorhanden
static String headerValue(const char *req, const char *name) {
  size_t nlen = strlen(name);
  for (const char *line = strstr(req, "\r\n"); line; line = strstr(line, "\r\n")) {
    line += 2;
    if (strncasecmp(line, name, nlen) == 0 && line[nlen] == ':') {
      const char *v = line + nlen + 1;
      while (*v == ' ') v++;
      const char *e = strstr(v, "\r\n");
      return e ? String(v).substring(0, e - v) : String(v);
    }
  }
  return String();
}

static bool authorized(const char *req) {
  return strlen(OTA_PASSWORD) == 0 || headerValue(req, "X-OTA-Password") == OTA_PASSWORD;
}

// Sammelt kleine Stücke und sendet sie in Blöcken. Jeder send() ist ein Auftrag an den
// lwIP-Task, der auch die WLAN-Pakete vom Otoskop verarbeitet; ~30 Aufträge pro Bild
// bremsen dort den Empfang. Offen: bei hoher Datenrate gehen einzelne TCP-Segmente auf
// dem Weg ins LAN verloren (vermutlich Sendepuffer des Ethernet-Controllers, 10 x 512
// Byte); TCP wiederholt sie erst nach ~1 s. Die Blockgröße ändert daran nichts.
static const size_t SEND_BLOCK = SEND_BLOCK_SEGMENTS * 1436;

struct BlockSender {
  int fd;
  uint8_t *buf;
  size_t len = 0;
  bool ok = true;

  explicit BlockSender(int fd) : fd(fd), buf((uint8_t *)malloc(SEND_BLOCK)) {}
  ~BlockSender() { free(buf); }

  void put(const void *data, size_t n) {
    if (!ok) return;
    if (!buf) {  // kein Speicher für den Puffer -> direkt senden
      ok = sendAll(fd, data, n);
      return;
    }
    const uint8_t *p = (const uint8_t *)data;
    while (n > 0 && ok) {
      size_t k = min(n, SEND_BLOCK - len);
      memcpy(buf + len, p, k);
      len += k;
      p += k;
      n -= k;
      if (len == SEND_BLOCK) flush();
    }
  }
  // Bildstück, das evtl. im IRAM liegt: wortweise lesen, byteweise weitergeben
  __attribute__((noinline)) void putChunk(const uint8_t *src, size_t n) {
    if (!esp_ptr_in_iram(src)) return put(src, n);
    const volatile uint32_t *s = (const volatile uint32_t *)src;
    uint8_t tmp[64];
    for (size_t done = 0; done < n && ok;) {
      size_t k = min(n - done, sizeof(tmp));
      for (size_t i = 0; i < k; i += 4) {
        uint32_t w = s[(done + i) / 4];
        memcpy(tmp + i, &w, 4);
      }
      put(tmp, k);
      done += k;
    }
  }
  bool flush() {
    if (ok && len > 0) ok = sendAll(fd, buf, len);
    len = 0;
    return ok;
  }
};

static void putFrame(BlockSender &out, const Frame &frame) {
  for (int i = 0; i < frame.chunks(); i++) out.putChunk(frame.chunk(i), frame.chunkLen(i));
}

static bool clientClosed(int fd) {
  char c;
  int n = recv(fd, &c, 1, MSG_DONTWAIT);
  return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
}

static void handleStream(int fd) {
  if (++streamClients > MAX_STREAM_CLIENTS) {
    streamClients--;
    sendText(fd, 503, "Service Unavailable", "Zu viele Zuschauer");
    return;
  }
  static const char hdr[] =
      "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
      "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
  crumb("stream auf (%d Zuschauer)", (int)streamClients);
  if (sendAll(fd, hdr, sizeof(hdr) - 1)) {
    uint32_t seq = 0;
    uint32_t lastCheck = millis();
    BlockSender out(fd);  // ein Puffer pro Zuschauer, für alle Bilder
    Frame frame;
    while (!updating) {
      uint32_t s = getFrame(frame);
      if (s == seq || !frame) {
        frame.reset();
        // Beim Warten gelegentlich prüfen, ob der Client noch da ist
        if (millis() - lastCheck > 1000) {
          if (clientClosed(fd)) break;
          lastCheck = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      seq = s;
      if (clientClosed(fd)) {
        frame.reset();
        break;
      }
      char part[96];
      int h = snprintf(part, sizeof(part),
                       "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                       (unsigned)frame.size());
      out.put(part, h);
      putFrame(out, frame);
      out.put("\r\n", 2);
      frame.reset();
      if (!out.flush()) break;
    }
  }
  streamClients--;
  crumb("stream zu (%d Zuschauer)", (int)streamClients);
}

static float wifiTxDbm() {
  int8_t q = 0;
  return esp_wifi_get_max_tx_power(&q) == ESP_OK ? q / 4.0f : 0;
}

static void handleStatus(int fd) {
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  char crash[320];
  crashlogFormat(crash, sizeof(crash));
  // Zeitpunkte der letzten sauberen Stillstände, älteste zuerst
  char times[96] = "";
  uint32_t nClean = stats.stallsClean;
  uint32_t shown = min(nClean, (uint32_t)VideoStats::CLEAN_STALL_TIMES);
  for (uint32_t k = 0, t = 0; k < shown; k++)
    t += snprintf(times + t, sizeof(times) - t, "%s%lu", k ? "," : "",
                  (unsigned long)stats.cleanStallAt[(nClean - shown + k) % VideoStats::CLEAN_STALL_TIMES]);
  char home[33], ap[33] = "";
  rescueHomeSsid(home, sizeof(home));
  for (char *c = home; *c; c++)
    if (*c == '"' || *c == '\\' || (uint8_t)*c < 0x20) *c = '?';  // JSON-sicher
  if (rescueApActive()) rescueApSsid(ap, sizeof(ap));
  char json[1536];
  int n = snprintf(json, sizeof(json),
                   "{\"version\":\"%s\",\"reset_reason\":\"%s\",\"boot_count\":%u,\"mode\":\"%s\",\"fps\":%.1f,\"frames\":%u,"
                   "\"dropped\":%u,\"drop_nomem\":%u,\"drop_toobig\":%u,\"drop_incomplete\":%u,\"packets_lost\":%u,\"damaged\":%u,\"max_frame\":%u,\"handshakes\":%u,\"keepalives\":%u,\"stalls_loss\":%u,\"stalls_clean\":%u,\"clean_stall_times\":[%s],\"stream_clients\":%d,\"sse_clients\":%d,\"client_tasks\":%d,"
                   "\"wifi_connected\":%s,\"wifi_ssid\":\"%s\",\"wifi_rssi\":%d,\"wifi_mode\":\"%s\",\"wifi_tx_dbm\":%.2f,\"wifi_ip\":\"%s\",\"home_ssid\":\"%s\",\"ap\":\"%s\",\"eth10\":%d,"
                   "\"cam_proto\":\"%s\",\"battery\":%d,\"led\":%d,"
                   "\"eth_begin\":%s,\"eth_started\":%s,\"eth_link\":%s,\"eth_speed\":%d,\"eth_full_duplex\":%s,\"eth_tx_store_forward\":%d,\"eth_ip\":\"%s\",\"free_heap\":%u,\"max_alloc\":%u,\"min_heap\":%u,\"iram_heap\":%u,\"psram\":%u,\"uptime_s\":%lu,\"last_crash\":\"%s\"}",
                   FW_VERSION, resetReasonText(), (unsigned)bootCount, rescueMode ? "rescue" : "normal", currentFps,
                   (unsigned)stats.framesTotal, (unsigned)stats.framesDropped, (unsigned)stats.dropNoMem,
                   (unsigned)stats.dropTooBig, (unsigned)stats.dropIncomplete, (unsigned)stats.packetsLost, (unsigned)stats.framesDamaged,
                   (unsigned)stats.maxFrameBytes, (unsigned)stats.handshakes, (unsigned)stats.keepalives, (unsigned)stats.stallsLoss, (unsigned)stats.stallsClean, times,
                   (int)streamClients, (int)sseClients, (int)clientTasks, wifiOk ? "true" : "false",
                   wifiOk ? WiFi.SSID().c_str() : "", wifiOk ? WiFi.RSSI() : 0, WIFI_MODES[wifiMode], wifiTxDbm(),
                   wifiOk ? WiFi.localIP().toString().c_str() : "", home, ap, (int)eth10,
                   protoKey(cameraProto()), (int)telemetry.battery, (int)telemetry.led,
                   ethBeginOk ? "true" : "false", ethStarted ? "true" : "false",
                   ethStarted && ETH.linkUp() ? "true" : "false",
                   ethStarted && ETH.linkUp() ? (int)ETH.linkSpeed() : 0,
                   ethStarted && ETH.linkUp() && ETH.fullDuplex() ? "true" : "false",
                   ethStarted ? (int)EMAC_DMA.dmaoperation_mode.tx_str_fwd : -1,
                   ethUp ? ETH.localIP().toString().c_str() : "", heapFree(), heapBlock(),
                   heapMin(), iramFree(), (unsigned)ESP.getPsramSize(), millis() / 1000, crash);
  sendResponse(fd, 200, "OK", "application/json", json, n);
}

// Firmware als Rohdaten im Body (application/octet-stream), z.B. aus der
// Update-Seite oder per: curl --data-binary @firmware.bin http://otoskop.local/update
static void handleUpdate(int fd, const char *req, const uint8_t *body, size_t bodyLen) {
  if (!authorized(req)) return sendText(fd, 401, "Unauthorized", "Falsches OTA-Passwort");
  size_t total = headerValue(req, "Content-Length").toInt();
  if (total == 0) return sendText(fd, 411, "Length Required", "Content-Length fehlt");
  if (updating.exchange(true)) return sendText(fd, 409, "Conflict", "Update läuft bereits");

  Serial.printf("[update] Web-Update, %u Byte\n", (unsigned)total);
  const char *err = nullptr;
  if (!Update.begin(total, U_FLASH)) {
    err = Update.errorString();
  } else {
    size_t done = 0;
    if (bodyLen > 0) {
      if (Update.write((uint8_t *)body, bodyLen) != bodyLen) err = Update.errorString();
      done = bodyLen;
    }
    std::unique_ptr<uint8_t[]> chunk(new (std::nothrow) uint8_t[2048]);
    if (!chunk) err = "Kein Speicher frei";
    while (!err && done < total) {
      int n = recv(fd, chunk.get(), min((size_t)2048, total - done), 0);
      if (n <= 0) {
        err = "Verbindung abgebrochen";
        break;
      }
      if (Update.write(chunk.get(), n) != (size_t)n) err = Update.errorString();
      done += n;
    }
    if (!err && !Update.end()) err = Update.errorString();
  }

  if (err) {
    Update.abort();
    updating = false;
    Serial.printf("[update] Fehler: %s\n", err);
    char msg[128];
    snprintf(msg, sizeof(msg), "Update fehlgeschlagen: %s", err);
    return sendText(fd, 500, "Internal Server Error", msg);
  }
  Serial.println("[update] OK, Neustart");
  sendText(fd, 200, "OK", "Update erfolgreich, Gerät startet neu");
  delay(500);
  ESP.restart();
}

// Mitschnitt pro Bild als Text: ms, Größe, RSSI, verlorene Pakete, Kopf-Bytes
static void handleOrientationLoop(int fd);

// --- WLAN-Modus zur Kamera (umschaltbar über die Update-Seite, im NVS) -----------
// "bgn": 802.11n mit Paket-Bündelung (schnell, aber ein fehlendes Teilpaket hält den
//        ganzen Block auf); "bg": ohne 11n, jedes Paket einzeln (Standard);
// "b":   nur 802.11b, langsam (max. 11 Mbit/s), dafür am robustesten bei schwachem Signal
void wifiApplyMode() {
  int mode = wifiMode;
  uint8_t proto = mode == 2   ? WIFI_PROTOCOL_11B
                  : mode == 1 ? (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G)
                              : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  esp_wifi_set_protocol(WIFI_IF_STA, proto);
  if (mode == 0) esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);  // 20 statt 40 MHz
  esp_wifi_set_max_tx_power(wifiTxQdbm);
}

static void loadWifiMode() {
  Preferences p;
  if (p.begin("otoskop", true)) {
    int i = wifiModeIndex(p.getString("wifimode", WIFI_MODE_DEFAULT));
    int tx = p.getInt("wifitx", WIFI_TX_QDBM_DEFAULT);
    eth10 = p.getBool("eth10", ETH_10MBIT_DEFAULT);
    p.end();
    if (i >= 0) wifiMode = i;
    if (tx >= 8 && tx <= 84) wifiTxQdbm = tx;
  }
}

static void handleWifiModePost(int fd, const char *path) {
  if (strncmp(path, "/wifi/tx/", 9) == 0) {  // Sendeleistung, wirkt sofort
    int tx = atoi(path + 9);
    if (tx < 8 || tx > 84) return sendText(fd, 400, "Bad Request", "Sendeleistung 8..84 (x 0,25 dBm)");
    Preferences p;
    if (p.begin("otoskop", false)) {
      p.putInt("wifitx", tx);
      p.end();
    }
    wifiTxQdbm = tx;
    esp_wifi_set_max_tx_power(tx);
    crumb("WLAN-Sendeleistung -> %.2f dBm", tx / 4.0);
    return sendText(fd, 200, "OK", "Sendeleistung gesetzt");
  }
  String m = String(path + strlen("/wifi/"));
  int i = wifiModeIndex(m);
  if (i < 0) return sendText(fd, 400, "Bad Request", "Modus: bgn, bg oder b");
  Preferences p;
  if (p.begin("otoskop", false)) {
    p.putString("wifimode", m);
    p.end();
  }
  wifiMode = i;
  crumb("WLAN-Modus -> %s, verbinde neu", m.c_str());
  sendText(fd, 200, "OK", "WLAN-Modus gesetzt, verbinde neu");
  // Der Protokollwechsel greift erst bei einer neuen Verbindung
  if (!rescueMode) cameraRestartWifi();  // im Notfall-Modus erst beim nächsten Normalbetrieb
}

// --- Kalibrierung der Lage (JSON, von den Webseiten berechnet, im NVS gespeichert) --
static const size_t CALIB_MAX = 1024;
static std::mutex calibMutex;
static String calibJson = "{}";

static void loadCalibration() {
  Preferences p;
  if (p.begin("otoskop", true)) {
    calibJson = p.getString("calib", "{}");
    p.end();
  }
}

static void handleCalibrationPost(int fd, const char *req, const uint8_t *body, size_t bodyLen) {
  size_t total = headerValue(req, "Content-Length").toInt();
  if (total == 0 || total > CALIB_MAX) return sendText(fd, 413, "Payload Too Large", "Ungültige Länge");
  char buf[CALIB_MAX + 1];
  size_t have = min(bodyLen, total);
  memcpy(buf, body, have);
  while (have < total) {
    int n = recv(fd, buf + have, total - have, 0);
    if (n <= 0) return sendText(fd, 400, "Bad Request", "Verbindung abgebrochen");
    have += n;
  }
  buf[total] = 0;
  if (buf[0] != '{' || buf[total - 1] != '}') return sendText(fd, 400, "Bad Request", "Kein JSON-Objekt");

  Preferences p;
  bool ok = p.begin("otoskop", false) && p.putString("calib", buf) > 0;
  p.end();
  if (!ok) return sendText(fd, 500, "Internal Server Error", "Speichern im NVS fehlgeschlagen");
  {
    std::lock_guard<std::mutex> lock(calibMutex);
    calibJson = buf;
  }
  crumb("Kalibrierung gespeichert (%u Byte)", (unsigned)total);
  sendText(fd, 200, "OK", "Gespeichert");
}

// Lage als Server-Sent Events: eine offene Verbindung, neue Werte ~17x pro Sekunde
static void handleOrientation(int fd) {
  sseClients++;
  handleOrientationLoop(fd);
  sseClients--;
}

static void handleOrientationLoop(int fd) {
  static const char hdr[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
  if (!sendAll(fd, hdr, sizeof(hdr) - 1)) return;
  uint32_t lastSeq = 0, lastSend = 0;
  while (!updating) {
    uint32_t seq = telemetry.accSeq;
    // neue Werte max. 25x/s (das Otoskop liefert ~17); sonst alle 5 s ein Lebenszeichen, damit tote Clients auffallen
    if ((seq != lastSeq && millis() - lastSend >= 40) || millis() - lastSend >= 5000) {
      char msg[64];
      int n = seq != lastSeq
                  ? snprintf(msg, sizeof(msg), "data: {\"x\":%d,\"y\":%d,\"z\":%d}\n\n",
                             (int)telemetry.accX, (int)telemetry.accY, (int)telemetry.accZ)
                  : snprintf(msg, sizeof(msg), ": ping\n\n");
      if (!sendAll(fd, msg, n)) break;
      lastSeq = seq;
      lastSend = millis();
    }
    if (clientClosed(fd)) break;  // Browser hat die Seite verlassen -> Platz sofort frei
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Ethernet nur 10 Mbit (1) oder 100 Mbit (0): POST /eth10/<0|1>, im NVS gespeichert
static void handleEth10Post(int fd, const char *path) {
  if ((path[7] != '0' && path[7] != '1') || path[8]) return sendText(fd, 400, "Bad Request", "/eth10/<0|1>");
  bool on = path[7] == '1';
  Preferences p;
  if (p.begin("otoskop", false)) {
    p.putBool("eth10", on);
    p.end();
  }
  eth10 = on;
  crumb("eth10 -> %d", on);
  sendText(fd, 200, "OK", "gesetzt, Ethernet handelt neu aus (Link kurz weg)");
  delay(200);
  ethApplySpeed();
}

// --- Kameraauswahl (/cameras) ------------------------------------------------------
// Wert aus einem Formular-Body (application/x-www-form-urlencoded), dekodiert
static bool formValue(const char *body, const char *name, char *out, size_t len) {
  size_t nlen = strlen(name);
  for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : nullptr) {
    if (strncmp(p, name, nlen) || p[nlen] != '=') continue;
    p += nlen + 1;
    size_t o = 0;
    for (; *p && *p != '&'; p++) {
      char c = *p;
      if (c == '+') c = ' ';
      else if (c == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
        char hex[3] = {p[1], p[2], 0};
        c = (char)strtol(hex, nullptr, 16);
        p += 2;
      }
      if (o + 1 >= len) return false;  // zu lang
      out[o++] = c;
    }
    out[o] = 0;
    return true;
  }
  out[0] = 0;
  return false;
}

// Formular-Body (max. FORM_MAX Byte) vollständig lesen; false = Antwort schon gesendet
static const size_t FORM_MAX = 256;
static bool readForm(int fd, const char *req, const uint8_t *body, size_t bodyLen, char *buf) {
  size_t total = headerValue(req, "Content-Length").toInt();
  if (total == 0 || total > FORM_MAX) {
    sendText(fd, 413, "Payload Too Large", "Ungültige Länge");
    return false;
  }
  size_t have = min(bodyLen, total);
  memcpy(buf, body, have);
  while (have < total) {
    int n = recv(fd, buf + have, total - have, 0);
    if (n <= 0) {
      sendText(fd, 400, "Bad Request", "Verbindung abgebrochen");
      return false;
    }
    have += n;
  }
  buf[total] = 0;
  return true;
}

static void handleWifiSetup(int fd, const char *req, const uint8_t *body, size_t bodyLen) {
  char buf[FORM_MAX + 1], ssid[34], pass[66];
  if (!readForm(fd, req, body, bodyLen, buf)) return;
  formValue(buf, "ssid", ssid, sizeof(ssid));
  formValue(buf, "pass", pass, sizeof(pass));
  if (!rescueSetHome(ssid, pass))
    return sendText(fd, 400, "Bad Request", "SSID max. 32 Zeichen, Passwort leer oder 8-64 Zeichen");
  sendText(fd, 200, "OK", !*ssid ? "Heim-WLAN gelöscht" : rescueMode ? "Gespeichert, verbinde…" : "Gespeichert (wird im Notfall-Modus benutzt)");
}

static void handleCameraSelect(int fd, const char *req, const uint8_t *body, size_t bodyLen) {
  char buf[FORM_MAX + 1];
  if (!readForm(fd, req, body, bodyLen, buf)) return;
  char ssid[34], pass[66], proto[12];
  formValue(buf, "ssid", ssid, sizeof(ssid));
  formValue(buf, "pass", pass, sizeof(pass));
  formValue(buf, "proto", proto, sizeof(proto));
  if (!cameraSelect(ssid, pass, protoFromKey(proto)))
    return sendText(fd, 400, "Bad Request", "SSID (max. 32), Passwort (max. 64) oder Protokoll ungültig");
  sendText(fd, 202, "Accepted", *ssid ? "Verbinde…" : "Auswahl gelöscht, suche automatisch");
}

static void handleCamerasJson(int fd) {
  std::unique_ptr<char[]> json(new (std::nothrow) char[2048]);
  if (!json) return sendText(fd, 503, "Service Unavailable", "Kein Speicher");
  size_t n = cameraJson(json.get(), 2048);
  sendResponse(fd, 200, "OK", "application/json", json.get(), n);
}

static void clientTask(void *arg) {
  int fd = (int)(intptr_t)arg;
  timeval rcv = {5, 0}, snd = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  // Header bis zur Leerzeile lesen; was danach schon im Puffer steht, ist Body
  static const size_t REQ_MAX = 1536;
  std::unique_ptr<char[]> req(new (std::nothrow) char[REQ_MAX + 1]);
  if (!req) {  // Speicher knapp -> Verbindung ablehnen statt abstürzen
    close(fd);
    clientTasks--;
    vTaskDelete(nullptr);
  }
  size_t len = 0;
  char *headerEnd = nullptr;
  while (len < REQ_MAX) {
    int n = recv(fd, req.get() + len, REQ_MAX - len, 0);
    if (n <= 0) break;
    len += n;
    req[len] = 0;
    if ((headerEnd = strstr(req.get(), "\r\n\r\n"))) break;
  }

  if (!headerEnd) {
    if (len > 0) sendText(fd, 400, "Bad Request", "Ungültige Anfrage");
  } else {
    const uint8_t *body = (const uint8_t *)headerEnd + 4;
    size_t bodyLen = len - (body - (const uint8_t *)req.get());
    headerEnd[2] = 0;  // Header-Block abschließen, damit headerValue() nicht in den Body sucht

    bool get = strncmp(req.get(), "GET ", 4) == 0;
    bool post = strncmp(req.get(), "POST ", 5) == 0;
    char path[32] = "";
    const char *p = req.get() + (get ? 4 : post ? 5 : 0);
    size_t plen = strcspn(p, " ?\r\n");
    if (plen < sizeof(path)) memcpy(path, p, plen), path[plen] = 0;

    if (get && strcmp(path, "/") == 0) {
      sendResponse(fd, 200, "OK", "text/html; charset=utf-8", INDEX_HTML, sizeof(INDEX_HTML) - 1);
    } else if (get && strcmp(path, "/update") == 0) {
      sendResponse(fd, 200, "OK", "text/html; charset=utf-8", UPDATE_HTML,
                   sizeof(UPDATE_HTML) - 1);
    } else if (get && strcmp(path, "/stream") == 0) {
      handleStream(fd);
    } else if (get && strcmp(path, "/snapshot") == 0) {
      Frame frame;
      getFrame(frame);
      if (frame) {
        char hdr[160];
        int h = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n"
                         "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
                         (unsigned)frame.size());
        BlockSender out(fd);
        out.put(hdr, h);
        putFrame(out, frame);
        out.flush();
      } else sendText(fd, 503, "Service Unavailable", "Noch kein Bild empfangen");
    } else if (get && strcmp(path, "/app.js") == 0) {
      sendResponse(fd, 200, "OK", "application/javascript; charset=utf-8", APP_JS,
                   sizeof(APP_JS) - 1);
    } else if (get && strcmp(path, "/cameras") == 0) {
      sendResponse(fd, 200, "OK", "text/html; charset=utf-8", CAMERAS_HTML, sizeof(CAMERAS_HTML) - 1);
    } else if (get && strcmp(path, "/cameras.json") == 0) {
      handleCamerasJson(fd);
    } else if (post && strcmp(path, "/cameras/scan") == 0) {
      cameraRequestScan();
      sendText(fd, 202, "Accepted", "Suche gestartet");
    } else if (post && strcmp(path, "/cameras/select") == 0) {
      if (!authorized(req.get())) sendText(fd, 401, "Unauthorized", "Falsches OTA-Passwort");
      else handleCameraSelect(fd, req.get(), body, bodyLen);
    } else if (get && strcmp(path, "/calibrate") == 0) {
      sendResponse(fd, 200, "OK", "text/html; charset=utf-8", CALIBRATE_HTML,
                   sizeof(CALIBRATE_HTML) - 1);
    } else if (get && strcmp(path, "/calibration") == 0) {
      String c;
      {
        std::lock_guard<std::mutex> lock(calibMutex);
        c = calibJson;
      }
      sendResponse(fd, 200, "OK", "application/json", c.c_str(), c.length());
    } else if (post && strncmp(path, "/led/", 5) == 0 &&
               (path[5] == '0' || path[5] == '1') && path[6] == '\0') {
      if (!telemetry.ledSupported) {
        sendText(fd, 501, "Not Implemented", "Diese Kamera hat keine schaltbare LED");
      } else {
        ledRequest = path[5] == '1';  // die Kamera-Sitzung sendet und wartet auf Bestätigung
        sendText(fd, 202, "Accepted", "gesendet");
      }
    } else if (post && strncmp(path, "/eth10/", 7) == 0) {
      if (!authorized(req.get())) sendText(fd, 401, "Unauthorized", "Falsches OTA-Passwort");
      else handleEth10Post(fd, path);
    } else if (get && strcmp(path, "/wifi-setup") == 0) {
      sendResponse(fd, 200, "OK", "text/html; charset=utf-8", WIFI_SETUP_HTML, sizeof(WIFI_SETUP_HTML) - 1);
    } else if (post && strcmp(path, "/wifi-setup") == 0) {
      if (!authorized(req.get())) sendText(fd, 401, "Unauthorized", "Falsches OTA-Passwort");
      else handleWifiSetup(fd, req.get(), body, bodyLen);
    } else if (post && strncmp(path, "/wifi/", 6) == 0 && authorized(req.get())) {
      handleWifiModePost(fd, path);
    } else if (post && strcmp(path, "/calibration") == 0) {
      handleCalibrationPost(fd, req.get(), body, bodyLen);
    } else if (get && strcmp(path, "/orientation") == 0) {
      handleOrientation(fd);
    } else if (get && strcmp(path, "/status") == 0) {
      handleStatus(fd);
    } else if (post && strcmp(path, "/update") == 0) {
      handleUpdate(fd, req.get(), body, bodyLen);
    } else if (post && strcmp(path, "/restart") == 0) {
      if (!authorized(req.get())) {
        sendText(fd, 401, "Unauthorized", "Falsches OTA-Passwort");
      } else {
        sendText(fd, 200, "OK", "Gerät startet neu");
        delay(500);
        ESP.restart();
      }
    } else if (!get && !post) {
      sendText(fd, 405, "Method Not Allowed", "Nur GET und POST");
    } else if (get && rescueApActive()) {
      // Captive Portal: Handys prüfen beim Verbinden eine Adresse im Internet und
      // öffnen bei einer Umleitung von selbst die Einrichtungsseite
      static const char redirect[] =
          "HTTP/1.1 302 Found\r\nLocation: http://192.168.4.1/wifi-setup\r\n"
          "Content-Length: 0\r\nConnection: close\r\n\r\n";
      sendAll(fd, redirect, sizeof(redirect) - 1);
    } else {
      sendText(fd, 404, "Not Found", "Not found");
    }
  }

  req.reset();
  shutdown(fd, SHUT_RDWR);
  close(fd);
  clientTasks--;
  vTaskDelete(nullptr);
}

static void httpTask(void *) {
  int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);  // Ethernet und (im Notfall) Heim-WLAN
  addr.sin_port = htons(HTTP_PORT);
  bind(srv, (sockaddr *)&addr, sizeof(addr));
  listen(srv, 4);

  for (;;) {
    int fd = accept(srv, nullptr, nullptr);
    if (fd < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    // Streams, Lage-Events und ein paar Plätze für Seiten, Status und Update
    bool started = false;
    if (clientTasks < 2 * MAX_STREAM_CLIENTS + 3) {
      clientTasks++;  // vorher zählen: der Task kann schon fertig sein, bevor create zurückkehrt
      // Bei Funk-Einbrüchen hält der WLAN-Treiber kurz viel Heap fest -> kurz warten und
      // erneut versuchen, statt die Verbindung gleich abzulehnen
      for (int attempt = 0; attempt < 5 && !started; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(50));
        started = xTaskCreatePinnedToCore(clientTask, "http-client", 6144, (void *)(intptr_t)fd,
                                          3, nullptr, 1) == pdPASS;
      }
      if (!started) clientTasks--;
    }
    if (!started) {
      crumb("ausgelastet: %d Tasks, %d Streams, %d Lage, Heap %u/%u", (int)clientTasks,
            (int)streamClients, (int)sseClients, heapFree(), heapBlock());
      sendText(fd, 503, "Service Unavailable", "Ausgelastet");
      close(fd);
    }
  }
}

// --- Ethernet: Senden erst mit vollständigem Paket im FIFO ------------------------
// ESP-IDF startet das Senden, sobald 64 Byte im Sende-FIFO liegen. Blockiert währenddessen
// die WLAN-DMA den Speicherbus, läuft der FIFO leer und das Paket geht verstümmelt raus;
// der Switch verwirft es, TCP wiederholt erst nach ~1 s (gemessen: nur bei WLAN-Empfang).
// "Store and Forward" sendet erst, wenn das ganze Paket im FIFO liegt (2 KB = 1 Paket).
static void ethStoreForward() {
  if (!ETH_TX_STORE_FORWARD || EMAC_DMA.dmaoperation_mode.tx_str_fwd) return;
  EMAC_DMA.dmaoperation_mode.start_stop_transmission_command = 0;  // Senden anhalten
  delay(2);
  EMAC_DMA.dmaoperation_mode.tx_str_fwd = 1;
  EMAC_DMA.dmaoperation_mode.start_stop_transmission_command = 1;
  crumb("eth: Store and Forward an");
}

// --- Ethernet auf 10 Mbit -----------------------------------------------------------
// Der ESP32 erzeugt den 50-MHz-Takt für den LAN8720 selbst (GPIO17). WLAN-Empfang stört
// ihn; bei 100 Mbit gehen dann ~2-3 % der Ethernet-Pakete verloren (gemessen, unabhängig
// von WLAN-Sendeleistung, Puffern und Zigbee). Bei 10 Mbit wird jedes Bit 10 Takte lang
// gehalten und ist unempfindlich. Umgesetzt über die Aushandlung (nur "10 Mbit Voll-
// duplex" anbieten), damit der Switch nicht auf Halbduplex zurückfällt.

static bool phyRead(uint32_t reg, uint32_t &val) {
  esp_eth_phy_reg_rw_data_t rw = {reg, &val};
  return esp_eth_ioctl(ETH.handle(), ETH_CMD_READ_PHY_REG, &rw) == ESP_OK;
}
static bool phyWrite(uint32_t reg, uint32_t val) {
  esp_eth_phy_reg_rw_data_t rw = {reg, &val};
  return esp_eth_ioctl(ETH.handle(), ETH_CMD_WRITE_PHY_REG, &rw) == ESP_OK;
}

static void ethApplySpeed() {
  const uint32_t ANAR = 4, BMCR = 0;
  uint32_t anar = 0, bmcr = 0;
  if (!phyRead(ANAR, anar) || !phyRead(BMCR, bmcr)) return;
  // Bits 5-8: 10HD, 10FD, 100HD, 100FD; Pause-Bits (10/11) und Selektor bleiben
  uint32_t want = (anar & ~0x01E0u) | (eth10 ? 0x0040u : 0x01E0u);
  if (want == anar) return;  // schon so ausgehandelt -> keine Endlosschleife
  phyWrite(ANAR, want);
  phyWrite(BMCR, bmcr | 0x1000 | 0x0200);  // Aushandlung an + neu starten
  crumb("eth: biete %s an, handle neu aus", eth10 ? "nur 10 Mbit" : "100 Mbit");
}

// --- Netzwerk -------------------------------------------------------------------
static void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      ETH.setHostname(HOSTNAME);
      ethStarted = true;
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      ethStoreForward();
      ethApplySpeed();
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.printf("[eth] IP %s\n", ETH.localIP().toString().c_str());
      crumb("eth IP %s, %d Mbit %s", ETH.localIP().toString().c_str(), (int)ETH.linkSpeed(),
            ETH.fullDuplex() ? "Vollduplex" : "HALBDUPLEX");
      ethUp = true;
      ETH.setDefault();  // Standardroute ins Heimnetz, nicht zum Otoskop
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("[eth] getrennt");
      crumb("eth getrennt (event %d)", (int)event);
      ethUp = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      cameraOnWifiGotIp();
      crumb("wifi verbunden %s, RSSI %d", WiFi.SSID().c_str(), WiFi.RSSI());
      Serial.printf("[wifi] verbunden mit %s, IP %s, RSSI %d dBm\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (ethUp) ETH.setDefault();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[wifi] getrennt (Grund %u)\n", info.wifi_sta_disconnected.reason);
      crumb("wifi getrennt, Grund %u", info.wifi_sta_disconnected.reason);
      break;
    default:
      break;
  }
}

static void setLed(int pin, bool on) {
  if (pin < 0) return;
  digitalWrite(pin, LED_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH));
}

static void startOta() {
  ArduinoOTA.setHostname(HOSTNAME);  // startet auch mDNS -> otoskop.local
  if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    updating = true;
    Serial.println("[ota] Update startet");
  });
  ArduinoOTA.onError([](ota_error_t) { updating = false; });
  ArduinoOTA.begin();
  IPAddress ip = ethUp ? ETH.localIP() : WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP();
  Serial.printf("[http] Viewer: http://%s.local/  bzw. http://%s/\n", HOSTNAME,
                ip.toString().c_str());
}

void setup() {
  if (LED_GREEN_GPIO >= 0) pinMode(LED_GREEN_GPIO, OUTPUT);
  if (LED_RED_GPIO >= 0) pinMode(LED_RED_GPIO, OUTPUT);
  setZigbee(false);  // Zigbee wird nicht gebraucht: stumm schalten
  setLed(LED_GREEN_GPIO, true);
  setLed(LED_RED_GPIO, false);

  Serial.begin(115200);
  if (bootMagic != 0xB007B007 || esp_reset_reason() == ESP_RST_POWERON) {
    bootMagic = 0xB007B007;
    bootCount = 0;
  }
  bootCount++;
  crashlogInit();
  Serial.printf("\n[boot] WiFi-Cam-Proxy %s, Reset: %s, Boot #%u\n", FW_VERSION,
                resetReasonText(), (unsigned)bootCount);

  loadCalibration();
  loadWifiMode();
  rescueBegin();
  Network.onEvent(onNetworkEvent);
  ethBeginOk = ETH.begin(ETH_PHY_LAN8720, ETH_PHY_ADDR_GW, ETH_MDC_GPIO, ETH_MDIO_GPIO, ETH_POWER_GPIO,
            ETH_CLK_MODE_GW);

  Serial.printf("[eth] begin %s\n", ethBeginOk ? "ok" : "FEHLGESCHLAGEN");

  cameraBegin();  // WLAN zur Kamera und Video-Task
  xTaskCreatePinnedToCore(httpTask, "http", 4096, nullptr, 3, nullptr, 1);
}

void loop() {
  static bool otaStarted = false;
  static uint32_t lastStats = 0, lastFrames = 0;
  static uint32_t ethDownSince = 0, ethUpSince = 0;

  // Notfall-Modus: rein, wenn Ethernet zu lange weg ist; raus per Neustart, wenn es
  // wieder stabil da ist
  if (ethUp) {
    ethDownSince = millis();
    if (rescueMode && !updating && millis() - ethUpSince > 10000) {
      Serial.println("[rescue] Ethernet wieder da -> Neustart in den Normalbetrieb");
      delay(200);
      ESP.restart();
    }
  } else {
    ethUpSince = millis();
    if (!rescueMode && millis() - ethDownSince > RESCUE_TIMEOUT_MS) {
      setLed(LED_RED_GPIO, true);
      rescueEnter();
    }
  }

  rescueLoop();
  if (!otaStarted && (ethUp || (rescueMode && (WiFi.status() == WL_CONNECTED || rescueApActive())))) {
    startOta();
    otaStarted = true;
  }
  if (otaStarted) ArduinoOTA.handle();
  cameraLoop();

  if (millis() - lastStats >= 5000) {
    uint32_t total = stats.framesTotal;
    currentFps = (total - lastFrames) * 1000.0f / (millis() - lastStats);
    lastFrames = total;
    lastStats = millis();
    Serial.printf("[stats] %.1f fps, %u Bilder, %u verworfen, %d Zuschauer, Heap %u%s\n",
                  currentFps, (unsigned)total, (unsigned)stats.framesDropped, (int)streamClients,
                  heapFree(), rescueMode ? ", NOTFALL-MODUS" : "");
  }
  delay(10);
}
