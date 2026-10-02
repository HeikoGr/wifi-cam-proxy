/*
 * Camera management: finds cameras by SSID in a Wi-Fi scan, connects and starts the
 * session of the matching protocol in the video task.
 *
 * Selection:
 *   1. The last connected (or selected in the web UI) camera, if it is in range. At
 *      startup it is contacted directly without a scan.
 *   2. Otherwise: exactly one recognised camera in range -> that one.
 *   3. Several recognised cameras -> wait until one is selected in the web UI
 *      (/cameras). Until then a new scan runs every 20 s.
 *
 * All Wi-Fi calls run in loop() (cameraLoop); the HTTP tasks only leave requests
 * (selection, scan), so WiFi.begin()/scan never overlap.
 */

#include "camera.h"

#include <WiFi.h>

#include <mutex>

#include "crashlog.h"
#include "settings.h"

VideoStats stats;
CamTelemetry telemetry;
portMUX_TYPE infoMux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<int> ledRequest{-1};
std::atomic<int> ledLevel{100};

void wifiApplyMode();              // main.cpp

void CamTelemetry::reset() {
  hasOrientation = false;
  battery = -1;
  batteryRaw = -1;
  charging = -1;
  led = -1;
  ledSupported = false;
  ledDimmable = false;
  width = height = 0;
  portENTER_CRITICAL(&infoMux);
  vendor[0] = product[0] = firmware[0] = 0;
  portEXIT_CRITICAL(&infoMux);
}

// --- Protocols and SSID patterns ---------------------------------------------------
// Case-insensitive prefixes. Order matters (first match wins).
// Sources: king-cake/otoscope-windows, rico001/open-web-soulear (app prefixes),
// Fyfar/ms5-wifi-microscope (MS5), czietz/wifimicroscope (MaxSee).
struct SsidPattern {
  const char *prefix;
  CamProto proto;
};
static const SsidPattern SSID_PATTERNS[] = {
    {"Soulear", CamProto::I4season},       // Hopefox Find T and others (verified on the device)
    {"SUEAR", CamProto::I4season},         // Suear ear cleaners
    {"i4season", CamProto::I4season},
    {"inskam", CamProto::I4season},
    {"Yanxuan", CamProto::I4season},
    {"wifi_camera_", CamProto::I4season},  // MS5 microscope (wifi_camera_MS5_XXXX)
    {"MAX-VIEW", CamProto::Jhcmd},         // MAXVIEW-xxxx sits at 192.168.29.1 and does not
    {"MAXVIEW", CamProto::Jhcmd},          // answer i4season (observed), so MaxSee family
    {"Maxsee", CamProto::Jhcmd},           // MaxSee/JoyHonest, camera at 192.168.29.1
    {"JH-", CamProto::Jhcmd},
};

// --- Session diagnostics -------------------------------------------------------------
static char diagBuf[1024];  // ~8 lines of hex
static size_t diagLen = 0;
static portMUX_TYPE diagMux = portMUX_INITIALIZER_UNLOCKED;

void diagLog(const char *fmt, ...) {
  char line[128];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  n = min(n, (int)sizeof(line) - 1);
  Serial.printf("%s\r\n", line);
  portENTER_CRITICAL(&diagMux);
  // Ring: if full, drop the oldest lines (the first line, the session header, stays)
  char *second = strchr(diagBuf, '\n');
  while (diagLen + n + 2 > sizeof(diagBuf) && second && second[1]) {
    char *third = strchr(second + 1, '\n');
    if (!third) break;
    size_t drop = third - second;  // the line after the first one
    memmove(second + 1, third + 1, diagLen - (third + 1 - diagBuf) + 1);
    diagLen -= drop;
  }
  if (diagLen + n + 2 <= sizeof(diagBuf)) {
    memcpy(diagBuf + diagLen, line, n);
    diagLen += n;
    diagBuf[diagLen++] = '\n';
    diagBuf[diagLen] = 0;
  }
  portEXIT_CRITICAL(&diagMux);
}

void diagReset() {
  portENTER_CRITICAL(&diagMux);
  diagLen = 0;
  diagBuf[0] = 0;
  portEXIT_CRITICAL(&diagMux);
}

static std::mutex diagRawMutex;
static Frame diagRaw;
static uint32_t diagRawAt = 0;  // when the capture was handed over (freed after 30 s)
static std::atomic<bool> diagRawWant{false};

static uint8_t diagSendBuf[64];
static size_t diagSendLen = 0;
static uint16_t diagSendPort = 0;
void diagSendPut(uint16_t port, const uint8_t *data, size_t len) {
  portENTER_CRITICAL(&diagMux);
  diagSendLen = min(len, sizeof(diagSendBuf));
  memcpy(diagSendBuf, data, diagSendLen);
  diagSendPort = port;
  portEXIT_CRITICAL(&diagMux);
}
bool diagSendTake(uint16_t &port, uint8_t *data, size_t &len) {
  bool have = false;
  portENTER_CRITICAL(&diagMux);
  if (diagSendLen) {
    len = min(len, diagSendLen);
    memcpy(data, diagSendBuf, len);
    port = diagSendPort;
    diagSendLen = 0;
    have = true;
  }
  portEXIT_CRITICAL(&diagMux);
  return have;
}

void diagRawRequest() { diagRawWant = true; }
bool diagRawWanted() { return diagRawWant; }
void diagRawPut(const Frame &f) {
  std::lock_guard<std::mutex> lock(diagRawMutex);
  diagRaw = f;
  diagRawAt = millis();
  diagRawWant = false;
}
bool diagRawTake(Frame &out) {
  std::lock_guard<std::mutex> lock(diagRawMutex);
  if (!diagRaw) return false;
  out = diagRaw;
  diagRaw.reset();
  return true;
}

void diagCopy(char *out, size_t len) {
  portENTER_CRITICAL(&diagMux);
  strlcpy(out, diagBuf, len);
  portEXIT_CRITICAL(&diagMux);
}

const CamProto PROTO_CHOICES[] = {CamProto::Auto, CamProto::I4season, CamProto::Jhcmd};
const int PROTO_CHOICE_COUNT = sizeof(PROTO_CHOICES) / sizeof(PROTO_CHOICES[0]);

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
    case CamProto::I4season: return "i4season (Soulear, MS5)";
    case CamProto::Jhcmd: return "MaxSee/JoyHonest/MAX-VIEW (JHCMD)";
    case CamProto::Auto: return "automatic";
    default: return "unknown";
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

// --- State ----------------------------------------------------------------------
enum class CamState : uint8_t { Off /* reconnect */, Connecting, Connected, Scanning, WaitChoice, Idle };
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

static const int MAX_SCAN = 16;

static std::mutex camMutex;  // guards everything up to the blank line
static std::atomic<CamState> state{CamState::Off};  // also set by the network event
static ScanEntry scanList[MAX_SCAN];
static int scanCount = 0;
static uint32_t scanAt = 0;           // millis() of the last scan result
static char prefSsid[33] = "", prefPass[65] = "";
static CamProto prefProto = CamProto::Auto;
static char curSsid[33] = "", curPass[65] = "";
static CamProto curProto = CamProto::Auto;  // requested (Auto possible)
static bool selPending = false;              // selection from the web UI
static char selSsid[33], selPass[65];
static CamProto selProto;
static std::atomic<bool> scanPending{false};
static std::atomic<bool> autoScan{true};      // see cameraSetAutoScan()
static std::atomic<bool> paused{false};       // see cameraPause()

// For the video task: active session, recreated via sessionGen on change
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
  crumb("camera: %s", stateKey(s));
}

// --- Video task -----------------------------------------------------------------
static void videoTask(void *) {
  static uint8_t pkt[2048] __attribute__((aligned(4)));  // payload at +16 stays aligned
  CamSession *session = nullptr;
  uint32_t gen = 0;
  for (;;) {
    uint32_t want = sessionGen;
    if (want != gen) {
      gen = want;
      delete session;  // only the active camera's session occupies RAM
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
      crumb("session: %s", protoKey(activeProto));
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
  nvsRead([](Preferences &p) {
    strlcpy(prefSsid, p.getString("cam_ssid", "").c_str(), sizeof(prefSsid));
    strlcpy(prefPass, p.getString("cam_pass", "").c_str(), sizeof(prefPass));
    prefProto = protoFromKey(p.getString("cam_proto", "auto").c_str());
    if (prefProto == CamProto::None) prefProto = CamProto::Auto;
    autoScan = p.getBool("cam_autoscan", true);
  });
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
  nvsWrite([&](Preferences &p) {
    p.putString("cam_ssid", ssid);
    p.putString("cam_pass", pass);
    p.putString("cam_proto", protoKey(proto));
  });
}

// --- Connect / scan (from loop only) ----------------------------------------------
static void connectTo(const char *ssid, const char *pass, CamProto proto) {
  {
    std::lock_guard<std::mutex> lock(camMutex);
    strlcpy(curSsid, ssid, sizeof(curSsid));
    strlcpy(curPass, pass, sizeof(curPass));
    curProto = proto;
  }
  Serial.printf("[cam] connecting to \"%s\" (%s)\r\n", ssid, protoKey(proto));
  crumb("camera: connecting %s", ssid);
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
  WiFi.scanNetworks(true, false);  // asynchronous, without hidden networks
  setState(CamState::Scanning);
}

// After a scan: choose a camera (see header comment)
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
    bool dup = false;  // same SSID from several APs only once
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
  xTaskCreatePinnedToCore(videoTask, "video", 4096, nullptr, 10, nullptr, 1);  // above HTTP (3)
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep loses packets with UDP video
  if (*prefSsid) {
    connectTo(prefSsid, prefPass, prefProto);  // fast path: without a scan
  } else {
    startScan(false);
  }
}

void cameraOnWifiGotIp() {
  if (rescueMode || paused) return;
  uint32_t gw = (uint32_t)WiFi.gatewayIP();
  CamProto p;
  {
    std::lock_guard<std::mutex> lock(camMutex);
    p = curProto;
    if (p == CamProto::Auto) {
      // The address beats the name: MaxSee cameras are fixed at 192.168.29.1, the SSID
      // patterns partly rest on assumptions. Otherwise by name, else i4season.
      if (gw == JHCMD_CAM_IP) p = CamProto::Jhcmd;
      else p = protoForSsid(curSsid);
      if (p == CamProto::None) p = CamProto::I4season;
    }
    // Update the remembered camera (loop stores it in NVS)
    if (strcmp(prefSsid, curSsid) || strcmp(prefPass, curPass) || prefProto != curProto) {
      strlcpy(prefSsid, curSsid, sizeof(prefSsid));
      strlcpy(prefPass, curPass, sizeof(prefPass));
      prefProto = curProto;
      savePref = true;
    }
  }
  if (!gw) gw = p == CamProto::Jhcmd ? JHCMD_CAM_IP : I4SEASON_CAM_IP;
  // New session only if camera or address changed: after short radio dropouts the
  // existing session simply continues
  if (p != activeProto || gw != activeIp) {
    activeIp = gw;
    activeProto = p;
    sessionGen++;
  }
  state = CamState::Connected;
  stateSince = millis();
}

bool cameraAutoScan() { return autoScan; }
void cameraSetAutoScan(bool on) {
  autoScan = on;
  nvsWrite([&](Preferences &p) { p.putBool("cam_autoscan", on); });
  crumb("camera: automatic scan %s", on ? "on" : "off");
}

// Without automatic scan: try the current camera again instead of scanning
static bool reconnectInstead() {
  if (autoScan) return false;
  char ssid[33], pass[65];
  CamProto proto;
  {
    std::lock_guard<std::mutex> lock(camMutex);
    strlcpy(ssid, curSsid, sizeof(ssid));
    strlcpy(pass, curPass, sizeof(pass));
    proto = curProto;
  }
  if (!*ssid) return false;
  connectTo(ssid, pass, proto);
  return true;
}

void cameraPause(bool on) {
  if (on == paused) return;
  paused = on;
  if (on) {
    activeProto = CamProto::None;  // video task ends the session
    activeIp = 0;
    sessionGen++;
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
    crumb("camera: paused");
  } else {
    crumb("camera: resumed");
  }
  setState(CamState::Off);  // after resuming, the loop reconnects to the current camera
}
bool cameraPaused() { return paused; }

void cameraLoop() {
  if (savePref.exchange(false)) storePref();
  {
    // A raw capture (/camdiag/raw) nobody fetched would keep a whole frame (up to
    // 80 KB) until the next restart: free it after 30 s
    std::lock_guard<std::mutex> lock(diagRawMutex);
    if (diagRaw && millis() - diagRawAt > 30000) diagRaw.reset();
  }
  if (updating || paused) return;
  if (rescueMode) {  // camera idle; only scans for the setup page (/wifi-setup)
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
      if (!*ssid) {  // clear preference -> scan again and choose automatically
        prefSsid[0] = prefPass[0] = 0;
        curSsid[0] = curPass[0] = 0;  // so that "reconnect instead of scan" does not take it
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
      if (millis() - stateSince > CAM_CONNECT_TIMEOUT_MS && !reconnectInstead()) startScan(false);
      break;
    case CamState::Connected:
      if (connected) {
        lostSince = 0;
        if (scanPending.exchange(false)) startScan(true);  // scan without disconnecting (short stutter)
      } else if (!lostSince) {
        lostSince = millis();  // auto-reconnect tries on its own first
      } else if (millis() - lostSince > CAM_LOST_RESCAN_MS) {
        lostSince = 0;
        if (!reconnectInstead()) startScan(false);
      }
      break;
    case CamState::Scanning: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) {
        if (millis() - stateSince > 15000) {  // scan stuck
          WiFi.scanDelete();
          setState(CamState::Idle);
        }
        break;
      }
      if (n >= 0) collectScan(n);
      WiFi.scanDelete();
      if (connected) {  // requested scan while connected
        state = CamState::Connected;
        break;
      }
      choose();
      break;
    }
    case CamState::WaitChoice:
    case CamState::Idle:
      if (scanPending.exchange(false) ||
          (autoScan && millis() - stateSince > (state == CamState::Idle ? CAM_RESCAN_MS : CAM_CHOICE_RESCAN_MS)))
        startScan(false);
      else if (!autoScan && millis() - stateSince > CAM_RESCAN_MS && !reconnectInstead())
        stateSince = millis();  // no camera remembered: wait for "Rescan"
      break;
  }
}

void cameraRestartWifi() {
  setState(CamState::Off);  // loop reconnects (protocol change only takes effect then)
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


// --- JSON for /cameras ------------------------------------------------------------
static size_t jsonStr(char *out, size_t len, const char *s) {  // "…" with escapes
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
               ",\"pref_proto\":\"%s\",\"autoscan\":%s,\"recognized\":%d,\"scan_age_s\":%ld,"
               "\"orientation\":%s,\"battery\":%d,\"charging\":%d,\"led\":%d,\"led_supported\":%s,"
               "\"led_dimmable\":%s,\"led_level\":%d,\"battery_raw\":%d,"
               "\"width\":%u,\"height\":%u,\"vendor\":",
               protoKey(prefProto), autoScan ? "true" : "false", recognized, scanAt ? (long)((millis() - scanAt) / 1000) : -1L,
               telemetry.hasOrientation ? "true" : "false", (int)telemetry.battery,
               (int)telemetry.charging, (int)telemetry.led, telemetry.ledSupported ? "true" : "false",
               telemetry.ledDimmable ? "true" : "false", (int)ledLevel, (int)telemetry.batteryRaw,
               (unsigned)telemetry.width, (unsigned)telemetry.height));
  add(jsonStr(out + o, room(), vendor));
  add(snprintf(out + o, room(), ",\"product\":"));
  add(jsonStr(out + o, room(), product));
  add(snprintf(out + o, room(), ",\"firmware\":"));
  add(jsonStr(out + o, room(), firmware));
  add(snprintf(out + o, room(), ",\"protocols\":["));
  for (int i = 0; i < PROTO_CHOICE_COUNT; i++)
    add(snprintf(out + o, room(), "%s[\"%s\",\"%s\"]", i ? "," : "", protoKey(PROTO_CHOICES[i]),
                 protoName(PROTO_CHOICES[i])));
  add(snprintf(out + o, room(), "],\"networks\":["));
  for (int i = 0; i < scanCount; i++) {
    add(snprintf(out + o, room(), "%s{\"ssid\":", i ? "," : ""));
    add(jsonStr(out + o, room(), scanList[i].ssid));
    add(snprintf(out + o, room(), ",\"rssi\":%d,\"open\":%s,\"proto\":\"%s\"}", scanList[i].rssi,
                 scanList[i].open ? "true" : "false", protoKey(scanList[i].proto)));
  }
  add(snprintf(out + o, room(), "]}"));
  return min(o, len ? len - 1 : 0);
}

const char *cameraStateKey() { return stateKey(state.load()); }

int cameraNetworks(ScanEntry *out, int max) {
  std::lock_guard<std::mutex> lock(camMutex);
  int n = min(max, scanCount);
  memcpy(out, scanList, n * sizeof(ScanEntry));
  return n;
}

void cameraCurrentSsid(char *out, size_t len) {
  std::lock_guard<std::mutex> lock(camMutex);
  CamState s = state;
  strlcpy(out, s == CamState::Connected || s == CamState::Connecting ? curSsid : "", len);
}
