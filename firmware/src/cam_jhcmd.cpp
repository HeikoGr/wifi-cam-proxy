/*
 * MaxSee/JoyHonest protocol ("JHCMD"): older Wi-Fi microscopes, camera fixed at
 * 192.168.29.1. Commands to UDP 20000, video arrives at the fixed port 10900.
 *
 * Source: czietz/wifimicroscope (wifi_microscope_dump.py, BSD-2-Clause) and
 * https://www.chzsoft.de/site/hardware/reverse-engineering-a-wifi-microscope/
 * Tested with a MAX-VIEW microscope (MAXVIEW-xxxx).
 *
 * Video packet: 8-byte header (bytes 0-1 frame number LE, byte 3 packet number within
 * the frame, 0 = first), then JPEG data. MAX-VIEW (observed, 1280x720): bytes 0-1
 * always 01 00, byte 2 = number of packets of this frame, bytes 4-7 = 02 14 00 00, and a
 * 16-byte block before the JPEG in packet 0. Orientation, battery and LED are not known.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>

#include "camera.h"
#include "crashlog.h"

namespace {

const uint16_t CMD_PORT = 20000;  // camera's command port, and our own: the camera sends
                                  // its messages to port 20000 of the client
const uint16_t FDWN_PORT = 20001;  // the same LED message again, as "FDWN" (received only)
const uint16_t VIDEO_PORT = 10900;
const size_t HDR_LEN = 8;
// Like the MAX-VIEW app (sniffed): the init sequence only when connecting, then a
// heartbeat (START) every 3 s. Sending the full init again after every second of
// silence restarted the camera before it was sending again: a cascade of up to one
// handshake per second with hardly a frame in between (measured with 98 KB frames).
const uint32_t JH_STALL_MS = 1000;      // no video this long: heartbeat right away
const uint32_t JH_REINIT_MS = 3000;     // still nothing: full init again, at most this often
const uint32_t JH_HEARTBEAT_MS = 3000;  // heartbeat while the video runs
const uint32_t SIDE_POLL_MS = 50;       // command/status sockets, see poll()

const uint8_t CMD_INIT1[] = {'J', 'H', 'C', 'M', 'D', 0x10, 0x00};
const uint8_t CMD_INIT2[] = {'J', 'H', 'C', 'M', 'D', 0x20, 0x00};
const uint8_t CMD_START[] = {'J', 'H', 'C', 'M', 'D', 0xD0, 0x01};  // heartbeat, starts the data
const uint8_t CMD_STOP[] = {'J', 'H', 'C', 'M', 'D', 0xD0, 0x02};
// LED brightness: "JHCMD" 20 02 <0..100>, 0 = off. Sniffed from the MAX-VIEW app (iOS),
// which sends every slider value; the camera does not answer it directly.
// Messages of the MAX-VIEW to port 20000 of the client (sniffed with HT40):
//   "JHCMD" 10 20 <level>   LED level changed with the light button (100/60/30/0)
//   "JHCMD" 20 00 61 ...     reply to INIT2 (105 bytes, after every handshake): at
//                            offset 24 the device name ("YPC320"). Byte 7 is always
//                            0x61, whatever the LED does: not the level.
//   "JHCMD" 00 <key>        photo 01, zoom+ 04, zoom- 05; 00 00 follows 50 ms later (release)
// Only the button is reported: commands from the client (20 02) are not.
// The same level also goes as "FDWN" 20 00 0e 00 01 00 <level> to port 20001: both are
// received, so a message is only missed if both UDP packets get lost (weak Wi-Fi).
// Status query of the app, every 5 s: "FDWN" 00 00 01 00 00 00 to port 20001; the camera
// answers to the client's port 20001 with 48 bytes: "FDWN" 00 00 01 00 1a 00 00 05 ...,
// byte 32 is the battery, at 40 the camera's MAC. Compared with the app's display (10 %
// steps, rounded down) by sniffing: raw 110 = 10 %, 115 = 20, 137 = 40, 148 = 50, 60 % from
// 148..152 to 160, 70 % from 160..162. That fits percent = raw - 91 (60 % at 151, 70 % at
// 161). The value is higher while charging (+36 when the cable was plugged in, no flag).
const uint8_t FDWN_STATUS_REQ[] = {'F', 'D', 'W', 'N', 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
const size_t FDWN_STATUS_LEN = 48;
const size_t FDWN_BATTERY_BYTE = 32;
const int BATTERY_RAW_ZERO = 91;  // percent = raw - 91
const uint32_t FDWN_POLL_MS = 5000;

class JhcmdSession : public CamSession {
 public:
  explicit JhcmdSession(uint32_t camIp) {
    cmd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    vid_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(VIDEO_PORT);
    if (bind(vid_, (sockaddr *)&local, sizeof(local)) != 0) crumb("jhcmd: port %u in use", VIDEO_PORT);
    local.sin_port = htons(CMD_PORT);  // receive the camera's messages (LED level)
    if (bind(cmd_, (sockaddr *)&local, sizeof(local)) != 0) crumb("jhcmd: port %u in use", CMD_PORT);
    fdwn_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    local.sin_port = htons(FDWN_PORT);
    if (fdwn_ >= 0 && bind(fdwn_, (sockaddr *)&local, sizeof(local)) != 0) {
      close(fdwn_);
      fdwn_ = -1;
    }
    timeval tv = {0, 200 * 1000};
    setsockopt(vid_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    camAddr_.sin_family = AF_INET;
    camAddr_.sin_addr.s_addr = camIp;
    camAddr_.sin_port = htons(CMD_PORT);
    telemetry.ledSupported = true;  // MAX-VIEW: yes; other MaxSee devices have a hardware dimmer
    telemetry.ledDimmable = true;
    telemetry.hasButtons = true;
    diagReset();
    diagLog("[jhcmd] camera %s, receiving on UDP port %u", IPAddress(camIp).toString().c_str(), VIDEO_PORT);
  }

  ~JhcmdSession() override {
    send(CMD_STOP, sizeof(CMD_STOP));
    close(vid_);
    close(cmd_);
    if (fdwn_ >= 0) close(fdwn_);
  }

  void poll(uint8_t *pkt, size_t cap) override {
    sockaddr_in from = {};
    socklen_t flen = sizeof(from);
    int n = recvfrom(vid_, pkt, cap, 0, (sockaddr *)&from, &flen);
    if (n > 0) logPacket("data", pkt, n, from);
    // Diagnostics: packets on 10900 that do not look like video (status from the camera,
    // e.g. after the light button?): not 1450 bytes and not the last packet of a frame,
    // or other header bytes 5-7 than 14 00 00
    if (n > 0 && oddLogged_ < 20 &&
        (n <= (int)HDR_LEN || (n != 1450 && pkt[3] + 1 != pkt[2]) || pkt[5] != 0x14 || pkt[6] || pkt[7])) {
      oddLogged_++;
      char hex[32 * 3 + 1];
      int k = min(n, 32);
      for (int i = 0; i < k; i++) snprintf(hex + i * 3, 4, "%02x ", pkt[i]);
      hex[k * 3] = 0;
      diagLog("[jhcmd] %lu ms: odd packet %d bytes on %u from port %u: %s", millis(), n, VIDEO_PORT,
              ntohs(from.sin_port), hex);
    }
    if (n > (int)HDR_LEN) captureRaw(pkt, n);
    uint32_t now = millis();
    bool link = cameraLinkUp();
    // The other sockets only every SIDE_POLL_MS while the video runs: per video packet
    // (~1500/s with the MAX-VIEW) they cost two empty recvfrom calls for nothing
    if (n <= 0 || now - lastSide_ >= SIDE_POLL_MS) {
      lastSide_ = now;
      pollSide(link);
    }

    if (link) {
      if (now - lastData_ > JH_STALL_MS) {  // no video
        if (running_) {
          stats.stallsLoss++;
          crumb("jhcmd: %u ms without data -> heartbeat", JH_STALL_MS);
          running_ = false;
          building_.reset();
        }
        if ((!inited_ || now - lastData_ >= JH_REINIT_MS) && now - lastInit_ >= JH_REINIT_MS) {
          send(CMD_INIT1, sizeof(CMD_INIT1));
          send(CMD_INIT2, sizeof(CMD_INIT2));
          send(CMD_START, sizeof(CMD_START));
          send(CMD_START, sizeof(CMD_START));
          inited_ = true;
          lastInit_ = lastBeat_ = now;
          stats.handshakes++;
        } else if (now - lastBeat_ >= JH_STALL_MS) {
          send(CMD_START, sizeof(CMD_START));
          lastBeat_ = now;
          stats.keepalives++;
        }
      } else if (now - lastBeat_ >= JH_HEARTBEAT_MS) {
        send(CMD_START, sizeof(CMD_START));
        lastBeat_ = now;
        stats.keepalives++;
      }
    }

    handleLed();
    if (n <= (int)HDR_LEN) return;
    lastData_ = millis();
    running_ = true;

    uint16_t fno = pkt[0] | (pkt[1] << 8);
    uint8_t idx = pkt[3], total = pkt[2];
    const uint8_t *payload = pkt + HDR_LEN;
    size_t plen = n - HDR_LEN;

    // Packets can arrive out of order (observed on the MAX-VIEW: 3 before 2). So each
    // one is put at its place by packet number. A packet that belongs to the next frame
    // (other frame number, or a packet number we already have) ends the current one.
    if (building_ && (fno != frame_ || have(idx))) finishFrame(false);
    // After a frame was given up midway (no memory, too big), ignore its remaining
    // packets: otherwise they would start a "frame" without a beginning that only costs
    // memory and is dropped again. It ends with the next frame number or packet 0.
    if (skipping_) {
      if (fno == skipFno_ && idx != 0) return;
      skipping_ = false;
    }
    if (!building_) {
      building_ = Frame::create();
      if (!building_) {
        stats.framesDropped++;
        stats.dropNoMem++;
        return;
      }
      frame_ = fno;
      count_ = total_ = 0;
      endIdx_ = -1;
      haveStart_ = false;
    }
    if (total) total_ = total;  // MAX-VIEW: byte 2 = packets in this frame

    if (idx == 0) {
      // The JPEG does not always start right after the header: the MAX-VIEW puts a
      // 16-byte block of its own in front. So search for FF D8 FF and skip the rest.
      size_t skip = 0;
      while (skip + 3 <= plen && !(payload[skip] == 0xFF && payload[skip + 1] == 0xD8 && payload[skip + 2] == 0xFF))
        skip++;
      if (skip + 3 > plen) {
        if (!noJpegStart_++) diagLog("[jhcmd] no JPEG start (FF D8 FF) in packet 0 of frame %u", fno);
        return;  // the frame stays incomplete and is dropped
      }
      if (skip != lastSkip_) {
        lastSkip_ = skip;
        diagLog("[jhcmd] JPEG starts at offset %u of packet 0", (unsigned)skip);
      }
      payload += skip;
      plen -= skip;
      haveStart_ = true;
    }

    // JPEG end (FF D9, possibly followed by padding zeros): cut the padding right away
    size_t end = plen;
    while (end > 0 && payload[end - 1] == 0x00) end--;
    if (end >= 2 && payload[end - 2] == 0xFF && payload[end - 1] == 0xD9) {
      endIdx_ = idx;
      plen = end;
    }

    if (building_.size() + plen > MAX_FRAME_BYTES) {
      giveUp();
      stats.dropTooBig++;
      return;
    }
    int pos = 0;  // number of stored packets with a smaller number
    while (pos < count_ && idxs_[pos] < idx) pos++;
    if (!building_.insert(pos, payload, plen)) {
      giveUp();
      stats.dropNoMem++;
      return;
    }
    memmove(&idxs_[pos + 1], &idxs_[pos], count_ - pos);
    idxs_[pos] = idx;
    count_++;

    // Complete: all packets up to the one with FF D9 are there (byte 2 is only used to
    // estimate losses: whether other MaxSee devices fill it the same way is unknown)
    if (haveStart_ && endIdx_ >= 0 && count_ == endIdx_ + 1) finishFrame(true);
  }

 private:
  // Messages of the camera on port 20000 and 20001 (LED level, device name, battery),
  // the app's status query, experiments from /camdiag/send
  void pollSide(bool link) {
    // Messages of the camera on our port 20000 (LED level, device name)
    sockaddr_in from = {};
    socklen_t flen = sizeof(from);
    uint8_t reply[128];
    int r = recvfrom(cmd_, reply, sizeof(reply), MSG_DONTWAIT, (sockaddr *)&from, &flen);
    if (r > 0) {
      logReply(reply, r, from);
      handleMessage(reply, r);
    }
    if (fdwn_ >= 0) {
      flen = sizeof(from);
      r = recvfrom(fdwn_, reply, sizeof(reply), MSG_DONTWAIT, (sockaddr *)&from, &flen);
      // "FDWN" 20 00 0e 00 01 00 <level>: same meaning as "JHCMD" 10 20 <level>
      if (r >= 11 && !memcmp(reply, "FDWN", 4) && reply[4] == 0x20 && reply[6] == 0x0E) {
        const uint8_t m[8] = {'J', 'H', 'C', 'M', 'D', 0x10, 0x20, reply[10]};
        handleMessage(m, sizeof(m));
      } else if (r == (int)FDWN_STATUS_LEN && !memcmp(reply, "FDWN", 4) && reply[6] == 0x01) {
        handleStatus(reply);
      }
      // Status query like the app does (needs the video running = the camera is serving us)
      if (running_ && link && millis() - lastStatusReq_ >= FDWN_POLL_MS) {
        lastStatusReq_ = millis();
        sockaddr_in to = camAddr_;
        to.sin_port = htons(FDWN_PORT);
        sendto(fdwn_, FDWN_STATUS_REQ, sizeof(FDWN_STATUS_REQ), 0, (sockaddr *)&to, sizeof(to));
      }
    }
    // Experiment from /camdiag/send, sent from the command socket (port 20000)
    uint8_t out[64];
    size_t olen = sizeof(out);
    uint16_t oport;
    if (link && diagSendTake(oport, out, olen)) {
      sockaddr_in to = camAddr_;
      to.sin_port = htons(oport);
      sendto(cmd_, out, olen, 0, (sockaddr *)&to, sizeof(to));
      char hex[64 * 3 + 1];
      for (size_t i = 0; i < olen; i++) snprintf(hex + i * 3, 4, "%02x ", out[i]);
      hex[olen * 3] = 0;
      diagLog("[jhcmd] %lu ms: sent to port %u: %s", millis(), oport, hex);
    }
  }

  // No confirmation from the camera: send twice and take the state as set
  void handleLed() {
    int want = ledRequest;
    if (want < 0 || !cameraLinkUp()) return;
    int level = want ? constrain((int)ledLevel, 1, 100) : 0;
    const uint8_t cmd[8] = {'J', 'H', 'C', 'M', 'D', 0x20, 0x02, (uint8_t)level};
    send(cmd, sizeof(cmd));
    send(cmd, sizeof(cmd));
    if (ledRequest.compare_exchange_strong(want, -1)) {
      telemetry.led = want ? 1 : 0;
      // The slider may have moved meanwhile (the web UI sets ledLevel before
      // ledRequest): then send the new value right away instead of losing it
      if (want && constrain((int)ledLevel, 1, 100) != level) ledRequest = 1;
    }
  }

  void giveUp() {
    building_.reset();
    stats.framesDropped++;
    skipping_ = true;
    skipFno_ = frame_;
  }

  bool have(uint8_t idx) const {
    for (int i = 0; i < count_; i++)
      if (idxs_[i] == idx) return true;
    return false;
  }

  // Publish the frame being built if it is a whole JPEG. complete = all packets there;
  // otherwise packets are missing: count them, show the frame only with
  // SHOW_DAMAGED_FRAMES (and only with JPEG start and end).
  void finishFrame(bool complete) {
    bool jpeg = count_ > 0 && haveStart_ && endIdx_ >= 0 && idxs_[count_ - 1] == endIdx_;
    if (!complete) {
      int expected = total_ ? total_ : endIdx_ >= 0 ? endIdx_ + 1 : count_ ? idxs_[count_ - 1] + 1 : 0;
      if (expected > count_) stats.packetsLost += expected - count_;
    }
    if (jpeg && (complete || SHOW_DAMAGED_FRAMES)) {
      if (!complete) stats.framesDamaged++;
      if (JPEG_COLLAPSE_FILL) collapseFill(building_);
      publishFrame(building_);
    } else {
      stats.framesDropped++;
      stats.dropIncomplete++;
    }
    building_.reset();
  }

  // The MAX-VIEW pads every restart interval with up to 32 fill bytes FF before the
  // RSTn marker (allowed by the JPEG standard). JPEGDEC (CYD) stops decoding at the first
  // run of two or more: the rest of the image kept the previous frame, horizontal
  // streaks. So every run in the entropy-coded data is cut down to one FF, in place
  // (chunks only get shorter). Browsers decode both the same way: only with
  // JPEG_COLLAPSE_FILL (CYD).
  static void collapseFill(Frame &f) {
    static uint8_t buf[2048];
    if (!f.chunks() || f.chunkLen(0) > sizeof(buf)) return;
    // Header: markers up to SOS (FF DA) must be in chunk 0, otherwise leave the frame as it is
    size_t n = f.chunkLen(0), pos = 2;
    copyFromChunk(buf, f.chunk(0), 0, n);
    while (pos + 4 <= n && buf[pos] == 0xFF && buf[pos + 1] != 0xDA) pos += 2 + (buf[pos + 2] << 8 | buf[pos + 3]);
    if (pos + 4 > n || buf[pos] != 0xFF || buf[pos + 1] != 0xDA) return;
    size_t scan = pos + 2 + (buf[pos + 2] << 8 | buf[pos + 3]);  // first byte of the scan data
    if (scan > n) return;
    bool prevFF = false;  // last byte kept was FF
    for (int c = 0; c < f.chunks(); c++) {
      size_t len = f.chunkLen(c), from = c ? 0 : scan;
      if (len > sizeof(buf)) return;
      if (c) copyFromChunk(buf, f.chunk(c), 0, len);
      size_t out = from;
      for (size_t i = from; i < len; i++) {
        if (buf[i] == 0xFF && prevFF) continue;  // fill byte
        prevFF = buf[i] == 0xFF;
        buf[out++] = buf[i];
      }
      if (out != len) {
        copyToChunk((uint8_t *)f.chunk(c), buf, out);
        f.shrinkChunk(c, out);
      }
    }
  }

  // Raw capture for /camdiag/raw: all packets of one frame, from packet 0 until the
  // next frame number appears
  void captureRaw(const uint8_t *pkt, int n) {
    uint16_t fno = pkt[0] | (pkt[1] << 8);
    if (raw_) {
      // until the next packet 0 (the MAX-VIEW keeps the frame number constant)
      if (pkt[3] != 0 && fno == rawFno_ && raw_.append(pkt, n)) return;
      diagRawPut(raw_);  // next frame (or no memory): hand over what we have
      diagLog("[jhcmd] raw capture: frame %u, %u bytes", rawFno_, (unsigned)raw_.size());
      raw_.reset();
    } else if (pkt[3] == 0 && diagRawWanted()) {
      raw_ = Frame::create();
      rawFno_ = fno;
      if (raw_ && !raw_.append(pkt, n)) raw_.reset();
    }
  }

  // "JHCMD" 10 20 <level>: LED changed with the light button. Reply to INIT2: name.
  void handleMessage(const uint8_t *m, int n) {
    if (n == 7 && !memcmp(m, "JHCMD", 5) && m[5] == 0x00) {
      // 00 00 = release, ignored
      uint8_t key = m[6] == 0x01 ? KEY_PHOTO : m[6] == 0x04 ? KEY_ZOOM_IN : m[6] == 0x05 ? KEY_ZOOM_OUT : KEY_NONE;
      if (key) telemetry.press(key);
      return;
    }
    if (n < 8 || memcmp(m, "JHCMD", 5)) return;
    if (m[5] == 0x10 && m[6] == 0x20 && m[7] <= 100) {
      telemetry.led = m[7] ? 1 : 0;
      if (m[7]) ledLevel = m[7];
    } else if (m[5] == 0x20 && m[6] == 0x00 && n >= 40) {  // device name at offset 24
      portENTER_CRITICAL(&infoMux);
      copyName(telemetry.product, sizeof(telemetry.product), m + 24, min(16, n - 24));
      portEXIT_CRITICAL(&infoMux);
    }
  }

  // 48-byte status answer on 20001: byte 32 = battery (percent = raw - 91, like the app
  // shown in 10 % steps). Logged whenever something in it changes.
  void handleStatus(const uint8_t *m) {
    int raw = m[FDWN_BATTERY_BYTE];
    telemetry.batteryRaw = raw;
    telemetry.battery = constrain(raw - BATTERY_RAW_ZERO, 0, 100) / 10 * 10;
    if (haveStatus_ && !memcmp(lastStatus_, m, FDWN_STATUS_LEN)) return;
    haveStatus_ = true;
    memcpy(lastStatus_, m, FDWN_STATUS_LEN);
    char hex[24 * 3 + 1];
    for (int half = 0; half < 2; half++) {
      for (int i = 0; i < 24; i++) snprintf(hex + i * 3, 4, "%02x ", m[half * 24 + i]);
      diagLog("[jhcmd] %lu ms: FDWN status +%02d: %s", millis(), half * 24, hex);
    }
  }

  static void copyName(char *dst, size_t cap, const uint8_t *src, int n) {
    int i = 0;
    for (; i < n && i + 1 < (int)cap && src[i]; i++) dst[i] = (src[i] >= 32 && src[i] < 127) ? src[i] : '?';
    dst[i] = 0;
  }

  // Messages on port 20000: every change of the content as a line, identical repeats
  // only counted
  void logReply(const uint8_t *pkt, int n, const sockaddr_in &from) {
    Last &l = lastReply_;
    int k = min(n, (int)sizeof(l.data));
    if (l.count && l.len == n && !memcmp(l.data, pkt, k)) {
      l.count++;
      return;
    }
    if (l.count > 1) diagLog("[jhcmd] (previous message %u times)", l.count);
    l.len = n;
    l.count = 1;
    memcpy(l.data, pkt, k);
    // short messages in one line, long ones (the 105-byte info reply) in lines of 32 bytes
    char hex[32 * 3 + 1];
    if (k <= 24) {
      for (int i = 0; i < k; i++) snprintf(hex + i * 3, 4, "%02x ", pkt[i]);
      hex[k * 3] = 0;
      diagLog("[jhcmd] %lu ms: message %d bytes from port %u: %s", millis(), n, ntohs(from.sin_port), hex);
      return;
    }
    diagLog("[jhcmd] %lu ms: message %d bytes from port %u:", millis(), n, ntohs(from.sin_port));
    for (int o = 0; o < k; o += 32) {
      int m = min(32, k - o);
      for (int i = 0; i < m; i++) snprintf(hex + i * 3, 4, "%02x ", pkt[o + i]);
      hex[m * 3] = 0;
      diagLog("[jhcmd]  +%03d: %s", o, hex);
    }
  }

  // Diagnostics: the first packets of each kind as hex (length, sender port, 24 bytes)
  void logPacket(const char *kind, const uint8_t *pkt, int n, const sockaddr_in &from) {
    uint8_t &count = kind[0] == 'd' ? loggedData_ : loggedReplies_;
    if (count >= 4) return;
    count++;
    char hex[24 * 3 + 1];
    int k = min(n, 24);
    for (int i = 0; i < k; i++) snprintf(hex + i * 3, 4, "%02x ", pkt[i]);
    hex[k * 3] = 0;
    diagLog("[jhcmd] %lu ms: %s %d bytes from port %u: %s", millis(), kind, n, ntohs(from.sin_port), hex);
  }

  void send(const uint8_t *data, size_t len) {
    sendto(cmd_, data, len, 0, (sockaddr *)&camAddr_, sizeof(camAddr_));
  }

  int cmd_ = -1, vid_ = -1, fdwn_ = -1;
  uint32_t lastStatusReq_ = 0;
  uint32_t lastSide_ = 0;  // last pollSide()
  bool haveStatus_ = false;
  uint8_t lastStatus_[FDWN_STATUS_LEN];
  sockaddr_in camAddr_ = {};
  struct Last {
    int len = 0;
    uint32_t count = 0;
    uint8_t data[128];
  } lastReply_;
  Frame building_;
  uint8_t idxs_[MAX_CHUNKS];  // packet numbers of the chunks in building_, sorted
  int count_ = 0;             // packets in building_
  int total_ = 0;             // packets announced for this frame (0 = unknown)
  int endIdx_ = -1;           // packet with FF D9
  bool haveStart_ = false;    // packet 0 with FF D8 is there
  bool skipping_ = false;     // rest of a given-up frame (see giveUp)
  uint16_t skipFno_ = 0;
  bool running_ = false;
  uint16_t frame_ = 0;
  uint8_t loggedData_ = 0, loggedReplies_ = 0, oddLogged_ = 0;
  uint32_t noJpegStart_ = 0;
  size_t lastSkip_ = 0;  // offset of the JPEG start in packet 0, logged when it changes
  Frame raw_;  // raw capture in progress
  uint16_t rawFno_ = 0;
  uint32_t lastData_ = 0;
  uint32_t lastInit_ = 0, lastBeat_ = 0;  // last full init / last START sent
  bool inited_ = false;                    // init sequence sent in this session
};

CamSession *create(uint32_t camIp) { return new (std::nothrow) JhcmdSession(camIp); }

// INIT2 ("JHCMD" 20 00) alone, from our port 20000: the MAX-VIEW answers it with its
// 105-byte info reply to the client's port 20000 (sniffed after every handshake).
// Whether it answers INIT2 without INIT1 before is not verified on the device; without
// an answer "automatic" falls back to address and SSID. Not the whole init: sent again
// shortly after by the session, that might restart the camera (see JH_REINIT_MS).
bool probe(uint32_t camIp) {
  return probeUdp(camIp, CMD_PORT, CMD_PORT, CMD_INIT2, sizeof(CMD_INIT2), [](const uint8_t *m, int n) {
    return n >= 24 && !memcmp(m, "JHCMD", 5) && m[5] == 0x20 && m[6] == 0x00;
  });
}

}  // namespace

const CamProtocol PROTOCOL_JHCMD = {CamProto::Jhcmd, "jhcmd", "MaxSee/JoyHonest/MAX-VIEW (JHCMD)",
                                    ipv4(192, 168, 29, 1), create, probe};
