/*
 * i4season protocol (libWifiCamera): Soulear/Hopefox otoscopes, MS5 microscopes,
 * Suear, inskam and others. (The MAX-VIEW microscope speaks JHCMD, see cam_jhcmd.cpp.)
 *
 * Sequence (verified on the Soulear Find T): GetDeviceInfo to UDP 10005, then START
 * to UDP 10006, both from the same socket. JPEG chunks with a 16-byte header (type 1)
 * or 28-byte header (type 6) then arrive at the announced port.
 *
 * Sources:
 *   king-cake/otoscope-windows docs/i4season-protocol.md  (Ghidra, verified on the Find T)
 *   Fyfar/ms5-wifi-microscope README                        (MS5 microscope, 1280x720)
 *
 * The receive path is taken unchanged from the firmware proven on the ZB-GW03.
 * New: battery (devinfo + status push on UDP 10007), LED (command 0x0A), header
 * length by type byte, orientation only if the "has G-sensor" flag is set.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>

#include "camera.h"
#include "crashlog.h"

namespace {

const uint8_t MAGIC[4] = {0xEE, 0xFF, 0xEE, 0xFF};
const uint16_t CMD_DEVINFO = 0x0001, CMD_OPEN_VIDEO = 0x0004, CMD_STATUS = 0x0009, CMD_LED = 0x000A;
const uint16_t DEVINFO_PORT = 10005;  // requests (devinfo, LED, ...)
const uint16_t VIDEO_CTRL_PORT = 10006;  // START / OpenVideo
const uint16_t NOTIFY_PORT = 10007;   // camera -> us: status push with battery, ~1x/s
// GetDeviceInfo: magic, id 0, type 0x01, unk 1, err 0, length 0
const uint8_t DISCOVERY[12] = {0xEE, 0xFF, 0xEE, 0xFF, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00};

// Orientation sensor: header bytes 6-9 (little-endian) hold three 10-bit values (bits
// 0-9 x, 10-19 y, 20-29 z), each bit 9 = sign, bits 0-8 = magnitude. ~128 equals 1 g.
int16_t signMag10(uint32_t v) { return (v & 0x200) ? -(int16_t)(v & 0x1FF) : (int16_t)(v & 0x1FF); }

void decodeOrientation(const uint8_t *hdr) {
  uint32_t v = hdr[6] | (hdr[7] << 8) | (hdr[8] << 16) | ((uint32_t)hdr[9] << 24);
  telemetry.accX = signMag10(v);
  telemetry.accY = signMag10(v >> 10);
  telemetry.accZ = signMag10(v >> 20);
  telemetry.accSeq++;
  telemetry.hasOrientation = true;
}

void copyText(char *dst, size_t cap, const uint8_t *src, size_t n) {
  size_t i = 0;
  for (; i < n && i + 1 < cap && src[i]; i++) dst[i] = (src[i] >= 32 && src[i] < 127) ? src[i] : '?';
  dst[i] = 0;
}

// Battery: bits 1-7 = %, bit 0 = probably "charging" (100 % observed as 0xC8)
void setBattery(uint8_t b) {
  if ((b >> 1) <= 100) {
    telemetry.battery = b >> 1;
    telemetry.charging = b & 1;
  }
}

class I4seasonSession : public CamSession {
 public:
  explicit I4seasonSession(uint32_t camIp) {
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;  // let the stack pick a free port
    bind(sock_, (sockaddr *)&local, sizeof(local));
    socklen_t slen = sizeof(local);
    getsockname(sock_, (sockaddr *)&local, &slen);
    myPort_ = ntohs(local.sin_port);

    timeval tv = {0, 200 * 1000};
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // START: magic, id 2, type 0x04, unk 1, err 0, length 2, port (LE), 00 00
    const uint8_t start[16] = {0xEE, 0xFF, 0xEE, 0xFF, 0x02, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
                               (uint8_t)(myPort_ & 0xFF), (uint8_t)(myPort_ >> 8), 0x00, 0x00};
    memcpy(start_, start, sizeof(start_));

    discAddr_.sin_family = ctrlAddr_.sin_family = AF_INET;
    discAddr_.sin_addr.s_addr = ctrlAddr_.sin_addr.s_addr = camIp;
    discAddr_.sin_port = htons(DEVINFO_PORT);
    ctrlAddr_.sin_port = htons(VIDEO_CTRL_PORT);

    // Status push from the camera (battery). Optional: if binding fails, the battery
    // level from the devinfo reply at handshake remains.
    notify_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in n = {};
    n.sin_family = AF_INET;
    n.sin_addr.s_addr = htonl(INADDR_ANY);
    n.sin_port = htons(NOTIFY_PORT);
    if (notify_ >= 0 && bind(notify_, (sockaddr *)&n, sizeof(n)) != 0) {
      close(notify_);
      notify_ = -1;
    }

    telemetry.ledSupported = true;
    diagReset();
    diagLog("[i4season] camera %s, receiving on UDP port %u, notify port %s", IPAddress(camIp).toString().c_str(),
            myPort_, notify_ >= 0 ? "10007" : "-");
  }

  ~I4seasonSession() override {
    if (notify_ >= 0) close(notify_);
    close(sock_);
  }

  void poll(uint8_t *pkt, size_t cap) override {
    sockaddr_in from = {};
    socklen_t flen = sizeof(from);
    int n = recvfrom(sock_, pkt, cap, 0, (sockaddr *)&from, &flen);
    if (n > 0) logPacket(pkt, n, from);

    // Detect a stall, but give the camera time to start up after a START: a new
    // START before it is running restarts it (cascade observed at 200 ms).
    // The first packet after idle is often lost -> repeat the handshake.
    if (millis() - lastData_ > STALL_TIMEOUT_MS && millis() - lastStart_ >= HANDSHAKE_RETRY_MS) {
      if (haveSeq_) {  // video was running until now
        bool loss = lastLoss_ && millis() - lastLoss_ < STALL_TIMEOUT_MS + 2000;
        if (loss) {
          stats.stallsLoss++;
        } else {
          stats.cleanStallAt[stats.stallsClean % VideoStats::CLEAN_STALL_TIMES] = millis() / 1000;
          stats.stallsClean++;
        }
        crumb("stall: %u ms without data -> handshake (%s)", STALL_TIMEOUT_MS,
              loss ? "after packet loss" : "without packet loss");
      }
      haveSeq_ = false;
      if (cameraLinkUp()) {
        sendto(sock_, DISCOVERY, sizeof(DISCOVERY), 0, (sockaddr *)&discAddr_, sizeof(discAddr_));
        sendto(sock_, start_, sizeof(start_), 0, (sockaddr *)&ctrlAddr_, sizeof(ctrlAddr_));
        lastStart_ = millis();
        stats.handshakes++;
      }
      lastData_ = millis();
      inFrame_ = false;
      building_.reset();
    }

    handleLed();
    if (millis() - lastNotify_ >= 250) pollNotify();

    if (n <= 0) return;
    if (n >= 12 && memcmp(pkt, MAGIC, 4) == 0) return handleReply(pkt, n);  // replies/ACKs
    // type 1 = 16-byte header, type 6 = 28-byte header (6-axis sensor), 5 = audio, 2 = ?
    size_t hdrLen = pkt[0] == 6 ? 28 : 16;
    if (pkt[0] != 1 && pkt[0] != 6) return;
    if (n <= (int)hdrLen) return;
    lastData_ = millis();

    const uint8_t *payload = pkt + hdrLen;
    size_t plen = n - hdrLen;

    // The orientation is in every packet of the frame; take it from the first packet
    // of a new frame (byte 2) that arrives, so a lost start packet costs no reading.
    // Only if header byte 5 bit 0 ("has G-sensor") is set (Soulear: always, MS5: never)
    if (!haveSensorFrame_ || pkt[2] != sensorFrame_) {
      if (pkt[5] & 0x01) decodeOrientation(pkt);
      telemetry.width = pkt[12] | (pkt[13] << 8);
      telemetry.height = pkt[14] | (pkt[15] << 8);
      sensorFrame_ = pkt[2];
      haveSensorFrame_ = true;
    }

    // Running packet number (byte 1, 8 bits) -> detect lost packets
    uint8_t seq = pkt[1];
    if (haveSeq_ && seq != nextSeq_) {
      uint8_t gap = seq - nextSeq_;
      lastLoss_ = millis();
      stats.packetsLost += gap;
      frameBroken_ = true;
    }
    haveSeq_ = true;
    nextSeq_ = seq + 1;

    if (plen >= 2 && payload[0] == 0xFF && payload[1] == 0xD8) {
      if (inFrame_) {  // previous frame never finished (end lost)
        stats.framesDropped++;
        stats.dropIncomplete++;
      }
      building_ = Frame::create();  // new frame starts
      inFrame_ = (bool)building_;
      frameBroken_ = false;
      if (!inFrame_) {
        stats.framesDropped++;
        stats.dropNoMem++;
        return;
      }
    } else if (!inFrame_) {
      return;  // joined mid-frame -> wait for the next frame start
    }

    if (building_.size() + plen > MAX_FRAME_BYTES) {
      inFrame_ = false;
      building_.reset();
      stats.framesDropped++;
      stats.dropTooBig++;
      return;
    }
    if (!building_.append(payload, plen)) {
      inFrame_ = false;
      building_.reset();
      stats.framesDropped++;
      stats.dropNoMem++;
      return;
    }

    // Does the frame end here? (FF D9, possibly followed by padding zeros)
    size_t end = plen;
    while (end > 0 && payload[end - 1] == 0x00) end--;
    if (end >= 2 && payload[end - 2] == 0xFF && payload[end - 1] == 0xD9) {
      if (frameBroken_ && !SHOW_DAMAGED_FRAMES) {  // do not pass on a broken JPEG
        stats.framesDropped++;
        stats.dropIncomplete++;
      } else {
        if (frameBroken_) stats.framesDamaged++;
        building_.trimLast(plen - end);
        publishFrame(building_);
      }
      building_.reset();
      inFrame_ = false;
    }
  }

 private:
  // Diagnostics for unknown cameras: the first few replies and video packets of each
  // session as hex (length, sender port, first 20 bytes)
  void logPacket(const uint8_t *pkt, int n, const sockaddr_in &from) {
    bool reply = n >= 12 && memcmp(pkt, MAGIC, 4) == 0;
    uint8_t &count = reply ? loggedReplies_ : loggedData_;
    if (count >= 3) return;
    count++;
    char hex[20 * 3 + 1];
    int k = min(n, 20);
    for (int i = 0; i < k; i++) snprintf(hex + i * 3, 4, "%02x ", pkt[i]);
    hex[k * 3] = 0;
    diagLog("[i4season] %lu ms: %s %d bytes from port %u: %s", millis(), reply ? "reply" : "data", n,
            ntohs(from.sin_port), hex);
  }

  // Camera replies on our socket: devinfo (battery, product) and LED
  void handleReply(const uint8_t *pkt, int n) {
    uint16_t cmd = pkt[6] | (pkt[7] << 8);
    size_t len = pkt[10] | (pkt[11] << 8);
    const uint8_t *p = pkt + 12;
    size_t plen = min(len, (size_t)(n - 12));
    if (cmd == CMD_DEVINFO && plen >= 0x79) {
      portENTER_CRITICAL(&infoMux);
      copyText(telemetry.vendor, sizeof(telemetry.vendor), p + 0x01, 32);
      copyText(telemetry.product, sizeof(telemetry.product), p + 0x21, 32);
      copyText(telemetry.firmware, sizeof(telemetry.firmware), p + 0x41, 16);
      portEXIT_CRITICAL(&infoMux);
      setBattery(p[0x78]);
    } else if (cmd == CMD_LED && plen >= 2 && pkt[9] == 0) {
      telemetry.led = p[1] ? 1 : 0;  // reply = resulting state
      int want = ledRequest;
      if (want == telemetry.led) ledRequest.compare_exchange_strong(want, -1);
      crumb("LED confirmed: %s", p[1] ? "on" : "off");
    }
  }

  // Send the LED request and repeat it until the camera confirms it (max. 5x).
  // Payload: op (LED 1 | 0x10 = write), status 0/1, brightness (verified on the Find T)
  void handleLed() {
    int want = ledRequest;
    if (want < 0) {
      ledTries_ = 0;
      return;
    }
    if (!cameraLinkUp() || (ledTries_ && millis() - ledSent_ < 300)) return;
    if (ledTries_ >= 5) {
      crumb("LED: no confirmation");
      ledRequest.compare_exchange_strong(want, -1);
      ledTries_ = 0;
      return;
    }
    uint8_t cmd[15] = {0xEE, 0xFF, 0xEE, 0xFF, (uint8_t)(cmdSeq_ & 0xFF), (uint8_t)(cmdSeq_ >> 8),
                       (uint8_t)CMD_LED, 0x00, 0x01, 0x00, 0x03, 0x00,
                       0x11, (uint8_t)(want ? 1 : 0), (uint8_t)(want ? 100 : 0)};
    if (!ledTries_) cmdSeq_++;
    sendto(sock_, cmd, sizeof(cmd), 0, (sockaddr *)&discAddr_, sizeof(discAddr_));
    ledSent_ = millis();
    ledTries_++;
  }

  // Status push on UDP 10007: cmd 0x0009, payload type 0x02, payload byte 1 = battery,
  // byte 5 (packet byte 17) = counter that rises with every press of the otoscope button
  void pollNotify() {
    lastNotify_ = millis();
    if (notify_ < 0) return;
    uint8_t buf[64];
    for (int i = 0; i < 4; i++) {
      int m = recv(notify_, buf, sizeof(buf), MSG_DONTWAIT);
      if (m <= 0) return;
      if (m >= 18 && memcmp(buf, MAGIC, 4) == 0 && (buf[6] | (buf[7] << 8)) == CMD_STATUS &&
          buf[12] == 0x02) {
        setBattery(buf[13]);
        telemetry.hasButtons = true;
        if (btnCount_ >= 0 && buf[17] != btnCount_) telemetry.press(KEY_PHOTO);
        btnCount_ = buf[17];
      }
    }
  }

  int sock_ = -1, notify_ = -1;
  uint16_t myPort_ = 0;
  uint8_t start_[16];
  sockaddr_in discAddr_ = {}, ctrlAddr_ = {};
  Frame building_;             // the current JPEG grows here
  bool inFrame_ = false;
  bool frameBroken_ = false;   // a packet of the current frame is missing
  bool haveSeq_ = false;
  uint8_t nextSeq_ = 0;
  uint8_t sensorFrame_ = 0;    // frame number of the last orientation reading
  bool haveSensorFrame_ = false;
  uint32_t lastData_ = 0;
  uint32_t lastStart_ = 0;     // last START (handshake)
  uint32_t lastLoss_ = 0;      // last gap in the packet number
  uint32_t lastNotify_ = 0;
  int btnCount_ = -1;          // button press counter of the last status push
  uint8_t loggedReplies_ = 0, loggedData_ = 0;  // logPacket()
  uint16_t cmdSeq_ = 5;
  uint32_t ledSent_ = 0;
  int ledTries_ = 0;
};

CamSession *create(uint32_t camIp) { return new (std::nothrow) I4seasonSession(camIp); }

// GetDeviceInfo, as at the start of every handshake: the camera answers with its device
// info (verified on the Soulear; the MAX-VIEW does not answer it)
bool probe(uint32_t camIp) {
  return probeUdp(camIp, DEVINFO_PORT, 0, DISCOVERY, sizeof(DISCOVERY), [](const uint8_t *m, int n) {
    return n >= 12 && !memcmp(m, MAGIC, 4) && (m[6] | m[7] << 8) == CMD_DEVINFO;
  });
}

}  // namespace

const CamProtocol PROTOCOL_I4SEASON = {CamProto::I4season, "i4season", "i4season (Soulear, MS5)",
                                       ipv4(192, 168, 1, 1), create, probe};
