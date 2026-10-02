/*
 * HTTP server of the Ethernet bridge (port 80, Ethernet and in rescue mode Wi-Fi):
 * web UI, MJPEG stream, snapshot, JSON status, settings, firmware update. One task per
 * connection; the routes are in ROUTES at the end of this file.
 */

#include <Arduino.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_memory_utils.h>
#include <lwip/sockets.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <new>

#include "camera.h"
#include "config.h"
#include "crashlog.h"
#include "device.h"
#include "settings.h"
#include "rescue.h"
#include "sniffer.h"
#include "web_ui.h"  // start page, calibration, update page, shared JS

static std::atomic<int> clientTasks{0};
static std::atomic<int> streamClients{0};
static std::atomic<uint32_t> streamGen{0};  // number of the newest stream (see handleStream)
static std::atomic<int> sseClients{0};

int streamViewers() { return streamClients; }

// One HTTP request: header block (request line and headers, 0-terminated) and the part
// of the body that arrived with it
struct Request {
  int fd;
  const char *head;
  const char *path;     // without query string
  const uint8_t *body;
  size_t bodyLen;
};

// --- Sending ----------------------------------------------------------------------
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

// A page or file compiled into the firmware (web_ui.h)
template <size_t N>
static void sendStatic(int fd, const char *ctype, const char (&text)[N]) {
  sendResponse(fd, 200, "OK", ctype, text, N - 1);
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
  // Frame chunk, possibly in IRAM (word access only): copyFromChunk reads it straight
  // into the send buffer
  void putChunk(const uint8_t *src, size_t n) {
    if (!buf) {  // no send buffer: in small pieces
      uint8_t tmp[64];
      for (size_t done = 0; done < n && ok; done += sizeof(tmp)) {
        size_t k = min(n - done, sizeof(tmp));
        copyFromChunk(tmp, src, done, k);
        put(tmp, k);
      }
      return;
    }
    for (size_t done = 0; done < n && ok;) {
      size_t k = min(n - done, SEND_BLOCK - len);
      copyFromChunk(buf + len, src, done, k);
      len += k;
      done += k;
      if (len == SEND_BLOCK) flush();
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

// Read the complete body (max. max bytes) into buf and terminate it; false = response
// already sent
static bool readBody(Request &r, char *buf, size_t max) {
  size_t total = headerValue(r.head, "Content-Length").toInt();
  if (total == 0 || total > max) {
    sendText(r.fd, 413, "Payload Too Large", "Invalid length");
    return false;
  }
  size_t have = min(r.bodyLen, total);
  memcpy(buf, r.body, have);
  while (have < total) {
    int n = recv(r.fd, buf + have, total - have, 0);
    if (n <= 0) {
      sendText(r.fd, 400, "Bad Request", "Connection aborted");
      return false;
    }
    have += n;
  }
  buf[total] = 0;
  return true;
}

// --- Video ------------------------------------------------------------------------
static void handleStream(Request &r) {
  int fd = r.fd;
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
    uint32_t lastSent = 0;   // for STREAM_MAX_FPS
    size_t lastSize = 0;     // size of the last frame in the store, for replacedByNewer()
    BlockSender out(fd);  // one buffer per viewer, for all frames
    Frame frame;
    // Large frames (720p microscopes): only one viewer, the newest wins. Each viewer
    // holds the frame it is sending; with several 50-80 KB frames the heap runs out,
    // reception drops frames and the Wi-Fi stalls. Also ends stale connections of a tab
    // that reconnected, while waiting as well (they would keep their task and buffer).
    auto replacedByNewer = [&]() {
      if (lastSize <= FRAME_RESERVE_FROM || me == streamGen) return false;
      crumb("stream ended: large frames, a newer viewer took over");
      return true;
    };
    while (!updating) {
      // Look at the sequence number first: holding the frame only to compare it would
      // keep the store from freeing it for the next one (releaseIdleFrame)
      uint32_t s = latestFrameSeq();
      if (s != seq && STREAM_MAX_FPS > 0) {
        // At most STREAM_MAX_FPS: wait out the rest of the interval, then send the
        // newest frame (frames in between are skipped, the stream does not fall behind)
        uint32_t since = millis() - lastSent, interval = 1000 / (STREAM_MAX_FPS > 0 ? STREAM_MAX_FPS : 1);
        if (since < interval) {
          if (replacedByNewer()) break;
          vTaskDelay(pdMS_TO_TICKS(interval - since));
          continue;
        }
      }
      if (s != seq) s = getFrame(frame);
      if (s == seq || !frame) {
        frame.reset();
        if (replacedByNewer()) break;
        // While waiting, check now and then whether the client is still there
        if (millis() - lastCheck > 1000) {
          if (clientClosed(fd)) break;
          lastCheck = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      lastSent = millis();  // start to start, so the sending time is not added on top
      seq = s;
      lastSize = frame.size();
      if (clientClosed(fd) || replacedByNewer()) {
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
  crumb("stream closed (%d viewers)", (int)streamClients);
}

static void handleSnapshot(Request &r) {
  Frame frame;
  getFrame(frame);
  if (!frame) return sendText(r.fd, 503, "Service Unavailable", "No frame received yet");
  char hdr[160];
  int h = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n"
                   "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
                   (unsigned)frame.size());
  BlockSender out(r.fd);
  out.put(hdr, h);
  putFrame(out, frame);
  out.flush();
}

// Orientation as server-sent events: one open connection, new values ~17x per second
static void handleOrientation(Request &r) {
  static const char hdr[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
      "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
  sseClients++;
  uint32_t lastSeq = 0, lastSend = 0;
  bool ok = sendAll(r.fd, hdr, sizeof(hdr) - 1);
  while (ok && !updating) {
    uint32_t seq = telemetry.accSeq;
    // new values max. 25x/s (the otoscope delivers ~17); otherwise a keepalive every 5 s so dead clients are noticed
    if ((seq != lastSeq && millis() - lastSend >= 40) || millis() - lastSend >= 5000) {
      char msg[64];
      int n = seq != lastSeq
                  ? snprintf(msg, sizeof(msg), "data: {\"x\":%d,\"y\":%d,\"z\":%d}\n\n",
                             (int)telemetry.accX, (int)telemetry.accY, (int)telemetry.accZ)
                  : snprintf(msg, sizeof(msg), ": ping\n\n");
      if (!sendAll(r.fd, msg, n)) break;
      lastSeq = seq;
      lastSend = millis();
    }
    if (clientClosed(r.fd)) break;  // browser left the page -> slot free right away
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  sseClients--;
}

// --- Status -----------------------------------------------------------------------
// Replace what would break a JSON string ("…" without escapes): the values in /status are
// for reading only, unlike /cameras.json, whose SSIDs are sent back exactly (jsonStr)
static void jsonSafe(char *s) {
  for (; *s; s++)
    if (*s == '"' || *s == '\\' || (uint8_t)*s < 0x20) *s = '?';
}

static void handleStatus(Request &r) {
  static const size_t JSON_MAX = 2048;
  std::unique_ptr<char[]> json(new (std::nothrow) char[JSON_MAX]);
  if (!json) return sendText(r.fd, 503, "Service Unavailable", "Out of memory");
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  char crash[320];
  crashlogFormat(crash, sizeof(crash));
  jsonSafe(crash);
  // Times of the last clean stalls, oldest first
  char times[96] = "";
  uint32_t nClean = stats.stallsClean;
  uint32_t shown = min(nClean, (uint32_t)VideoStats::CLEAN_STALL_TIMES);
  for (uint32_t k = 0, t = 0; k < shown; k++)
    t += snprintf(times + t, sizeof(times) - t, "%s%lu", k ? "," : "",
                  (unsigned long)stats.cleanStallAt[(nClean - shown + k) % VideoStats::CLEAN_STALL_TIMES]);
  char home[33], ap[33] = "", ssid[33] = "";
  rescueHomeSsid(home, sizeof(home));
  jsonSafe(home);
  if (rescueApActive()) rescueApSsid(ap, sizeof(ap));
  if (wifiOk) strlcpy(ssid, WiFi.SSID().c_str(), sizeof(ssid));
  jsonSafe(ssid);
  EthStatus eth;
  ethStatus(eth);
  int n = snprintf(json.get(), JSON_MAX,
                   "{\"version\":\"%s\",\"commit\":\"%s\",\"reset_reason\":\"%s\",\"boot_count\":%u,\"mode\":\"%s\",\"fps\":%.1f,\"frames\":%u,"
                   "\"dropped\":%u,\"drop_nomem\":%u,\"drop_toobig\":%u,\"drop_incomplete\":%u,\"packets_lost\":%u,\"damaged\":%u,\"released\":%u,\"max_frame\":%u,\"handshakes\":%u,\"keepalives\":%u,\"stalls_loss\":%u,\"stalls_clean\":%u,\"clean_stall_times\":[%s],\"stream_clients\":%d,\"sse_clients\":%d,\"client_tasks\":%d,"
                   "\"wifi_connected\":%s,\"wifi_ssid\":\"%s\",\"wifi_rssi\":%d,\"wifi_mode\":\"%s\",\"wifi_tx_dbm\":%.2f,\"wifi_ip\":\"%s\",\"home_ssid\":\"%s\",\"ap\":\"%s\",\"eth10\":%d,"
                   "\"cam_proto\":\"%s\",\"battery\":%d,\"led\":%d,"
                   "\"eth_begin\":%s,\"eth_started\":%s,\"eth_link\":%s,\"eth_speed\":%d,\"eth_full_duplex\":%s,\"eth_tx_store_forward\":%d,\"eth_ip\":\"%s\",\"free_heap\":%u,\"max_alloc\":%u,\"min_heap\":%u,\"iram_heap\":%u,\"psram\":%u,\"uptime_s\":%lu,\"last_crash\":\"%s\"}",
                   FW_VERSION, FW_COMMIT, resetReasonText(), (unsigned)bootCount(), rescueMode ? "rescue" : "normal", currentFps(),
                   (unsigned)stats.framesTotal, (unsigned)stats.framesDropped, (unsigned)stats.dropNoMem,
                   (unsigned)stats.dropTooBig, (unsigned)stats.dropIncomplete, (unsigned)stats.packetsLost, (unsigned)stats.framesDamaged,
                   (unsigned)stats.framesReleased,
                   (unsigned)stats.maxFrameBytes, (unsigned)stats.handshakes, (unsigned)stats.keepalives, (unsigned)stats.stallsLoss, (unsigned)stats.stallsClean, times,
                   (int)streamClients, (int)sseClients, (int)clientTasks, wifiOk ? "true" : "false",
                   ssid, wifiOk ? WiFi.RSSI() : 0, wifiModeName(), wifiTxDbm(),
                   wifiOk ? WiFi.localIP().toString().c_str() : "", home, ap, (int)eth10Mbit(),
                   protoKey(cameraProto()), (int)telemetry.battery, (int)telemetry.led,
                   eth.beginOk ? "true" : "false", eth.started ? "true" : "false", eth.link ? "true" : "false",
                   eth.speed, eth.fullDuplex ? "true" : "false", eth.storeForward, eth.ip,
                   heapFree(), heapBlock(), heapMin(), iramFree(), (unsigned)ESP.getPsramSize(), millis() / 1000, crash);
  n = constrain(n, 0, (int)JSON_MAX - 1);  // never send more than the buffer holds
  sendResponse(r.fd, 200, "OK", "application/json", json.get(), n);
}

// --- Firmware update --------------------------------------------------------------
// Firmware as raw data in the body (application/octet-stream), e.g. from the
// update page or via: curl --data-binary @firmware.bin http://otoskop.local/update
static void handleUpdate(Request &r) {
  int fd = r.fd;
  size_t total = headerValue(r.head, "Content-Length").toInt();
  if (total == 0) return sendText(fd, 411, "Length Required", "Content-Length missing");
  if (updating.exchange(true)) return sendText(fd, 409, "Conflict", "Update already running");

  Serial.printf("[update] web update, %u bytes\r\n", (unsigned)total);
  const char *err = nullptr;
  if (!Update.begin(total, U_FLASH)) {
    err = Update.errorString();
  } else {
    size_t done = 0;
    if (r.bodyLen > 0) {
      if (Update.write((uint8_t *)r.body, r.bodyLen) != r.bodyLen) err = Update.errorString();
      done = r.bodyLen;
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

static void handleRestart(Request &r) {
  sendText(r.fd, 200, "OK", "Device is restarting");
  delay(500);
  ESP.restart();
}

// --- Settings ---------------------------------------------------------------------
// POST /wifi/<bgn|bg|b> (mode towards the camera) or /wifi/tx/<8..84> (transmit power)
static void handleWifiPost(Request &r) {
  if (strncmp(r.path, "/wifi/tx/", 9) == 0) {  // transmit power, takes effect immediately
    if (!wifiSetTxPower(atoi(r.path + 9)))
      return sendText(r.fd, 400, "Bad Request", "Transmit power 8..84 (x 0.25 dBm)");
    return sendText(r.fd, 200, "OK", "Transmit power set");
  }
  if (!wifiSetMode(r.path + 6)) return sendText(r.fd, 400, "Bad Request", "Mode: bgn, bg or b");
  sendText(r.fd, 200, "OK", "Wi-Fi mode set, reconnecting");
}

// Ethernet 10 Mbit only (1) or 100 Mbit (0): POST /eth10/<0|1>, stored in NVS
static void handleEth10Post(Request &r) {
  const char *v = r.path + 7;
  if ((v[0] != '0' && v[0] != '1') || v[1]) return sendText(r.fd, 400, "Bad Request", "/eth10/<0|1>");
  sendText(r.fd, 200, "OK", "set, Ethernet renegotiates (link briefly down)");
  delay(200);  // the answer goes out before the link drops
  ethSet10Mbit(v[0] == '1');
}

// --- Orientation calibration (JSON, computed by the web pages, stored in NVS) -----
static const size_t CALIB_MAX = 1024;
static std::mutex calibMutex;
static String calibJson = "{}";

static void handleCalibrationGet(Request &r) {
  String c;
  {
    std::lock_guard<std::mutex> lock(calibMutex);
    c = calibJson;
  }
  sendResponse(r.fd, 200, "OK", "application/json", c.c_str(), c.length());
}

static void handleCalibrationPost(Request &r) {
  char buf[CALIB_MAX + 1];
  if (!readBody(r, buf, CALIB_MAX)) return;
  size_t len = strlen(buf);
  if (buf[0] != '{' || !len || buf[len - 1] != '}') return sendText(r.fd, 400, "Bad Request", "Not a JSON object");
  bool ok = false;
  nvsWrite([&](Preferences &p) { ok = p.putString("calib", buf) > 0; });
  if (!ok) return sendText(r.fd, 500, "Internal Server Error", "Saving to NVS failed");
  {
    std::lock_guard<std::mutex> lock(calibMutex);
    calibJson = buf;
  }
  crumb("calibration stored (%u bytes)", (unsigned)len);
  sendText(r.fd, 200, "OK", "Saved");
}

// --- Camera selection and home Wi-Fi (forms) ---------------------------------------
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

static const size_t FORM_MAX = 256;

static void handleWifiSetup(Request &r) {
  char buf[FORM_MAX + 1], ssid[34], pass[66];
  if (!readBody(r, buf, FORM_MAX)) return;
  formValue(buf, "ssid", ssid, sizeof(ssid));
  formValue(buf, "pass", pass, sizeof(pass));
  if (!rescueSetHome(ssid, pass))
    return sendText(r.fd, 400, "Bad Request", "SSID max. 32 characters, password empty or 8-64 characters");
  sendText(r.fd, 200, "OK", !*ssid ? "Home Wi-Fi deleted" : rescueMode ? "Saved, connecting…" : "Saved (used in rescue mode)");
}

static void handleCameraSelect(Request &r) {
  char buf[FORM_MAX + 1];
  if (!readBody(r, buf, FORM_MAX)) return;
  char ssid[34], pass[66], proto[12];
  formValue(buf, "ssid", ssid, sizeof(ssid));
  formValue(buf, "pass", pass, sizeof(pass));
  formValue(buf, "proto", proto, sizeof(proto));
  if (!cameraSelect(ssid, pass, protoFromKey(proto)))
    return sendText(r.fd, 400, "Bad Request", "Invalid SSID (max. 32), password (max. 64) or protocol");
  sendText(r.fd, 202, "Accepted", *ssid ? "Connecting…" : "Selection cleared, choosing automatically");
}

static void handleCamerasJson(Request &r) {
  std::unique_ptr<char[]> json(new (std::nothrow) char[3072]);
  if (!json) return sendText(r.fd, 503, "Service Unavailable", "Out of memory");
  size_t n = cameraJson(json.get(), 3072);
  sendResponse(r.fd, 200, "OK", "application/json", json.get(), n);
}

static void handleAutoScan(Request &r) {
  const char *v = r.path + 18;
  if ((v[0] != '0' && v[0] != '1') || v[1]) return sendText(r.fd, 400, "Bad Request", "/cameras/autoscan/<0|1>");
  cameraSetAutoScan(v[0] == '1');
  sendText(r.fd, 200, "OK", v[0] == '1' ? "Automatic scan on" : "Automatic scan off");
}

// --- LED --------------------------------------------------------------------------
// Tiny status for the live page's one-second polling (the full /cameras.json costs a
// 3 KB buffer per request, too much while a 720p stream needs the heap)
static void handleLedGet(Request &r) {
  char json[64];
  int n = snprintf(json, sizeof(json), "{\"led\":%d,\"level\":%d}", (int)telemetry.led, (int)ledLevel);
  sendResponse(r.fd, 200, "OK", "application/json", json, n);
}

// POST /led/level/<0..100>: brightness in % (dimmable cameras), 0 = off
static void handleLedLevel(Request &r) {
  const char *v = r.path + 11;
  int level = atoi(v);
  if (!telemetry.ledDimmable) return sendText(r.fd, 501, "Not Implemented", "The LED of this camera cannot be dimmed");
  if (level < 0 || level > 100 || !isdigit((unsigned char)*v)) return sendText(r.fd, 400, "Bad Request", "/led/level/<0..100>");
  if (level) ledLevel = level;
  ledRequest = level ? 1 : 0;
  sendText(r.fd, 202, "Accepted", "sent");
}

// POST /led/<0|1>
static void handleLedSwitch(Request &r) {
  const char *v = r.path + 5;
  if ((v[0] != '0' && v[0] != '1') || v[1]) return sendText(r.fd, 404, "Not Found", "Not found");
  if (!telemetry.ledSupported) return sendText(r.fd, 501, "Not Implemented", "This camera has no switchable LED");
  ledRequest = v[0] == '1';  // the camera session sends it and waits for confirmation
  sendText(r.fd, 202, "Accepted", "sent");
}

// --- Diagnostics ------------------------------------------------------------------
// POST /sniff/start[/<channel>[/<a|b|n>]]
static void handleSniffStart(Request &r) {
  const char *rest = r.path + 12;
  if (*rest && *rest != '/') return sendText(r.fd, 404, "Not Found", "Not found");
  if (rescueMode) return sendText(r.fd, 409, "Conflict", "Not in rescue mode");
  int channel = *rest ? atoi(rest + 1) : 0;
  const char *sec = *rest ? strchr(rest + 1, '/') : nullptr;
  int res = sniffRequest(true, channel, (uint32_t)WiFi.gatewayIP(), sec ? sec[1] : 0);
  if (res > 0)
    sendText(r.fd, 200, "OK", "Sniffer running: now connect the vendor app to the camera. Read GET /sniff\n");
  else if (res == 0)
    sendText(r.fd, 400, "Bad Request", "Channel unknown (not connected to a camera): use /sniff/start/<channel>\n");
  else
    sendText(r.fd, 503, "Service Unavailable", "Busy, try again\n");
}

static void handleSniffStop(Request &r) {
  if (sniffRequest(false) < 0) return sendText(r.fd, 503, "Service Unavailable", "Busy, try again\n");
  sendText(r.fd, 200, "OK", "Sniffer stopped, reconnecting to the camera\n");
}

static void handleSniffGet(Request &r) {
  static const char hdr[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
      "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
  BlockSender out(r.fd);
  out.put(hdr, sizeof(hdr) - 1);
  sniffText([](void *o, const char *line) { ((BlockSender *)o)->put(line, strlen(line)); }, &out);
  out.flush();
}

static int hexNibble(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

// POST /camdiag/send/<port>/<hex bytes>: experiment, sent by the camera session
static void handleCamdiagSend(Request &r) {
  char *end;
  long port = strtol(r.path + 14, &end, 10);
  uint8_t data[64];
  size_t n = 0;
  bool ok = port > 0 && port < 65536 && *end == '/';
  for (const char *h = end + 1; ok && *h && n < sizeof(data); h += 2) {
    if (!isxdigit((unsigned char)h[0]) || !isxdigit((unsigned char)h[1])) ok = false;
    else data[n++] = hexNibble(h[0]) << 4 | hexNibble(h[1]);
  }
  if (!ok || !n) return sendText(r.fd, 400, "Bad Request", "/camdiag/send/<port>/<hex>, max. 64 bytes\n");
  diagSendPut((uint16_t)port, data, n);
  sendText(r.fd, 202, "Accepted", "queued, see /camdiag\n");
}

// First call requests the capture of the next frame, the next one fetches it
static void handleCamdiagRaw(Request &r) {
  Frame raw;
  if (!diagRawTake(raw)) {
    diagRawRequest();
    return sendText(r.fd, 202, "Accepted", "Capture requested, fetch again in a moment\n");
  }
  char hdr[200];
  int h = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %u\r\n"
                   "Content-Disposition: attachment; filename=raw-frame.bin\r\n"
                   "Cache-Control: no-cache\r\nConnection: close\r\n\r\n",
                   (unsigned)raw.size());
  BlockSender out(r.fd);
  out.put(hdr, h);
  putFrame(out, raw);
  out.flush();
}

static void handleCamdiag(Request &r) {
  std::unique_ptr<char[]> text(new (std::nothrow) char[1024]);
  if (!text) return sendText(r.fd, 503, "Service Unavailable", "no memory");
  diagCopy(text.get(), 1024);
  sendText(r.fd, 200, "OK", *text.get() ? text.get() : "no session yet\n");
}

// --- Routes -----------------------------------------------------------------------
// auth: needs the OTA password (header X-OTA-Password) if one is set. Protected is the
// update and everything that changes configuration or connection; viewing and
// operating (LED, scan, calibration: its page has no password field) stay open.
// Prefix routes check the rest of the path themselves. First match wins.
enum Method : uint8_t { GET, POST };
enum Match : uint8_t { EXACT, PREFIX };
struct Route {
  Method method;
  const char *path;
  Match match;
  bool auth;
  void (*handler)(Request &r);
};

static const char HTML[] = "text/html; charset=utf-8";

static const Route ROUTES[] = {
    // pages and shared files
    {GET, "/", EXACT, false, [](Request &r) { sendStatic(r.fd, HTML, INDEX_HTML); }},
    {GET, "/cameras", EXACT, false, [](Request &r) { sendStatic(r.fd, HTML, CAMERAS_HTML); }},
    {GET, "/calibrate", EXACT, false, [](Request &r) { sendStatic(r.fd, HTML, CALIBRATE_HTML); }},
    {GET, "/update", EXACT, false, [](Request &r) { sendStatic(r.fd, HTML, UPDATE_HTML); }},
    {GET, "/wifi-setup", EXACT, false, [](Request &r) { sendStatic(r.fd, HTML, WIFI_SETUP_HTML); }},
    {GET, "/style.css", EXACT, false, [](Request &r) { sendStatic(r.fd, "text/css; charset=utf-8", STYLE_CSS); }},
    {GET, "/app.js", EXACT, false, [](Request &r) { sendStatic(r.fd, "application/javascript; charset=utf-8", APP_JS); }},
    // video and state
    {GET, "/stream", EXACT, false, handleStream},
    {GET, "/snapshot", EXACT, false, handleSnapshot},
    {GET, "/orientation", EXACT, false, handleOrientation},
    {GET, "/status", EXACT, false, handleStatus},
    {GET, "/cameras.json", EXACT, false, handleCamerasJson},
    {GET, "/led", EXACT, false, handleLedGet},
    {GET, "/calibration", EXACT, false, handleCalibrationGet},
    // operating
    {POST, "/led/level/", PREFIX, false, handleLedLevel},
    {POST, "/led/", PREFIX, false, handleLedSwitch},
    {POST, "/cameras/scan", EXACT, false, [](Request &r) {
       cameraRequestScan();
       sendText(r.fd, 202, "Accepted", "Scan started");
     }},
    {POST, "/calibration", EXACT, false, handleCalibrationPost},
    // configuration (password)
    {POST, "/cameras/select", EXACT, true, handleCameraSelect},
    {POST, "/cameras/autoscan/", PREFIX, true, handleAutoScan},
    {POST, "/wifi-setup", EXACT, true, handleWifiSetup},
    {POST, "/wifi/", PREFIX, true, handleWifiPost},
    {POST, "/eth10/", PREFIX, true, handleEth10Post},
    {POST, "/update", EXACT, true, handleUpdate},
    {POST, "/restart", EXACT, true, handleRestart},
    // diagnostics
    {GET, "/camdiag", EXACT, false, handleCamdiag},
    {GET, "/camdiag/raw", EXACT, false, handleCamdiagRaw},
    {POST, "/camdiag/send/", PREFIX, true, handleCamdiagSend},
    {GET, "/sniff", EXACT, false, handleSniffGet},
    {POST, "/sniff/start", PREFIX, true, handleSniffStart},
    {POST, "/sniff/stop", EXACT, true, handleSniffStop},
};

static void route(Request &r, Method method) {
  for (const Route &rt : ROUTES) {
    if (rt.method != method) continue;
    if (rt.match == EXACT ? strcmp(r.path, rt.path) != 0 : strncmp(r.path, rt.path, strlen(rt.path)) != 0) continue;
    if (rt.auth && !authorized(r.head)) return sendText(r.fd, 401, "Unauthorized", "Wrong OTA password");
    return rt.handler(r);
  }
  if (method == GET && rescueApActive()) {
    // Captive portal: phones probe an internet address when connecting and open
    // the setup page by themselves when redirected
    static const char redirect[] =
        "HTTP/1.1 302 Found\r\nLocation: http://192.168.4.1/wifi-setup\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n";
    sendAll(r.fd, redirect, sizeof(redirect) - 1);
    return;
  }
  sendText(r.fd, 404, "Not Found", "Not found");
}

// --- Server -----------------------------------------------------------------------
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
  size_t len = 0;
  char *headerEnd = nullptr;
  while (req && len < REQ_MAX) {  // memory short -> close the connection instead of crashing
    int n = recv(fd, req.get() + len, REQ_MAX - len, 0);
    if (n <= 0) break;
    len += n;
    req[len] = 0;
    if ((headerEnd = strstr(req.get(), "\r\n\r\n"))) break;
  }

  if (!headerEnd) {
    if (len > 0) sendText(fd, 400, "Bad Request", "Invalid request");
  } else {
    Request r;
    r.fd = fd;
    r.head = req.get();
    r.body = (const uint8_t *)headerEnd + 4;
    r.bodyLen = len - (r.body - (const uint8_t *)req.get());
    headerEnd[2] = 0;  // terminate the header block so headerValue() does not search the body

    bool get = strncmp(r.head, "GET ", 4) == 0;
    bool post = strncmp(r.head, "POST ", 5) == 0;
    char path[160] = "";  // longest: /camdiag/send/<port>/<64 bytes as hex>; longer paths are not served
    const char *p = r.head + (get ? 4 : post ? 5 : 0);
    size_t plen = strcspn(p, " ?\r\n");
    if (plen < sizeof(path)) memcpy(path, p, plen), path[plen] = 0;
    r.path = path;

    if (!get && !post) sendText(fd, 405, "Method Not Allowed", "Only GET and POST");
    else route(r, get ? GET : POST);
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

void httpBegin() {
  nvsRead([](Preferences &p) { calibJson = p.getString("calib", "{}"); });
  xTaskCreatePinnedToCore(httpTask, "http", 4096, nullptr, 3, nullptr, 1);
}
