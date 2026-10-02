/*
 * Otoskop-Bridge für ZB-GW03 v1.4 (ESP32 + LAN8720)
 *
 * - WLAN (Client): verbindet sich mit dem offenen AP des Soulear-Otoskops
 * - Ethernet (DHCP): stellt im Heimnetz einen MJPEG-Server bereit
 *     http://otoskop.local/          Browser
 *     http://otoskop.local/stream    VLC / Home Assistant
 *     http://otoskop.local/snapshot  Einzelbild
 *     http://otoskop.local/status    JSON mit Statistik
 *     http://otoskop.local/update    Firmware-Update im Browser
 *
 * Notfall-Modus: Hat Ethernet RESCUE_TIMEOUT_MS lang keine IP, geht das WLAN ins
 * Heim-WLAN (secrets.h), damit Weboberfläche und OTA erreichbar bleiben.
 *
 * Protokoll wie soulear-viewer.py: GetDeviceInfo an UDP 10005, danach START an
 * UDP 10006 (beides vom selben Socket), dann kommen JPEG-Stücke mit 16-Byte-Kopf.
 */

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ETH.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <soc/emac_dma_struct.h>
#include <lwip/sockets.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <new>

#include "config.h"
#include "crashlog.h"

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
static const uint8_t MAGIC[4] = {0xEE, 0xFF, 0xEE, 0xFF};

// --- Speicher -------------------------------------------------------------------
// ESP.getFreeHeap()/getMaxAllocHeap() zählen den IRAM-Rest mit (MALLOC_CAP_INTERNAL).
// Der ist nur wortweise nutzbar, also weder für malloc() noch für Task-Stacks. Für
// Entscheidungen und Anzeige zählt nur der byteweise nutzbare Speicher (8BIT).
static const uint32_t HEAP_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
static unsigned heapFree() { return heap_caps_get_free_size(HEAP_CAPS); }
static unsigned heapMin() { return heap_caps_get_minimum_free_size(HEAP_CAPS); }
static unsigned heapBlock() { return heap_caps_get_largest_free_block(HEAP_CAPS); }
static unsigned iramFree() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - heap_caps_get_free_size(HEAP_CAPS);
}

// --- Status ---------------------------------------------------------------------
static volatile bool ethUp = false;
static volatile bool ethStarted = false;  // LAN8720 initialisiert
static bool ethBeginOk = false;
static volatile bool rescueMode = false;
static std::atomic<bool> updating{false};
static std::atomic<uint32_t> framesTotal{0};
static std::atomic<uint32_t> framesDropped{0};    // Summe der drei folgenden
static std::atomic<uint32_t> dropNoMem{0};        // kein Heap für das fertige Bild
static std::atomic<uint32_t> dropTooBig{0};       // größer als MAX_FRAME_BYTES
static std::atomic<uint32_t> dropIncomplete{0};   // Paket(e) im Bild verloren
static std::atomic<uint32_t> packetsLost{0};      // Lücken in der Sequenznummer
static std::atomic<uint32_t> framesDamaged{0};    // trotz Paketverlust angezeigt
static std::atomic<uint32_t> maxFrameBytes{0};
static std::atomic<uint32_t> handshakes{0};
static std::atomic<uint32_t> keepalives{0};
// Stillstände des Videos: mit Paketverlust in den 2 s davor (Funk) oder ohne (Otoskop
// pausiert von sich aus). Von den sauberen die letzten Zeitpunkte (s seit Start).
static std::atomic<uint32_t> stallsLoss{0};
static std::atomic<uint32_t> stallsClean{0};
static const int CLEAN_STALL_TIMES = 8;
static uint32_t cleanStallAt[CLEAN_STALL_TIMES];
static std::atomic<int> streamClients{0};
static std::atomic<int> sseClients{0};
// LED-Steuerung am Otoskop (Befehlstyp 0x0A, SetLed, laut i4season-Protokoll)
// Ungetestet am Gerät; -1 = kein ausstehender Befehl, 0 = aus, 1 = an
static std::atomic<int> ledPending{-1};
static std::atomic<bool> ledState{false};
// Signalstärke, 2x/s in loop() gelesen: WiFi.RSSI() wartet auf den WLAN-Treiber und
// darf den Video-Task nicht ausbremsen (UDP-Empfangspuffer fasst nur 6 Pakete)
static std::atomic<int8_t> wifiRssi{0};
// WLAN-Modus zum Otoskop (Index in WIFI_MODES), siehe applyWifiMode()
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

// Test-Schalter zum Eingrenzen der Ethernet-Verluste bei laufendem Video (POST /debug/...)
static std::atomic<bool> dbgVideo{true};    // WLAN-Pakete verarbeiten (sonst sofort verwerfen)
static std::atomic<bool> dbgWifi{true};     // WLAN-Verbindung zum Otoskop halten
static std::atomic<bool> zigbeeOn{false};   // Zigbee-Chip laufen lassen (sonst im Reset)
static std::atomic<bool> eth10{ETH_10MBIT_DEFAULT};  // Ethernet nur 10 Mbit, siehe ethApplySpeed()
static void ethApplySpeed();

static void setZigbee(bool on) {
  if (ZIGBEE_NRST_GPIO < 0) return;
  zigbeeOn = on;
  pinMode(ZIGBEE_NRST_GPIO, OUTPUT);
  digitalWrite(ZIGBEE_NRST_GPIO, on ? HIGH : LOW);
}

// Sendet einen SetLed-Befehl (Typ 0x0A) an das Otoskop über einen temporären UDP-Socket.
// Laut i4season-Protokolldoku (Fyfar/ms5-wifi-microscope) mit 1 Byte Payload (0=aus, 1=an).
// Am Soulear-Gerät bisher ungetestet.
static void sendLedCommand(bool on) {
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return;
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(OTOSCOPE_IP);
  addr.sin_port = htons(VIDEO_CTRL_PORT);
  // Header: magic, id=5, type=0x0A (SetLed), unk=1, err=0, length=1; Payload: 0/1
  uint8_t cmd[13] = {0xEE, 0xFF, 0xEE, 0xFF, 0x05, 0x00, 0x0A, 0x00,
                     0x01, 0x00, 0x01, 0x00, on ? 0x01u : 0x00u};
  sendto(s, cmd, sizeof(cmd), 0, (sockaddr *)&addr, sizeof(addr));
  close(s);
  ledState = on;
  crumb("SetLed -> %s", on ? "an" : "aus");
}
static std::atomic<int> clientTasks{0};
static float currentFps = 0;

// --- Gemeinsamer Bildspeicher ---------------------------------------------------
// Ein Bild wird als Liste seiner UDP-Nutzdaten (je ~1,3 KB) gespeichert statt am
// Stück: keine Kopie, kein großer Puffer und nie ein großer zusammenhängender
// Heap-Block, der beim Streamen schnell zerstückelt ist. Der Empfänger ersetzt das
// Bild, die HTTP-Clients halten sich eine Referenz und senden ohne Sperre.
// Bewusst malloc statt new: bei Speichermangel wird das Bild verworfen, statt per
// bad_alloc das Gerät abstürzen zu lassen.
static const int MAX_CHUNKS = MAX_FRAME_BYTES / 1024 + 1;

// Bildstücke liegen bevorzugt im IRAM-Rest (~44 KB), der sonst ungenutzt bleibt und
// den normalen Heap entlastet. IRAM verträgt nur 32-Bit-Zugriffe: deshalb wortweise
// kopieren (volatile, damit der Compiler daraus kein byteweises memcpy macht).
static uint8_t *allocChunk(size_t len) {
  size_t bytes = (len + 3) & ~(size_t)3;
  void *p = USE_IRAM_CHUNKS ? heap_caps_malloc(bytes, MALLOC_CAP_EXEC) : nullptr;  // IRAM
  // EXEC liefert u.U. auch RTC-FAST-Speicher: der ist ebenfalls nur wortweise und von
  // Core 1 gar nicht nutzbar -> nur echtes IRAM behalten, sonst normaler Heap
  if (p && !esp_ptr_in_iram(p)) {
    free(p);
    p = nullptr;
  }
  if (!p) p = malloc(bytes);  // IRAM voll -> normaler Heap
  return (uint8_t *)p;
}

__attribute__((noinline)) static void copyToChunk(uint8_t *dst, const uint8_t *src, size_t len) {
  if (!esp_ptr_in_iram(dst)) {
    memcpy(dst, src, len);
    return;
  }
  volatile uint32_t *d = (volatile uint32_t *)dst;
  size_t i = 0;
  for (; i + 4 <= len; i += 4) {
    uint32_t w;
    memcpy(&w, src + i, 4);
    *d++ = w;
  }
  if (i < len) {
    uint32_t w = 0;
    memcpy(&w, src + i, len - i);
    *d = w;
  }
}

struct FrameBuf {
  std::atomic<int> refs;
  size_t len;
  int n;
  uint8_t *chunk[MAX_CHUNKS];
  uint16_t clen[MAX_CHUNKS];
};

class Frame {
 public:
  Frame() = default;
  Frame(const Frame &o) : p_(o.p_) { if (p_) p_->refs++; }
  Frame &operator=(const Frame &o) {
    if (o.p_) o.p_->refs++;
    reset();
    p_ = o.p_;
    return *this;
  }
  ~Frame() { reset(); }

  static Frame create() {
    Frame f;
    f.p_ = (FrameBuf *)malloc(sizeof(FrameBuf));
    if (f.p_) {
      new (&f.p_->refs) std::atomic<int>(1);
      f.p_->len = 0;
      f.p_->n = 0;
    }
    return f;
  }
  // Nur vom Empfänger, solange das Bild noch nicht veröffentlicht ist
  bool append(const uint8_t *data, size_t len) {
    if (p_->n >= MAX_CHUNKS) return false;
    uint8_t *c = allocChunk(len);
    if (!c) return false;
    copyToChunk(c, data, len);
    p_->chunk[p_->n] = c;
    p_->clen[p_->n++] = len;
    p_->len += len;
    return true;
  }
  void trimLast(size_t bytes) {  // Füll-Nullen nach FF D9 abschneiden
    p_->clen[p_->n - 1] -= bytes;
    p_->len -= bytes;
  }
  void reset() {
    if (p_ && --p_->refs == 0) {
      for (int i = 0; i < p_->n; i++) free(p_->chunk[i]);
      free(p_);
    }
    p_ = nullptr;
  }
  explicit operator bool() const { return p_ != nullptr; }
  size_t size() const { return p_->len; }
  int chunks() const { return p_->n; }
  const uint8_t *chunk(int i) const { return p_->chunk[i]; }
  size_t chunkLen(int i) const { return p_->clen[i]; }

 private:
  FrameBuf *p_ = nullptr;
};

static std::mutex frameMutex;
static Frame latestFrame;
static uint32_t frameSeq = 0;

static void publishFrame(const Frame &frame) {
  if (frame.size() > maxFrameBytes) maxFrameBytes = frame.size();
  std::lock_guard<std::mutex> lock(frameMutex);
  latestFrame = frame;  // altes Bild wird frei, sobald kein Client es mehr sendet
  frameSeq++;
  framesTotal++;
}

static uint32_t getFrame(Frame &out) {
  std::lock_guard<std::mutex> lock(frameMutex);
  out = latestFrame;
  return frameSeq;
}

// --- Mitschnitt pro Bild (für /sensor) -----------------------------------------
// Paketkopf des ersten Pakets, Bildgröße, Signalstärke und Paketverlust der letzten
// ~25 s. Dient zum Entschlüsseln des Lagesensors (vermutlich Kopf-Bytes 6-11).
struct SensorSample {
  uint32_t ms;
  uint16_t size;
  int8_t rssi;
  uint8_t lost;
  uint8_t hdr[CHUNK_HEADER_LEN];
};
static const int SENSOR_SAMPLES = 128;  // ~7 s; war nur zum Entschlüsseln nötig
static SensorSample *sensorRing = nullptr;
static uint32_t sensorNext = 0;
static portMUX_TYPE sensorMux = portMUX_INITIALIZER_UNLOCKED;

static void recordSample(const uint8_t *hdr, size_t size, uint8_t lost) {
  if (!sensorRing) return;
  SensorSample smp;
  smp.ms = millis();
  smp.size = size > 0xFFFF ? 0xFFFF : size;
  smp.rssi = wifiRssi;
  smp.lost = lost;
  memcpy(smp.hdr, hdr, CHUNK_HEADER_LEN);
  portENTER_CRITICAL(&sensorMux);
  sensorRing[sensorNext++ % SENSOR_SAMPLES] = smp;
  portEXIT_CRITICAL(&sensorMux);
}

// --- Lagesensor -----------------------------------------------------------------
// Kopf-Bytes 6-9 (little-endian) enthalten den Beschleunigungssensor: drei 10-Bit-
// Werte (Bits 0-9 x, 10-19 y, 20-29 z), je Bit 9 = Vorzeichen, Bits 0-8 = Betrag.
// ~128 entsprechen 1 g. Drehen um die Stiftachse bewegt x/y, z bleibt fast gleich.
static std::atomic<int16_t> accX{0}, accY{0}, accZ{0};
static std::atomic<uint32_t> accSeq{0};

static int16_t signMag10(uint32_t v) {
  return (v & 0x200) ? -(int16_t)(v & 0x1FF) : (int16_t)(v & 0x1FF);
}

static void decodeOrientation(const uint8_t *hdr) {
  uint32_t v = hdr[6] | (hdr[7] << 8) | (hdr[8] << 16) | ((uint32_t)hdr[9] << 24);
  accX = signMag10(v);
  accY = signMag10(v >> 10);
  accZ = signMag10(v >> 20);
  accSeq++;
}

// --- Video-Task: UDP-Pakete -> ganze JPEGs --------------------------------------
static void videoTask(void *) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = 0;  // freien Port vom Stack holen
  bind(sock, (sockaddr *)&local, sizeof(local));
  socklen_t slen = sizeof(local);
  getsockname(sock, (sockaddr *)&local, &slen);
  uint16_t myPort = ntohs(local.sin_port);

  timeval tv = {0, 200 * 1000};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // GetDeviceInfo: magic, id 0, type 0x01, unk 1, err 0, length 0
  uint8_t discovery[12] = {0xEE, 0xFF, 0xEE, 0xFF, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00};
  // START: magic, id 2, type 0x04, unk 1, err 0, length 2, Port (LE), 00 00
  uint8_t start[16] = {0xEE, 0xFF, 0xEE, 0xFF, 0x02, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
                       (uint8_t)(myPort & 0xFF), (uint8_t)(myPort >> 8), 0x00, 0x00};

  sockaddr_in discAddr = {}, ctrlAddr = {};
  discAddr.sin_family = ctrlAddr.sin_family = AF_INET;
  discAddr.sin_addr.s_addr = ctrlAddr.sin_addr.s_addr = inet_addr(OTOSCOPE_IP);
  discAddr.sin_port = htons(DISCOVERY_PORT);
  ctrlAddr.sin_port = htons(VIDEO_CTRL_PORT);

  Serial.printf("[video] Empfange auf UDP-Port %u\n", myPort);

  static uint8_t pkt[2048] __attribute__((aligned(4)));  // Nutzdaten ab +16 bleiben ausgerichtet
  sensorRing = (SensorSample *)calloc(SENSOR_SAMPLES, sizeof(SensorSample));
  uint8_t frameHdr[CHUNK_HEADER_LEN] = {};  // Kopf des ersten Pakets im Bild
  uint8_t frameLost = 0;
  Frame building;            // hier wächst das aktuelle JPEG
  bool inFrame = false;
  bool frameBroken = false;  // im aktuellen Bild fehlt ein Paket
  bool haveSeq = false;
  uint8_t nextSeq = 0;
  uint8_t sensorFrame = 0;   // Bildnummer des zuletzt gelesenen Lagewerts
  bool haveSensorFrame = false;
  uint32_t lastData = 0;
  uint32_t lastStart = 0;  // letzter START (Handshake oder Lebenszeichen)
  uint32_t lastLoss = 0;   // letzte Lücke in der Paketnummer

  for (;;) {
    int n = recvfrom(sock, pkt, sizeof(pkt), 0, nullptr, nullptr);

    // Stille -> Handshake wiederholen (erstes Paket nach Leerlauf geht oft verloren).
    // Im Notfall-Modus hängt das WLAN im Heimnetz, dort hat das Otoskop nichts verloren.
    // Lebenszeichen: START vorsorglich wiederholen, solange Video läuft. Vermutung: das
    // Otoskop pausiert sonst etwa alle 20 s kurz (Stillstände ohne Paketverlust)
    if (KEEPALIVE_INTERVAL_MS > 0 && haveSeq && !rescueMode && !updating &&
        millis() - lastStart >= KEEPALIVE_INTERVAL_MS) {
      sendto(sock, start, sizeof(start), 0, (sockaddr *)&ctrlAddr, sizeof(ctrlAddr));
      lastStart = millis();
      keepalives++;
    }

    // Stillstand erkennen, aber nach einem START dem Otoskop Zeit zum Anlaufen lassen:
    // ein neuer START vor dem Anlaufen startet es erneut (Kaskade bei 200 ms beobachtet)
    if (millis() - lastData > STALL_TIMEOUT_MS && millis() - lastStart >= HANDSHAKE_RETRY_MS) {
      if (haveSeq) {  // Video lief bis eben
        bool loss = lastLoss && millis() - lastLoss < STALL_TIMEOUT_MS + 2000;
        if (loss) {
          stallsLoss++;
        } else {
          cleanStallAt[stallsClean % CLEAN_STALL_TIMES] = millis() / 1000;
          stallsClean++;
        }
        crumb("stall: %u ms keine Daten -> Handshake (%s)", STALL_TIMEOUT_MS,
              loss ? "nach Paketverlust" : "ohne Paketverlust");
      }
      haveSeq = false;
      if (!rescueMode && !updating && dbgVideo && WiFi.status() == WL_CONNECTED) {
        sendto(sock, discovery, sizeof(discovery), 0, (sockaddr *)&discAddr, sizeof(discAddr));
        sendto(sock, start, sizeof(start), 0, (sockaddr *)&ctrlAddr, sizeof(ctrlAddr));
        lastStart = millis();
        handshakes++;
      }
      lastData = millis();
      inFrame = false;
      building.reset();
    }

    if (n <= 0) continue;
    if (!dbgVideo) continue;  // Test: empfangen, aber nicht verarbeiten
    if (n >= 4 && memcmp(pkt, MAGIC, 4) == 0) continue;  // Antworten/ACKs
    if (n <= CHUNK_HEADER_LEN) continue;
    lastData = millis();

    const uint8_t *payload = pkt + CHUNK_HEADER_LEN;
    size_t plen = n - CHUNK_HEADER_LEN;

    // Lage steht in jedem Paket des Bildes; beim ersten ankommenden Paket eines neuen
    // Bildes (Byte 2) übernehmen, damit ein verlorenes Startpaket keinen Messwert kostet
    if (!haveSensorFrame || pkt[2] != sensorFrame) {
      decodeOrientation(pkt);
      sensorFrame = pkt[2];
      haveSensorFrame = true;
    }

    // Laufende Paketnummer (Byte 1, 8 Bit) -> verlorene Pakete erkennen
    uint8_t seq = pkt[1];
    if (haveSeq && seq != nextSeq) {
      uint8_t gap = seq - nextSeq;
      lastLoss = millis();
      packetsLost += gap;
      frameLost = frameLost + gap > 255 ? 255 : frameLost + gap;
      frameBroken = true;
    }
    haveSeq = true;
    nextSeq = seq + 1;

    if (plen >= 2 && payload[0] == 0xFF && payload[1] == 0xD8) {
      if (inFrame) {  // vorheriges Bild wurde nie fertig (Ende verloren)
        framesDropped++;
        dropIncomplete++;
      }
      building = Frame::create();  // neues Bild beginnt
      inFrame = (bool)building;
      frameBroken = false;
      frameLost = 0;
      memcpy(frameHdr, pkt, CHUNK_HEADER_LEN);
      if (!inFrame) {
        framesDropped++;
        dropNoMem++;
        continue;
      }
    } else if (!inFrame) {
      continue;  // mittendrin eingestiegen -> auf nächsten Bildanfang warten
    }

    if (building.size() + plen > MAX_FRAME_BYTES) {
      inFrame = false;
      building.reset();
      framesDropped++;
      dropTooBig++;
      continue;
    }
    if (!building.append(payload, plen)) {
      inFrame = false;
      building.reset();
      framesDropped++;
      dropNoMem++;
      continue;
    }

    // Endet hier das Bild? (FF D9, eventuell gefolgt von Füll-Nullen)
    size_t end = plen;
    while (end > 0 && payload[end - 1] == 0x00) end--;
    if (end >= 2 && payload[end - 2] == 0xFF && payload[end - 1] == 0xD9) {
      recordSample(frameHdr, building.size(), frameLost);
      if (frameBroken && !SHOW_DAMAGED_FRAMES) {  // kaputtes JPEG nicht weitergeben
        framesDropped++;
        dropIncomplete++;
      } else {
        if (frameBroken) framesDamaged++;
        building.trimLast(plen - end);
        publishFrame(building);
      }
      building.reset();
      inFrame = false;
    }
  }
}

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
  uint32_t nClean = stallsClean;
  uint32_t shown = min(nClean, (uint32_t)CLEAN_STALL_TIMES);
  for (uint32_t k = 0, t = 0; k < shown; k++)
    t += snprintf(times + t, sizeof(times) - t, "%s%lu", k ? "," : "",
                  (unsigned long)cleanStallAt[(nClean - shown + k) % CLEAN_STALL_TIMES]);
  char json[1280];
  int n = snprintf(json, sizeof(json),
                   "{\"version\":\"%s\",\"reset_reason\":\"%s\",\"boot_count\":%u,\"mode\":\"%s\",\"fps\":%.1f,\"frames\":%u,"
                   "\"dropped\":%u,\"drop_nomem\":%u,\"drop_toobig\":%u,\"drop_incomplete\":%u,\"packets_lost\":%u,\"damaged\":%u,\"max_frame\":%u,\"handshakes\":%u,\"keepalives\":%u,\"stalls_loss\":%u,\"stalls_clean\":%u,\"clean_stall_times\":[%s],\"stream_clients\":%d,\"sse_clients\":%d,\"client_tasks\":%d,"
                   "\"wifi_connected\":%s,\"wifi_ssid\":\"%s\",\"wifi_rssi\":%d,\"wifi_mode\":\"%s\",\"wifi_tx_dbm\":%.2f,\"dbg_video\":%d,\"dbg_wifi\":%d,\"zigbee\":%d,"
                   "\"led\":%s,"
                   "\"eth_begin\":%s,\"eth_started\":%s,\"eth_link\":%s,\"eth_speed\":%d,\"eth_full_duplex\":%s,\"eth_tx_store_forward\":%d,\"eth_ip\":\"%s\",\"free_heap\":%u,\"max_alloc\":%u,\"min_heap\":%u,\"iram_heap\":%u,\"psram\":%u,\"uptime_s\":%lu,\"last_crash\":\"%s\"}",
                   FW_VERSION, resetReasonText(), (unsigned)bootCount, rescueMode ? "rescue" : "normal", currentFps,
                   (unsigned)framesTotal, (unsigned)framesDropped, (unsigned)dropNoMem,
                   (unsigned)dropTooBig, (unsigned)dropIncomplete, (unsigned)packetsLost, (unsigned)framesDamaged,
                   (unsigned)maxFrameBytes, (unsigned)handshakes, (unsigned)keepalives, (unsigned)stallsLoss, (unsigned)stallsClean, times,
                   (int)streamClients, (int)sseClients, (int)clientTasks, wifiOk ? "true" : "false",
                   wifiOk ? WiFi.SSID().c_str() : "", wifiOk ? WiFi.RSSI() : 0, WIFI_MODES[wifiMode], wifiTxDbm(), (int)dbgVideo, (int)dbgWifi, (int)zigbeeOn,
                   ledState ? "true" : "false",
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

// --- WLAN-Modus zum Otoskop (umschaltbar über die Update-Seite, im NVS) ----------
// "bgn": 802.11n mit Paket-Bündelung (schnell, aber ein fehlendes Teilpaket hält den
//        ganzen Block auf); "bg": ohne 11n, jedes Paket einzeln (Standard);
// "b":   nur 802.11b, langsam (max. 11 Mbit/s), dafür am robustesten bei schwachem Signal
static void applyWifiMode() {
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
  WiFi.disconnect();
  applyWifiMode();
  WiFi.begin(rescueMode ? HOME_WIFI_SSID : OTOSCOPE_SSID, rescueMode ? HOME_WIFI_PASSWORD : nullptr);
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
    uint32_t seq = accSeq;
    // neue Werte max. 25x/s (das Otoskop liefert ~17); sonst alle 5 s ein Lebenszeichen, damit tote Clients auffallen
    if ((seq != lastSeq && millis() - lastSend >= 40) || millis() - lastSend >= 5000) {
      char msg[64];
      int n = seq != lastSeq
                  ? snprintf(msg, sizeof(msg), "data: {\"x\":%d,\"y\":%d,\"z\":%d}\n\n",
                             (int)accX, (int)accY, (int)accZ)
                  : snprintf(msg, sizeof(msg), ": ping\n\n");
      if (!sendAll(fd, msg, n)) break;
      lastSeq = seq;
      lastSend = millis();
    }
    if (clientClosed(fd)) break;  // Browser hat die Seite verlassen -> Platz sofort frei
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void handleSensor(int fd) {
  if (!sensorRing) return sendText(fd, 503, "Service Unavailable", "Kein Speicher");
  SensorSample *copy = (SensorSample *)malloc(SENSOR_SAMPLES * sizeof(SensorSample));
  if (!copy) return sendText(fd, 503, "Service Unavailable", "Kein Speicher");
  portENTER_CRITICAL(&sensorMux);
  memcpy(copy, sensorRing, SENSOR_SAMPLES * sizeof(SensorSample));
  uint32_t next = sensorNext;
  portEXIT_CRITICAL(&sensorMux);

  static const char hdr[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
      "Cache-Control: no-cache\r\nConnection: close\r\n\r\n"
      "# ms size rssi lost | Kopf (16 Byte hex)\n";
  bool ok = sendAll(fd, hdr, sizeof(hdr) - 1);
  uint32_t n = min(next, (uint32_t)SENSOR_SAMPLES);
  char line[96];
  for (uint32_t k = 0; ok && k < n; k++) {
    const SensorSample &smp = copy[(next - n + k) % SENSOR_SAMPLES];
    int h = snprintf(line, sizeof(line), "%lu %u %d %u |", (unsigned long)smp.ms, smp.size,
                     smp.rssi, smp.lost);
    for (int i = 0; i < CHUNK_HEADER_LEN; i++) h += snprintf(line + h, sizeof(line) - h, " %02x", smp.hdr[i]);
    line[h++] = '\n';
    ok = sendAll(fd, line, h);
  }
  free(copy);
}

// Test-Schalter: /debug/video/0|1, /debug/wifi/0|1, /debug/zigbee/0|1
static void handleDebugPost(int fd, const char *path) {
  const char *name = path + 7;
  const char *slash = strchr(name, '/');
  if (!slash || (slash[1] != '0' && slash[1] != '1'))
    return sendText(fd, 400, "Bad Request", "/debug/<video|wifi|zigbee|eth10>/<0|1>");
  bool on = slash[1] == '1';
  String n = String(name).substring(0, slash - name);
  if (n == "video") {
    dbgVideo = on;
  } else if (n == "wifi") {
    dbgWifi = on;
    WiFi.setAutoReconnect(on);
    if (on) WiFi.begin(OTOSCOPE_SSID);
    else WiFi.disconnect();
  } else if (n == "zigbee") {
    setZigbee(on);
  } else if (n == "eth10") {
    Preferences p;
    if (p.begin("otoskop", false)) {
      p.putBool("eth10", on);
      p.end();
    }
    eth10 = on;
    crumb("debug eth10 -> %d", on);
    sendText(fd, 200, "OK", "gesetzt, Ethernet handelt neu aus (Link kurz weg)");
    delay(200);
    ethApplySpeed();
    return;
  } else {
    return sendText(fd, 400, "Bad Request", "unbekannter Schalter");
  }
  crumb("debug %s -> %d", n.c_str(), on);
  sendText(fd, 200, "OK", "gesetzt");
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
      bool on = path[5] == '1';
      sendLedCommand(on);
      sendText(fd, 200, "OK", on ? "LED an" : "LED aus");
    } else if (post && strncmp(path, "/debug/", 7) == 0 && authorized(req.get())) {
      handleDebugPost(fd, path);
    } else if (post && strncmp(path, "/wifi/", 6) == 0 && authorized(req.get())) {
      handleWifiModePost(fd, path);
    } else if (post && strcmp(path, "/calibration") == 0) {
      handleCalibrationPost(fd, req.get(), body, bodyLen);
    } else if (get && strcmp(path, "/orientation") == 0) {
      handleOrientation(fd);
    } else if (get && strcmp(path, "/sensor") == 0) {
      handleSensor(fd);
    } else if (get && strcmp(path, "/log") == 0) {
      String log = crashlogText();
      sendResponse(fd, 200, "OK", "text/plain; charset=utf-8", log.c_str(), log.length());
    } else if (post && strcmp(path, "/crashtest") == 0 && authorized(req.get())) {
      sendText(fd, 200, "OK", "Absturz wird ausgelöst");
      crumb("crashtest");
      delay(300);
      *(volatile int *)nullptr = 0;
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

static void enterRescueMode() {
  rescueMode = true;
  setLed(LED_RED_GPIO, true);
  Serial.printf("[rescue] Ethernet ohne IP -> WLAN wechselt zu \"%s\"\n", HOME_WIFI_SSID);
  WiFi.disconnect();
  WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASSWORD);
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
  IPAddress ip = ethUp ? ETH.localIP() : WiFi.localIP();
  Serial.printf("[http] Viewer: http://%s.local/  bzw. http://%s/\n", HOSTNAME,
                ip.toString().c_str());
}

void setup() {
  pinMode(LED_GREEN_GPIO, OUTPUT);
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
  Serial.printf("\n[boot] Otoskop-Bridge %s, Reset: %s, Boot #%u\n", FW_VERSION,
                resetReasonText(), (unsigned)bootCount);

  loadCalibration();
  loadWifiMode();
  Network.onEvent(onNetworkEvent);
  ethBeginOk = ETH.begin(ETH_PHY_LAN8720, ETH_PHY_ADDR_GW, ETH_MDC_GPIO, ETH_MDIO_GPIO, ETH_POWER_GPIO,
            ETH_CLK_MODE_GW);

  Serial.printf("[eth] begin %s\n", ethBeginOk ? "ok" : "FEHLGESCHLAGEN");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // Modem-Sleep kostet bei UDP-Video Pakete
  applyWifiMode();
  WiFi.setAutoReconnect(true);
  WiFi.begin(OTOSCOPE_SSID);

  xTaskCreatePinnedToCore(videoTask, "video", 4096, nullptr, 10, nullptr, 1);  // vor HTTP (3)
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
    if (!rescueMode && strlen(HOME_WIFI_SSID) > 0 && millis() - ethDownSince > RESCUE_TIMEOUT_MS)
      enterRescueMode();
  }

  if (!otaStarted && (ethUp || (rescueMode && WiFi.status() == WL_CONNECTED))) {
    startOta();
    otaStarted = true;
  }
  if (otaStarted) ArduinoOTA.handle();

  static uint32_t lastRssi = 0;
  if (millis() - lastRssi >= 500) {
    wifiRssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    lastRssi = millis();
  }

  static uint32_t lastCrumb = 0, lastCrumbFrames = 0;
  if (millis() - lastCrumb >= 1000) {
    uint32_t total = framesTotal;
    static uint32_t lastNoMem = 0, lastInc = 0, lastLost = 0;
    uint32_t nm = dropNoMem, inc = dropIncomplete, lost = packetsLost;
    crumb("%lufps nm%lu inc%lu lost%lu heap%u min%u blk%u rssi%d cl%d",
          (unsigned long)(total - lastCrumbFrames), (unsigned long)(nm - lastNoMem),
          (unsigned long)(inc - lastInc), (unsigned long)(lost - lastLost),
          heapFree(), heapMin(), heapBlock(), WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
          (int)streamClients);
    lastCrumbFrames = total;
    lastNoMem = nm;
    lastInc = inc;
    lastLost = lost;
    lastCrumb = millis();
  }

  if (millis() - lastStats >= 5000) {
    uint32_t total = framesTotal;
    currentFps = (total - lastFrames) * 1000.0f / (millis() - lastStats);
    lastFrames = total;
    lastStats = millis();
    Serial.printf("[stats] %.1f fps, %u Bilder, %u verworfen, %d Zuschauer, Heap %u%s\n",
                  currentFps, (unsigned)total, (unsigned)framesDropped, (int)streamClients,
                  heapFree(), rescueMode ? ", NOTFALL-MODUS" : "");
  }
  delay(10);
}
