/*
 * WiFi-Cam-Proxy for ESP32 + LAN8720 (ZB-GW03 v1.4, WT32-ETH01)
 *
 * - Wi-Fi (client): looks for Wi-Fi cameras (otoscopes, microscopes) by SSID and
 *   connects to one of them (src/camera.cpp, protocols in src/cam_*.cpp)
 * - Ethernet (DHCP): provides an MJPEG server in the home network
 *     http://otoskop.local/          browser
 *     http://otoskop.local/stream    VLC / Home Assistant
 *     http://otoskop.local/snapshot  single frame
 *     http://otoskop.local/cameras   choose camera
 *     http://otoskop.local/status    JSON with statistics
 *     http://otoskop.local/update    firmware update in the browser
 *
 * Rescue mode (src/rescue.cpp): if Ethernet has no IP for RESCUE_TIMEOUT_MS, Wi-Fi
 * joins the home Wi-Fi or opens its own access point with a setup page.
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
#include "sniffer.h"

#ifndef GIT_REV
#define GIT_REV "unknown"  // set by git_rev.py
#endif
static const char FW_VERSION[] = __DATE__ " " __TIME__;
static const char FW_COMMIT[] = GIT_REV;

// Survives restarts (but not power loss) -> counts resets since power-on
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
static volatile bool ethStarted = false;  // LAN8720 initialised
static bool ethBeginOk = false;
volatile bool rescueMode = false;
std::atomic<bool> updating{false};
static std::atomic<int> streamClients{0};
static std::atomic<uint32_t> streamGen{0};  // number of the newest stream (see handleStream)
static std::atomic<int> sseClients{0};
// Wi-Fi mode towards the camera (index into WIFI_MODES), see wifiApplyMode()
static const char *const WIFI_MODES[] = {"bgn", "bg", "b"};
static int wifiModeIndex(const String &m) {
  for (int i = 0; i < 3; i++)
    if (m == WIFI_MODES[i]) return i;
  return -1;
}
static std::atomic<int> wifiMode{wifiModeIndex(WIFI_MODE_DEFAULT)};
// Wi-Fi transmit power in 0.25 dBm (8..84). High power disturbs the Ethernet clock the
// ESP32 generates itself on GPIO17 -> lost Ethernet packets (measured on the device)
static std::atomic<int> wifiTxQdbm{WIFI_TX_QDBM_DEFAULT};

static std::atomic<bool> eth10{ETH_10MBIT_DEFAULT};  // Ethernet 10 Mbit only, see ethApplySpeed()
static void ethApplySpeed();

// The Zigbee module (ZB-GW03 only) is not needed: hold it in reset, saves power
static void setZigbee(bool on) {
  if (ZIGBEE_NRST_GPIO < 0) return;
  pinMode(ZIGBEE_NRST_GPIO, OUTPUT);
  digitalWrite(ZIGBEE_NRST_GPIO, on ? HIGH : LOW);
}

static std::atomic<int> clientTasks{0};
static float currentFps = 0;

#include "web_ui.h"  // start page, calibration, update page, shared JS

// --- HTTP server ----------------------------------------------------------------
// Send non-blocking and retry ourselves after 5 ms. A blocking send() sleeps in lwIP
// on a brief memory shortage (ERR_MEM) until the connection's next poll timer, which
// only runs ~1x per second -> 1 s frozen frame (measured: everything acknowledged,
// window open, still a send pause).
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
    if (!busy || millis() - lastProgress > 5000) return false;  // error or nothing for 5 s
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

// Value of a request header (case-insensitive), "" if not present
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

// Collects small pieces and sends them in blocks. Every send() is a job for the lwIP
// task, which also processes the Wi-Fi packets from the camera; ~30 jobs per frame
// slow down reception there. Open issue: at high data rates single TCP segments get
// lost on the way into the LAN (probably the Ethernet controller's transmit buffer,
// 10 x 512 bytes); TCP only retransmits them after ~1 s. The block size does not help.
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
    if (!buf) {  // no memory for the buffer -> send directly
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
  // Frame chunk possibly in IRAM: read word by word, pass on byte by byte
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
    sendText(fd, 503, "Service Unavailable", "Too many viewers");
    return;
  }
  static const char hdr[] =
      "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
      "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
  crumb("stream open (%d viewers)", (int)streamClients);
  const uint32_t me = ++streamGen;
  if (sendAll(fd, hdr, sizeof(hdr) - 1)) {
    uint32_t seq = 0;
    uint32_t lastCheck = millis();
    uint32_t lastSent = 0;  // for STREAM_MAX_FPS
    BlockSender out(fd);  // one buffer per viewer, for all frames
    Frame frame;
    while (!updating) {
      uint32_t s = getFrame(frame);
      if (s == seq || !frame) {
        // replaced by a newer viewer (large frames, see below): end while waiting as well,
        // a stale connection would keep its task and send buffer
        bool replaced = frame && frame.size() > FRAME_RESERVE_FROM && me != streamGen;
        frame.reset();
        if (replaced) {
          crumb("stream ended: large frames, a newer viewer took over");
          break;
        }
        // While waiting, check now and then whether the client is still there
        if (millis() - lastCheck > 1000) {
          if (clientClosed(fd)) break;
          lastCheck = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      // At most STREAM_MAX_FPS: wait and then send the newest frame (frames in between
      // are skipped, the stream does not fall behind)
      if (STREAM_MAX_FPS > 0 && millis() - lastSent < 1000 / STREAM_MAX_FPS) {
        frame.reset();
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      lastSent = millis();  // start to start, so the sending time is not added on top
      seq = s;
      if (clientClosed(fd)) {
        frame.reset();
        break;
      }
      // Large frames (720p microscopes): only one viewer, the newest wins. Each viewer
      // holds the frame it is sending; with several 50-80 KB frames the heap runs out,
      // reception drops frames and the Wi-Fi stalls. Also clears stale connections of
      // a tab that reconnected.
      if (frame.size() > FRAME_RESERVE_FROM && me != streamGen) {
        frame.reset();
        crumb("stream ended: large frames, a newer viewer took over");
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
  crumb("stream closed (%d viewers)", (int)streamClients);
}

static float wifiTxDbm() {
  int8_t q = 0;
  return esp_wifi_get_max_tx_power(&q) == ESP_OK ? q / 4.0f : 0;
}

static void handleStatus(int fd) {
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  char crash[320];
  crashlogFormat(crash, sizeof(crash));
  // Times of the last clean stalls, oldest first
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
                   "{\"version\":\"%s\",\"commit\":\"%s\",\"reset_reason\":\"%s\",\"boot_count\":%u,\"mode\":\"%s\",\"fps\":%.1f,\"frames\":%u,"
                   "\"dropped\":%u,\"drop_nomem\":%u,\"drop_toobig\":%u,\"drop_incomplete\":%u,\"packets_lost\":%u,\"damaged\":%u,\"max_frame\":%u,\"handshakes\":%u,\"keepalives\":%u,\"stalls_loss\":%u,\"stalls_clean\":%u,\"clean_stall_times\":[%s],\"stream_clients\":%d,\"sse_clients\":%d,\"client_tasks\":%d,"
                   "\"wifi_connected\":%s,\"wifi_ssid\":\"%s\",\"wifi_rssi\":%d,\"wifi_mode\":\"%s\",\"wifi_tx_dbm\":%.2f,\"wifi_ip\":\"%s\",\"home_ssid\":\"%s\",\"ap\":\"%s\",\"eth10\":%d,"
                   "\"cam_proto\":\"%s\",\"battery\":%d,\"led\":%d,"
                   "\"eth_begin\":%s,\"eth_started\":%s,\"eth_link\":%s,\"eth_speed\":%d,\"eth_full_duplex\":%s,\"eth_tx_store_forward\":%d,\"eth_ip\":\"%s\",\"free_heap\":%u,\"max_alloc\":%u,\"min_heap\":%u,\"iram_heap\":%u,\"psram\":%u,\"uptime_s\":%lu,\"last_crash\":\"%s\"}",
                   FW_VERSION, FW_COMMIT, resetReasonText(), (unsigned)bootCount, rescueMode ? "rescue" : "normal", currentFps,
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

// Firmware as raw data in the body (application/octet-stream), e.g. from the
// update page or via: curl --data-binary @firmware.bin http://otoskop.local/update
static void handleUpdate(int fd, const char *req, const uint8_t *body, size_t bodyLen) {
  if (!authorized(req)) return sendText(fd, 401, "Unauthorized", "Wrong OTA password");
  size_t total = headerValue(req, "Content-Length").toInt();
  if (total == 0) return sendText(fd, 411, "Length Required", "Content-Length missing");
  if (updating.exchange(true)) return sendText(fd, 409, "Conflict", "Update already running");

  Serial.printf("[update] web update, %u bytes\r\n", (unsigned)total);
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
    if (!chunk) err = "Out of memory";
    while (!err && done < total) {
      int n = recv(fd, chunk.get(), min((size_t)2048, total - done), 0);
      if (n <= 0) {
        err = "Connection aborted";
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
    Serial.printf("[update] error: %s\r\n", err);
    char msg[128];
    snprintf(msg, sizeof(msg), "Update failed: %s", err);
    return sendText(fd, 500, "Internal Server Error", msg);
  }
  Serial.println("[update] OK, restarting");
  sendText(fd, 200, "OK", "Update successful, device is restarting");
  delay(500);
  ESP.restart();
}

// Orientation stream, see below
static void handleOrientationLoop(int fd);

// --- Wi-Fi mode towards the camera (switchable on the update page, in NVS) ---------
// "bgn": 802.11n with packet aggregation (fast, but one missing sub-packet holds up
//        the whole block); "bg": without 11n, every packet on its own (default);
// "b":   802.11b only, slow (max. 11 Mbit/s), but most robust with a weak signal
void wifiApplyMode() {
  int mode = wifiMode;
  uint8_t proto = mode == 2   ? WIFI_PROTOCOL_11B
                  : mode == 1 ? (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G)
                              : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  esp_wifi_set_protocol(WIFI_IF_STA, proto);
  if (mode == 0) esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);  // 20 instead of 40 MHz
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
  if (strncmp(path, "/wifi/tx/", 9) == 0) {  // transmit power, takes effect immediately
    int tx = atoi(path + 9);
    if (tx < 8 || tx > 84) return sendText(fd, 400, "Bad Request", "Transmit power 8..84 (x 0.25 dBm)");
    Preferences p;
    if (p.begin("otoskop", false)) {
      p.putInt("wifitx", tx);
      p.end();
    }
    wifiTxQdbm = tx;
    esp_wifi_set_max_tx_power(tx);
    crumb("Wi-Fi transmit power -> %.2f dBm", tx / 4.0);
    return sendText(fd, 200, "OK", "Transmit power set");
  }
  String m = String(path + strlen("/wifi/"));
  int i = wifiModeIndex(m);
  if (i < 0) return sendText(fd, 400, "Bad Request", "Mode: bgn, bg or b");
  Preferences p;
  if (p.begin("otoskop", false)) {
    p.putString("wifimode", m);
    p.end();
  }
  wifiMode = i;
  crumb("Wi-Fi mode -> %s, reconnecting", m.c_str());
  sendText(fd, 200, "OK", "Wi-Fi mode set, reconnecting");
  // The protocol change only takes effect on a new connection
  if (!rescueMode) cameraRestartWifi();  // in rescue mode only at the next normal operation
}

// --- Orientation calibration (JSON, computed by the web pages, stored in NVS) -----
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
  if (total == 0 || total > CALIB_MAX) return sendText(fd, 413, "Payload Too Large", "Invalid length");
  char buf[CALIB_MAX + 1];
  size_t have = min(bodyLen, total);
  memcpy(buf, body, have);
  while (have < total) {
    int n = recv(fd, buf + have, total - have, 0);
    if (n <= 0) return sendText(fd, 400, "Bad Request", "Connection aborted");
    have += n;
  }
  buf[total] = 0;
  if (buf[0] != '{' || buf[total - 1] != '}') return sendText(fd, 400, "Bad Request", "Not a JSON object");

  Preferences p;
  bool ok = p.begin("otoskop", false) && p.putString("calib", buf) > 0;
  p.end();
  if (!ok) return sendText(fd, 500, "Internal Server Error", "Saving to NVS failed");
  {
    std::lock_guard<std::mutex> lock(calibMutex);
    calibJson = buf;
  }
  crumb("calibration stored (%u bytes)", (unsigned)total);
  sendText(fd, 200, "OK", "Saved");
}

// Orientation as server-sent events: one open connection, new values ~17x per second
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
    // new values max. 25x/s (the otoscope delivers ~17); otherwise a keepalive every 5 s so dead clients are noticed
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
    if (clientClosed(fd)) break;  // browser left the page -> slot free right away
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Ethernet 10 Mbit only (1) or 100 Mbit (0): POST /eth10/<0|1>, stored in NVS
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
  sendText(fd, 200, "OK", "set, Ethernet renegotiates (link briefly down)");
  delay(200);
  ethApplySpeed();
}

// --- Camera selection (/cameras) ---------------------------------------------------
// Value from a form body (application/x-www-form-urlencoded), decoded
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
      if (o + 1 >= len) return false;  // too long
      out[o++] = c;
    }
    out[o] = 0;
    return true;
  }
  out[0] = 0;
  return false;
}

// Read the complete form body (max. FORM_MAX bytes); false = response already sent
static const size_t FORM_MAX = 256;
static bool readForm(int fd, const char *req, const uint8_t *body, size_t bodyLen, char *buf) {
  size_t total = headerValue(req, "Content-Length").toInt();
  if (total == 0 || total > FORM_MAX) {
    sendText(fd, 413, "Payload Too Large", "Invalid length");
    return false;
  }
  size_t have = min(bodyLen, total);
  memcpy(buf, body, have);
  while (have < total) {
    int n = recv(fd, buf + have, total - have, 0);
    if (n <= 0) {
      sendText(fd, 400, "Bad Request", "Connection aborted");
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
    return sendText(fd, 400, "Bad Request", "SSID max. 32 characters, password empty or 8-64 characters");
  sendText(fd, 200, "OK", !*ssid ? "Home Wi-Fi deleted" : rescueMode ? "Saved, connecting…" : "Saved (used in rescue mode)");
}

static void handleCameraSelect(int fd, const char *req, const uint8_t *body, size_t bodyLen) {
  char buf[FORM_MAX + 1];
  if (!readForm(fd, req, body, bodyLen, buf)) return;
  char ssid[34], pass[66], proto[12];
  formValue(buf, "ssid", ssid, sizeof(ssid));
  formValue(buf, "pass", pass, sizeof(pass));
  formValue(buf, "proto", proto, sizeof(proto));
  if (!cameraSelect(ssid, pass, protoFromKey(proto)))
    return sendText(fd, 400, "Bad Request", "Invalid SSID (max. 32), password (max. 64) or protocol");
  sendText(fd, 202, "Accepted", *ssid ? "Connecting…" : "Selection cleared, choosing automatically");
}

static void handleCamerasJson(int fd) {
  std::unique_ptr<char[]> json(new (std::nothrow) char[3072]);
  if (!json) return sendText(fd, 503, "Service Unavailable", "Out of memory");
  size_t n = cameraJson(json.get(), 3072);
  sendResponse(fd, 200, "OK", "application/json", json.get(), n);
}

static void clientTask(void *arg) {
  int fd = (int)(intptr_t)arg;
  timeval rcv = {5, 0}, snd = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  // Read the header up to the blank line; whatever follows in the buffer is body
  static const size_t REQ_MAX = 1536;
  std::unique_ptr<char[]> req(new (std::nothrow) char[REQ_MAX + 1]);
  if (!req) {  // memory short -> reject the connection instead of crashing
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
    if (len > 0) sendText(fd, 400, "Bad Request", "Invalid request");
  } else {
    const uint8_t *body = (const uint8_t *)headerEnd + 4;
    size_t bodyLen = len - (body - (const uint8_t *)req.get());
    headerEnd[2] = 0;  // terminate the header block so headerValue() does not search the body

    bool get = strncmp(req.get(), "GET ", 4) == 0;
    bool post = strncmp(req.get(), "POST ", 5) == 0;
    char path[160] = "";  // longest: /camdiag/send/<port>/<64 bytes as hex>; longer paths are not served
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
      } else sendText(fd, 503, "Service Unavailable", "No frame received yet");
    } else if (get && strcmp(path, "/style.css") == 0) {
      sendResponse(fd, 200, "OK", "text/css; charset=utf-8", STYLE_CSS, sizeof(STYLE_CSS) - 1);
    } else if (get && strcmp(path, "/app.js") == 0) {
      sendResponse(fd, 200, "OK", "application/javascript; charset=utf-8", APP_JS,
                   sizeof(APP_JS) - 1);
    } else if (get && strcmp(path, "/cameras") == 0) {
      sendResponse(fd, 200, "OK", "text/html; charset=utf-8", CAMERAS_HTML, sizeof(CAMERAS_HTML) - 1);
    } else if (get && strcmp(path, "/led") == 0) {
      // tiny status for the live page's one-second polling (the full /cameras.json costs
      // a 3 KB buffer per request, too much while a 720p stream needs the heap)
      char json[64];
      int n = snprintf(json, sizeof(json), "{\"led\":%d,\"level\":%d}", (int)telemetry.led, (int)ledLevel);
      sendResponse(fd, 200, "OK", "application/json", json, n);
    } else if (get && strcmp(path, "/cameras.json") == 0) {
      handleCamerasJson(fd);
    } else if (post && strcmp(path, "/cameras/scan") == 0) {
      cameraRequestScan();
      sendText(fd, 202, "Accepted", "Scan started");
    } else if (post && (strcmp(path, "/cameras/autoscan/0") == 0 || strcmp(path, "/cameras/autoscan/1") == 0)) {
      if (!authorized(req.get())) {
        sendText(fd, 401, "Unauthorized", "Wrong OTA password");
      } else {
        cameraSetAutoScan(path[18] == '1');
        sendText(fd, 200, "OK", path[18] == '1' ? "Automatic scan on" : "Automatic scan off");
      }
    } else if (post && strcmp(path, "/cameras/select") == 0) {
      if (!authorized(req.get())) sendText(fd, 401, "Unauthorized", "Wrong OTA password");
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
    } else if (post && strncmp(path, "/led/level/", 11) == 0) {
      // brightness in % (dimmable cameras), 0 = off
      int level = atoi(path + 11);
      if (!telemetry.ledDimmable) {
        sendText(fd, 501, "Not Implemented", "The LED of this camera cannot be dimmed");
      } else if (level < 0 || level > 100 || !isdigit((unsigned char)path[11])) {
        sendText(fd, 400, "Bad Request", "/led/level/<0..100>");
      } else {
        if (level) ledLevel = level;
        ledRequest = level ? 1 : 0;
        sendText(fd, 202, "Accepted", "sent");
      }
    } else if (post && strncmp(path, "/led/", 5) == 0 &&
               (path[5] == '0' || path[5] == '1') && path[6] == '\0') {
      if (!telemetry.ledSupported) {
        sendText(fd, 501, "Not Implemented", "This camera has no switchable LED");
      } else {
        ledRequest = path[5] == '1';  // the camera session sends it and waits for confirmation
        sendText(fd, 202, "Accepted", "sent");
      }
    } else if (post && strncmp(path, "/eth10/", 7) == 0) {
      if (!authorized(req.get())) sendText(fd, 401, "Unauthorized", "Wrong OTA password");
      else handleEth10Post(fd, path);
    } else if (get && strcmp(path, "/wifi-setup") == 0) {
      sendResponse(fd, 200, "OK", "text/html; charset=utf-8", WIFI_SETUP_HTML, sizeof(WIFI_SETUP_HTML) - 1);
    } else if (post && strcmp(path, "/wifi-setup") == 0) {
      if (!authorized(req.get())) sendText(fd, 401, "Unauthorized", "Wrong OTA password");
      else handleWifiSetup(fd, req.get(), body, bodyLen);
    } else if (post && strncmp(path, "/wifi/", 6) == 0 && authorized(req.get())) {
      handleWifiModePost(fd, path);
    } else if (post && strcmp(path, "/calibration") == 0) {
      handleCalibrationPost(fd, req.get(), body, bodyLen);
    } else if (get && strcmp(path, "/orientation") == 0) {
      handleOrientation(fd);
    } else if (get && strcmp(path, "/status") == 0) {
      handleStatus(fd);
    } else if (post && strncmp(path, "/sniff/start", 12) == 0 && (!path[12] || path[12] == '/')) {
      // /sniff/start or /sniff/start/<channel>
      if (!authorized(req.get())) {
        sendText(fd, 401, "Unauthorized", "Wrong OTA password");
      } else if (rescueMode) {
        sendText(fd, 409, "Conflict", "Not in rescue mode");
      } else if (sniffStart(path[12] ? atoi(path + 13) : 0, (uint32_t)WiFi.gatewayIP(),
                            path[12] && strchr(path + 13, '/') ? strchr(path + 13, '/')[1] : 0)) {
        sendText(fd, 200, "OK", "Sniffer running: now connect the vendor app to the camera. Read GET /sniff\n");
      } else {
        sendText(fd, 400, "Bad Request", "Channel unknown (not connected to a camera): use /sniff/start/<channel>\n");
      }
    } else if (post && strcmp(path, "/sniff/stop") == 0) {
      if (!authorized(req.get())) {
        sendText(fd, 401, "Unauthorized", "Wrong OTA password");
      } else {
        sniffStop();
        sendText(fd, 200, "OK", "Sniffer stopped, reconnecting to the camera\n");
      }
    } else if (get && strcmp(path, "/sniff") == 0) {
      static const char hdr[] =
          "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
          "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
      BlockSender out(fd);
      out.put(hdr, sizeof(hdr) - 1);
      sniffText([](void *o, const char *line) { ((BlockSender *)o)->put(line, strlen(line)); }, &out);
      out.flush();
    } else if (post && strncmp(path, "/camdiag/send/", 14) == 0) {
      // /camdiag/send/<port>/<hex bytes>: experiment, sent by the camera session
      const char *p = path + 14;
      char *end;
      long port = strtol(p, &end, 10);
      uint8_t data[64];
      size_t n = 0;
      bool ok = port > 0 && port < 65536 && *end == '/';
      for (const char *h = end + 1; ok && *h && n < sizeof(data); h += 2) {
        if (!isxdigit((unsigned char)h[0]) || !isxdigit((unsigned char)h[1])) ok = false;
        else data[n++] = (uint8_t)strtol(String(h).substring(0, 2).c_str(), nullptr, 16);
      }
      if (!authorized(req.get())) sendText(fd, 401, "Unauthorized", "Wrong OTA password");
      else if (!ok || !n) sendText(fd, 400, "Bad Request", "/camdiag/send/<port>/<hex>, max. 64 bytes\n");
      else {
        diagSendPut((uint16_t)port, data, n);
        sendText(fd, 202, "Accepted", "queued, see /camdiag\n");
      }
    } else if (get && strcmp(path, "/camdiag/raw") == 0) {
      // first call requests the capture of the next frame, the next one fetches it
      Frame raw;
      if (diagRawTake(raw)) {
        char hdr[200];
        int h = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %u\r\n"
                         "Content-Disposition: attachment; filename=raw-frame.bin\r\n"
                         "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
                         (unsigned)raw.size());
        BlockSender out(fd);
        out.put(hdr, h);
        putFrame(out, raw);
        out.flush();
      } else {
        diagRawRequest();
        sendText(fd, 202, "Accepted", "Capture requested, fetch again in a moment\n");
      }
    } else if (get && strcmp(path, "/camdiag") == 0) {
      std::unique_ptr<char[]> text(new (std::nothrow) char[1024]);
      if (!text) {
        sendText(fd, 503, "Service Unavailable", "no memory");
      } else {
        diagCopy(text.get(), 1024);
        sendText(fd, 200, "OK", *text.get() ? text.get() : "no session yet\n");
      }
    } else if (post && strcmp(path, "/update") == 0) {
      handleUpdate(fd, req.get(), body, bodyLen);
    } else if (post && strcmp(path, "/restart") == 0) {
      if (!authorized(req.get())) {
        sendText(fd, 401, "Unauthorized", "Wrong OTA password");
      } else {
        sendText(fd, 200, "OK", "Device is restarting");
        delay(500);
        ESP.restart();
      }
    } else if (!get && !post) {
      sendText(fd, 405, "Method Not Allowed", "Only GET and POST");
    } else if (get && rescueApActive()) {
      // Captive portal: phones probe an internet address when connecting and open
      // the setup page by themselves when redirected
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
  addr.sin_addr.s_addr = htonl(INADDR_ANY);  // Ethernet and (in rescue mode) home Wi-Fi
  addr.sin_port = htons(HTTP_PORT);
  bind(srv, (sockaddr *)&addr, sizeof(addr));
  listen(srv, 4);

  for (;;) {
    int fd = accept(srv, nullptr, nullptr);
    if (fd < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    // streams, orientation events and a few slots for pages, status and update
    bool started = false;
    if (clientTasks < 2 * MAX_STREAM_CLIENTS + 3) {
      clientTasks++;  // count beforehand: the task may already be done before create returns
      // During radio dropouts the Wi-Fi driver briefly holds a lot of heap -> wait a
      // little and retry instead of rejecting the connection right away
      for (int attempt = 0; attempt < 5 && !started; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(50));
        started = xTaskCreatePinnedToCore(clientTask, "http-client", 6144, (void *)(intptr_t)fd,
                                          3, nullptr, 1) == pdPASS;
      }
      if (!started) clientTasks--;
    }
    if (!started) {
      crumb("busy: %d tasks, %d streams, %d orientation, heap %u/%u", (int)clientTasks,
            (int)streamClients, (int)sseClients, heapFree(), heapBlock());
      sendText(fd, 503, "Service Unavailable", "Busy");
      close(fd);
    }
  }
}

// --- Ethernet: transmit only with a complete packet in the FIFO --------------------
// ESP-IDF starts transmitting as soon as 64 bytes are in the TX FIFO. If the Wi-Fi DMA
// blocks the memory bus meanwhile, the FIFO runs empty and the packet goes out mangled;
// the switch drops it, TCP retransmits only after ~1 s (measured: only with Wi-Fi RX).
// "Store and forward" transmits only once the whole packet is in the FIFO (2 KB = 1 packet).
static void ethStoreForward() {
  if (!ETH_TX_STORE_FORWARD || EMAC_DMA.dmaoperation_mode.tx_str_fwd) return;
  EMAC_DMA.dmaoperation_mode.start_stop_transmission_command = 0;  // stop transmitting
  delay(2);
  EMAC_DMA.dmaoperation_mode.tx_str_fwd = 1;
  EMAC_DMA.dmaoperation_mode.start_stop_transmission_command = 1;
  crumb("eth: store and forward on");
}

// --- Ethernet at 10 Mbit ------------------------------------------------------------
// The ESP32 generates the 50 MHz clock for the LAN8720 itself (GPIO17). Wi-Fi reception
// disturbs it; at 100 Mbit ~2-3 % of the Ethernet packets are then lost (measured,
// independent of Wi-Fi transmit power, buffers and Zigbee). At 10 Mbit every bit is
// held for 10 clock cycles and is immune. Implemented via auto-negotiation (only offer
// "10 Mbit full duplex") so the switch does not fall back to half duplex.

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
  // bits 5-8: 10HD, 10FD, 100HD, 100FD; pause bits (10/11) and selector stay
  uint32_t want = (anar & ~0x01E0u) | (eth10 ? 0x0040u : 0x01E0u);
  if (want == anar) return;  // already negotiated like this -> no endless loop
  phyWrite(ANAR, want);
  phyWrite(BMCR, bmcr | 0x1000 | 0x0200);  // enable + restart auto-negotiation
  crumb("eth: offering %s, renegotiating", eth10 ? "10 Mbit only" : "100 Mbit");
}

// --- Network --------------------------------------------------------------------
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
      Serial.printf("[eth] IP %s\r\n", ETH.localIP().toString().c_str());
      crumb("eth IP %s, %d Mbit %s", ETH.localIP().toString().c_str(), (int)ETH.linkSpeed(),
            ETH.fullDuplex() ? "full duplex" : "HALF DUPLEX");
      ethUp = true;
      ETH.setDefault();  // default route into the home network, not to the camera
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("[eth] disconnected");
      crumb("eth disconnected (event %d)", (int)event);
      ethUp = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      cameraOnWifiGotIp();
      crumb("wifi connected %s, RSSI %d", WiFi.SSID().c_str(), WiFi.RSSI());
      Serial.printf("[wifi] connected to %s, IP %s, RSSI %d dBm\r\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (ethUp) ETH.setDefault();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[wifi] disconnected (reason %u)\r\n", info.wifi_sta_disconnected.reason);
      crumb("wifi disconnected, reason %u", info.wifi_sta_disconnected.reason);
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
  ArduinoOTA.setHostname(HOSTNAME);  // also starts mDNS -> otoskop.local
  if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    updating = true;
    Serial.println("[ota] update starting");
  });
  ArduinoOTA.onError([](ota_error_t) { updating = false; });
  ArduinoOTA.begin();
  IPAddress ip = ethUp ? ETH.localIP() : WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP();
  Serial.printf("[http] viewer: http://%s.local/  or http://%s/\r\n", HOSTNAME,
                ip.toString().c_str());
}

void setup() {
  if (LED_GREEN_GPIO >= 0) pinMode(LED_GREEN_GPIO, OUTPUT);
  if (LED_RED_GPIO >= 0) pinMode(LED_RED_GPIO, OUTPUT);
  setZigbee(false);  // Zigbee is not needed: silence it
  setLed(LED_GREEN_GPIO, true);
  setLed(LED_RED_GPIO, false);

  Serial.begin(115200);
  if (bootMagic != 0xB007B007 || esp_reset_reason() == ESP_RST_POWERON) {
    bootMagic = 0xB007B007;
    bootCount = 0;
  }
  bootCount++;
  crashlogInit();
  Serial.printf("\r\n[boot] WiFi-Cam-Proxy %s (%s), Reset: %s, Boot #%u\r\n", FW_VERSION, FW_COMMIT,
                resetReasonText(), (unsigned)bootCount);

  loadCalibration();
  loadWifiMode();
  rescueBegin();
  Network.onEvent(onNetworkEvent);
  ethBeginOk = ETH.begin(ETH_PHY_LAN8720, ETH_PHY_ADDR_GW, ETH_MDC_GPIO, ETH_MDIO_GPIO, ETH_POWER_GPIO,
            ETH_CLK_MODE_GW);

  Serial.printf("[eth] begin %s\r\n", ethBeginOk ? "ok" : "FAILED");

  cameraBegin();  // Wi-Fi to the camera and video task
  xTaskCreatePinnedToCore(httpTask, "http", 4096, nullptr, 3, nullptr, 1);
}

void loop() {
  static bool otaStarted = false;
  static uint32_t lastStats = 0, lastFrames = 0;
  static uint32_t ethDownSince = 0, ethUpSince = 0;

  // Rescue mode: enter when Ethernet is gone too long; leave via restart once it is
  // stable again
  if (ethUp) {
    ethDownSince = millis();
    if (rescueMode && !updating && millis() - ethUpSince > 10000) {
      Serial.println("[rescue] Ethernet is back -> restarting into normal operation");
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
    Serial.printf("[stats] %.1f fps, %u frames, %u dropped, %d viewers, heap %u%s\r\n",
                  currentFps, (unsigned)total, (unsigned)stats.framesDropped, (int)streamClients,
                  heapFree(), rescueMode ? ", RESCUE MODE" : "");
  }
  delay(10);
}
