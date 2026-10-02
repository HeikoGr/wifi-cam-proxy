/*
 * Kameraverwaltung: findet Kameras per WLAN-Scan am SSID-Namen, verbindet sich und
 * startet im Video-Task die Sitzung des passenden Protokolls.
 *
 * Auswahl:
 *   1. Die zuletzt verbundene (oder in der Weboberfläche gewählte) Kamera, wenn sie
 *      in Reichweite ist. Beim Start wird sie ohne Scan direkt angesprochen.
 *   2. Sonst: genau eine erkannte Kamera in Reichweite -> diese.
 *   3. Mehrere erkannte Kameras -> warten, bis in der Weboberfläche (/cameras) eine
 *      gewählt wird. Bis dahin wird alle 20 s neu gesucht.
 *
 * Alle WLAN-Aufrufe laufen in loop() (cameraLoop); die HTTP-Tasks hinterlegen nur
 * Wünsche (Auswahl, Scan), damit sich WiFi.begin()/scan nie überschneiden.
 */

#include "camera.h"

#include <Preferences.h>
#include <WiFi.h>

#include <mutex>

#include "crashlog.h"

VideoStats stats;
CamTelemetry telemetry;
portMUX_TYPE infoMux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<int> ledRequest{-1};

void wifiApplyMode();              // main.cpp

void CamTelemetry::reset() {
  hasOrientation = false;
  battery = -1;
  charging = -1;
  led = -1;
  ledSupported = false;
  width = height = 0;
  portENTER_CRITICAL(&infoMux);
  vendor[0] = product[0] = firmware[0] = 0;
  portEXIT_CRITICAL(&infoMux);
}

// --- Protokolle und SSID-Muster ---------------------------------------------------
// Präfixe ohne Groß-/Kleinschreibung. Reihenfolge zählt (erster Treffer gewinnt).
// Quellen: king-cake/otoscope-windows, rico001/open-web-soulear (App-Präfixe),
// Fyfar/ms5-wifi-microscope (MS5), czietz/wifimicroscope (MaxSee).
struct SsidPattern {
  const char *prefix;
  CamProto proto;
};
static const SsidPattern SSID_PATTERNS[] = {
    {"Soulear", CamProto::I4season},       // Hopefox Find T u.a. (am Gerät verifiziert)
    {"SUEAR", CamProto::I4season},         // Suear-Ohrreiniger
    {"i4season", CamProto::I4season},
    {"inskam", CamProto::I4season},
    {"Yanxuan", CamProto::I4season},
    {"wifi_camera_", CamProto::I4season},  // MS5-Mikroskop (wifi_camera_MS5_XXXX)
    {"MAX-VIEW", CamProto::I4season},      // Vermutung: MAX-VIEW-App ist von i4season
    {"MAXVIEW", CamProto::I4season},
    {"Maxsee", CamProto::Jhcmd},           // MaxSee/JoyHonest, Kamera auf 192.168.29.1
    {"JH-", CamProto::Jhcmd},
};

const char *protoKey(CamProto p) {
  switch (p) {
    case CamProto::I4season: return "i4season";
    case CamProto::Jhcmd: return "jhcmd";
    case CamProto::Auto: return "auto";
    default: return "";
  }
}
const char *protoName(CamProto p) {
  switch (p) {
    case CamProto::I4season: return "i4season (Soulear, MS5, MAX-VIEW)";
    case CamProto::Jhcmd: return "MaxSee/JoyHonest (JHCMD)";
    case CamProto::Auto: return "automatisch";
    default: return "unbekannt";
  }
}
CamProto protoFromKey(const char *key) {
  if (!strcmp(key, "i4season")) return CamProto::I4season;
  if (!strcmp(key, "jhcmd")) return CamProto::Jhcmd;
  if (!strcmp(key, "auto") || !*key) return CamProto::Auto;
  return CamProto::None;
}
CamProto protoForSsid(const char *ssid) {
  for (const auto &p : SSID_PATTERNS)
    if (strncasecmp(ssid, p.prefix, strlen(p.prefix)) == 0) return p.proto;
  return CamProto::None;
}

// --- Zustand --------------------------------------------------------------------
enum class CamState : uint8_t { Off /* neu verbinden */, Connecting, Connected, Scanning, WaitChoice, Idle };
static const char *stateKey(CamState s) {
  switch (s) {
    case CamState::Connecting: return "connecting";
    case CamState::Connected: return "connected";
    case CamState::Scanning: return "scanning";
    case CamState::WaitChoice: return "choose";
    case CamState::Idle: return "searching";
    default: return "restart";
  }
}

struct ScanEntry {
  char ssid[33];
  int8_t rssi;
  bool open;
  CamProto proto;
};
static const int MAX_SCAN = 16;

static std::mutex camMutex;  // schützt alles bis zur Leerzeile
static std::atomic<CamState> state{CamState::Off};  // auch vom Netzwerk-Event gesetzt
static ScanEntry scanList[MAX_SCAN];
static int scanCount = 0;
static uint32_t scanAt = 0;           // millis() des letzten Scan-Ergebnisses
static char prefSsid[33] = "", prefPass[65] = "";
static CamProto prefProto = CamProto::Auto;
static char curSsid[33] = "", curPass[65] = "";
static CamProto curProto = CamProto::Auto;  // gewünscht (Auto möglich)
static bool selPending = false;              // Auswahl aus der Weboberfläche
static char selSsid[33], selPass[65];
static CamProto selProto;
static std::atomic<bool> scanPending{false};

// Für den Video-Task: aktive Sitzung, wird bei Wechsel über sessionGen neu angelegt
static std::atomic<CamProto> activeProto{CamProto::None};
static std::atomic<uint32_t> activeIp{0};
static std::atomic<uint32_t> sessionGen{0};
static std::atomic<bool> savePref{false};
static std::atomic<uint32_t> stateSince{0};
static uint32_t lostSince = 0;

bool cameraLinkUp() {
  return !rescueMode && !updating && state == CamState::Connected &&
         WiFi.status() == WL_CONNECTED;
}
CamProto cameraProto() { return activeProto; }

static void setState(CamState s) {
  if (s == state) return;
  state = s;
  stateSince = millis();
  crumb("Kamera: %s", stateKey(s));
}

// --- Video-Task -----------------------------------------------------------------
static void videoTask(void *) {
  static uint8_t pkt[2048] __attribute__((aligned(4)));  // Nutzdaten ab +16 bleiben ausgerichtet
  CamSession *session = nullptr;
  uint32_t gen = 0;
  for (;;) {
    uint32_t want = sessionGen;
    if (want != gen) {
      gen = want;
      delete session;  // nur die Sitzung der aktiven Kamera belegt RAM
      session = nullptr;
      clearFrame();
      telemetry.reset();
      ledRequest = -1;
      uint32_t ip = activeIp;
      switch (activeProto.load()) {
        case CamProto::I4season: session = createI4seasonSession(ip); break;
        case CamProto::Jhcmd: session = createJhcmdSession(ip); break;
        default: break;
      }
      crumb("Sitzung: %s", protoKey(activeProto));
    }
    if (!session) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    session->poll(pkt, sizeof(pkt));
  }
}

// --- NVS ------------------------------------------------------------------------
static void loadPref() {
  Preferences p;
  if (p.begin("otoskop", true)) {
    strlcpy(prefSsid, p.getString("cam_ssid", "").c_str(), sizeof(prefSsid));
    strlcpy(prefPass, p.getString("cam_pass", "").c_str(), sizeof(prefPass));
    prefProto = protoFromKey(p.getString("cam_proto", "auto").c_str());
    if (prefProto == CamProto::None) prefProto = CamProto::Auto;
    p.end();
  }
}

static void storePref() {
  char ssid[33], pass[65];
  CamProto proto;
  {
    std::lock_guard<std::mutex> lock(camMutex);
    strlcpy(ssid, prefSsid, sizeof(ssid));
    strlcpy(pass, prefPass, sizeof(pass));
    proto = prefProto;
  }
  Preferences p;
  if (p.begin("otoskop", false)) {
    p.putString("cam_ssid", ssid);
    p.putString("cam_pass", pass);
    p.putString("cam_proto", protoKey(proto));
    p.end();
  }
}

// --- Verbinden / Suchen (nur aus loop) ------------------------------------------
static void connectTo(const char *ssid, const char *pass, CamProto proto) {
  {
    std::lock_guard<std::mutex> lock(camMutex);
    strlcpy(curSsid, ssid, sizeof(curSsid));
    strlcpy(curPass, pass, sizeof(curPass));
    curProto = proto;
  }
  Serial.printf("[cam] verbinde mit \"%s\" (%s)\n", ssid, protoKey(proto));
  crumb("Kamera: verbinde %s", ssid);
  WiFi.disconnect();
  wifiApplyMode();
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, *pass ? pass : nullptr);
  setState(CamState::Connecting);
}

static void startScan(bool keepConnection) {
  if (!keepConnection) {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
  }
  WiFi.scanNetworks(true, false);  // asynchron, ohne versteckte Netze
  setState(CamState::Scanning);
}

// Nach einem Scan: Kamera wählen (siehe Kopfkommentar)
static void choose() {
  char ssid[33] = "", pass[65] = "";
  CamProto proto = CamProto::Auto;
  int found = 0;
  {
    std::lock_guard<std::mutex> lock(camMutex);
    for (int i = 0; i < scanCount; i++) {
      if (*prefSsid && !strcmp(scanList[i].ssid, prefSsid)) {
        strlcpy(ssid, prefSsid, sizeof(ssid));
        strlcpy(pass, prefPass, sizeof(pass));
        proto = prefProto;
        found = 1;
        break;
      }
      if (scanList[i].proto != CamProto::None && scanList[i].open) {
        if (!found++) strlcpy(ssid, scanList[i].ssid, sizeof(ssid));
      }
    }
  }
  if (found == 1) return connectTo(ssid, pass, proto);
  setState(found > 1 ? CamState::WaitChoice : CamState::Idle);
}

static void collectScan(int n) {
  std::lock_guard<std::mutex> lock(camMutex);
  scanCount = 0;
  for (int i = 0; i < n && scanCount < MAX_SCAN; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;
    bool dup = false;  // dieselbe SSID von mehreren APs nur einmal
    for (int k = 0; k < scanCount; k++) dup |= s == scanList[k].ssid;
    if (dup) continue;
    ScanEntry &e = scanList[scanCount++];
    strlcpy(e.ssid, s.c_str(), sizeof(e.ssid));
    e.rssi = WiFi.RSSI(i);
    e.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    e.proto = protoForSsid(e.ssid);
  }
  scanAt = millis();
}

void cameraBegin() {
  loadPref();
  xTaskCreatePinnedToCore(videoTask, "video", 4096, nullptr, 10, nullptr, 1);  // vor HTTP (3)
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // Modem-Sleep kostet bei UDP-Video Pakete
  if (*prefSsid) {
    connectTo(prefSsid, prefPass, prefProto);  // schneller Weg: ohne Scan
  } else {
    startScan(false);
  }
}

void cameraOnWifiGotIp() {
  if (rescueMode) return;
  uint32_t gw = (uint32_t)WiFi.gatewayIP();
  CamProto p;
  {
    std::lock_guard<std::mutex> lock(camMutex);
    p = curProto;
    if (p == CamProto::Auto) {
      p = protoForSsid(curSsid);
      // MaxSee-Kameras sitzen fest auf 192.168.29.1, sonst i4season (192.168.1.1)
      if (p == CamProto::None) p = gw == (uint32_t)IPAddress(192, 168, 29, 1) ? CamProto::Jhcmd : CamProto::I4season;
    }
    // Gemerkte Kamera aktualisieren (Speichern im NVS erledigt loop)
    if (strcmp(prefSsid, curSsid) || strcmp(prefPass, curPass) || prefProto != curProto) {
      strlcpy(prefSsid, curSsid, sizeof(prefSsid));
      strlcpy(prefPass, curPass, sizeof(prefPass));
      prefProto = curProto;
      savePref = true;
    }
  }
  if (!gw) gw = p == CamProto::Jhcmd ? (uint32_t)IPAddress(192, 168, 29, 1) : (uint32_t)IPAddress(192, 168, 1, 1);
  // Neue Sitzung nur, wenn sich Kamera oder Adresse geändert hat: nach kurzen
  // Funkabbrüchen läuft die bestehende Sitzung einfach weiter
  if (p != activeProto || gw != activeIp) {
    activeIp = gw;
    activeProto = p;
    sessionGen++;
  }
  state = CamState::Connected;
  stateSince = millis();
}

void cameraLoop() {
  if (savePref.exchange(false)) storePref();
  if (updating) return;
  if (rescueMode) {  // Kamera ruht; nur Scans für die Einrichtungsseite (/wifi-setup)
    if (state == CamState::Scanning) {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING && millis() - stateSince < 15000) return;
      if (n >= 0) collectScan(n);
      WiFi.scanDelete();
      setState(CamState::Off);
    } else if (scanPending.exchange(false)) {
      WiFi.scanNetworks(true, false);
      setState(CamState::Scanning);
    }
    return;
  }

  {
    std::unique_lock<std::mutex> lock(camMutex);
    if (selPending) {
      selPending = false;
      char ssid[33], pass[65];
      strlcpy(ssid, selSsid, sizeof(ssid));
      strlcpy(pass, selPass, sizeof(pass));
      CamProto proto = selProto;
      if (!*ssid) {  // Vorgabe löschen -> neu suchen und automatisch wählen
        prefSsid[0] = prefPass[0] = 0;
        prefProto = CamProto::Auto;
        lock.unlock();
        storePref();
        startScan(false);
        return;
      }
      lock.unlock();
      connectTo(ssid, pass, proto);
      return;
    }
  }
  bool connected = WiFi.status() == WL_CONNECTED;
  switch (state) {
    case CamState::Off: {
      char ssid[33], pass[65];
      CamProto proto;
      {
        std::lock_guard<std::mutex> lock(camMutex);
        strlcpy(ssid, curSsid, sizeof(ssid));
        strlcpy(pass, curPass, sizeof(pass));
        proto = curProto;
      }
      if (*ssid) connectTo(ssid, pass, proto);
      else startScan(false);
      break;
    }
    case CamState::Connecting:
      if (millis() - stateSince > CAM_CONNECT_TIMEOUT_MS) startScan(false);
      break;
    case CamState::Connected:
      if (connected) {
        lostSince = 0;
        if (scanPending.exchange(false)) startScan(true);  // Scan ohne Trennung (kurzes Ruckeln)
      } else if (!lostSince) {
        lostSince = millis();  // Auto-Reconnect versucht es erst selbst
      } else if (millis() - lostSince > CAM_LOST_RESCAN_MS) {
        lostSince = 0;
        startScan(false);
      }
      break;
    case CamState::Scanning: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) {
        if (millis() - stateSince > 15000) {  // Scan hängt
          WiFi.scanDelete();
          setState(CamState::Idle);
        }
        break;
      }
      if (n >= 0) collectScan(n);
      WiFi.scanDelete();
      if (connected) {  // Scan auf Wunsch während der Verbindung
        state = CamState::Connected;
        break;
      }
      choose();
      break;
    }
    case CamState::WaitChoice:
    case CamState::Idle:
      if (scanPending.exchange(false) ||
          millis() - stateSince > (state == CamState::Idle ? CAM_RESCAN_MS : CAM_CHOICE_RESCAN_MS))
        startScan(false);
      break;
  }
}

void cameraRestartWifi() {
  setState(CamState::Off);  // loop verbindet neu (Protokolländerung greift erst dann)
}

bool cameraSelect(const char *ssid, const char *pass, CamProto proto) {
  if (strlen(ssid) > 32 || strlen(pass) > 64 || proto == CamProto::None) return false;
  std::lock_guard<std::mutex> lock(camMutex);
  strlcpy(selSsid, ssid, sizeof(selSsid));
  strlcpy(selPass, pass, sizeof(selPass));
  selProto = proto;
  selPending = true;
  return true;
}

void cameraRequestScan() { scanPending = true; }

// --- JSON für /cameras ------------------------------------------------------------
static size_t jsonStr(char *out, size_t len, const char *s) {  // "…" mit Escapes
  size_t o = 0;
  auto put = [&](char c) {
    if (o + 1 < len) out[o] = c;
    o++;
  };
  put('"');
  for (; *s; s++) {
    unsigned char c = *s;
    if (c == '"' || c == '\\') {
      put('\\');
      put(c);
    } else if (c < 0x20) {
      char u[8];
      snprintf(u, sizeof(u), "\\u%04x", c);
      for (char *q = u; *q; q++) put(*q);
    } else {
      put(c);
    }
  }
  put('"');
  if (len) out[min(o, len - 1)] = 0;
  return o;
}

size_t cameraJson(char *out, size_t len) {
  size_t o = 0;
  auto room = [&]() { return o < len ? len - o : 0; };
  auto add = [&](size_t n) { o += n; };
  char vendor[33], product[33], firmware[17];
  portENTER_CRITICAL(&infoMux);
  strlcpy(vendor, telemetry.vendor, sizeof(vendor));
  strlcpy(product, telemetry.product, sizeof(product));
  strlcpy(firmware, telemetry.firmware, sizeof(firmware));
  portEXIT_CRITICAL(&infoMux);

  std::lock_guard<std::mutex> lock(camMutex);
  int recognized = 0;
  for (int i = 0; i < scanCount; i++) recognized += scanList[i].proto != CamProto::None;
  add(snprintf(out + o, room(), "{\"state\":\"%s\",\"ssid\":", stateKey(state.load())));
  add(jsonStr(out + o, room(), state == CamState::Connected || state == CamState::Connecting ? curSsid : ""));
  add(snprintf(out + o, room(), ",\"proto\":\"%s\",\"preferred\":", protoKey(activeProto)));
  add(jsonStr(out + o, room(), prefSsid));
  add(snprintf(out + o, room(),
               ",\"pref_proto\":\"%s\",\"recognized\":%d,\"scan_age_s\":%ld,"
               "\"orientation\":%s,\"battery\":%d,\"charging\":%d,\"led\":%d,\"led_supported\":%s,"
               "\"width\":%u,\"height\":%u,\"vendor\":",
               protoKey(prefProto), recognized, scanAt ? (long)((millis() - scanAt) / 1000) : -1L,
               telemetry.hasOrientation ? "true" : "false", (int)telemetry.battery,
               (int)telemetry.charging, (int)telemetry.led, telemetry.ledSupported ? "true" : "false",
               (unsigned)telemetry.width, (unsigned)telemetry.height));
  add(jsonStr(out + o, room(), vendor));
  add(snprintf(out + o, room(), ",\"product\":"));
  add(jsonStr(out + o, room(), product));
  add(snprintf(out + o, room(), ",\"firmware\":"));
  add(jsonStr(out + o, room(), firmware));
  add(snprintf(out + o, room(), ",\"networks\":["));
  for (int i = 0; i < scanCount; i++) {
    add(snprintf(out + o, room(), "%s{\"ssid\":", i ? "," : ""));
    add(jsonStr(out + o, room(), scanList[i].ssid));
    add(snprintf(out + o, room(), ",\"rssi\":%d,\"open\":%s,\"proto\":\"%s\"}", scanList[i].rssi,
                 scanList[i].open ? "true" : "false", protoKey(scanList[i].proto)));
  }
  add(snprintf(out + o, room(), "]}"));
  return min(o, len ? len - 1 : 0);
}
