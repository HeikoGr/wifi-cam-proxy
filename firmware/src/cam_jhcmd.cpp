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

const uint16_t CMD_PORT = 20000;
const uint16_t VIDEO_PORT = 10900;
const size_t HDR_LEN = 8;
const uint32_t JH_STALL_MS = 1000;      // per czietz: heartbeat again after 1 s of silence
const uint32_t JH_HEARTBEAT_FRAMES = 50;

const uint8_t CMD_INIT1[] = {'J', 'H', 'C', 'M', 'D', 0x10, 0x00};
const uint8_t CMD_INIT2[] = {'J', 'H', 'C', 'M', 'D', 0x20, 0x00};
const uint8_t CMD_START[] = {'J', 'H', 'C', 'M', 'D', 0xD0, 0x01};  // heartbeat, starts the data
const uint8_t CMD_STOP[] = {'J', 'H', 'C', 'M', 'D', 0xD0, 0x02};

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
    timeval tv = {0, 200 * 1000};
    setsockopt(vid_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    camAddr_.sin_family = AF_INET;
    camAddr_.sin_addr.s_addr = camIp;
    camAddr_.sin_port = htons(CMD_PORT);
    telemetry.ledSupported = false;
    diagReset();
    diagLog("[jhcmd] camera %s, receiving on UDP port %u", IPAddress(camIp).toString().c_str(), VIDEO_PORT);
  }

  ~JhcmdSession() override {
    send(CMD_STOP, sizeof(CMD_STOP));
    close(vid_);
    close(cmd_);
  }

  void poll(uint8_t *pkt, size_t cap) override {
    sockaddr_in from = {};
    socklen_t flen = sizeof(from);
    int n = recvfrom(vid_, pkt, cap, 0, (sockaddr *)&from, &flen);
    if (n > 0) logPacket("data", pkt, n, from);
    if (n > (int)HDR_LEN) captureRaw(pkt, n);
    // Replies on the command socket (not used by the protocol, only logged)
    uint8_t reply[64];
    flen = sizeof(from);
    int r = recvfrom(cmd_, reply, sizeof(reply), MSG_DONTWAIT, (sockaddr *)&from, &flen);
    if (r > 0) logPacket("reply", reply, r, from);

    if (millis() - lastData_ > JH_STALL_MS && millis() - lastStart_ >= JH_STALL_MS) {
      if (running_) {
        stats.stallsLoss++;
        crumb("jhcmd: %u ms without data -> handshake", JH_STALL_MS);
      }
      running_ = false;
      if (cameraLinkUp()) {
        send(CMD_INIT1, sizeof(CMD_INIT1));
        send(CMD_INIT2, sizeof(CMD_INIT2));
        send(CMD_START, sizeof(CMD_START));
        send(CMD_START, sizeof(CMD_START));
        lastStart_ = millis();
        stats.handshakes++;
      }
      lastData_ = millis();
      building_.reset();
    }

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
      // The heartbeat keeps the data stream running. Counted by ourselves: the MAX-VIEW
      // always sends frame number 1.
      if (++framesSeen_ % JH_HEARTBEAT_FRAMES == 0) {
        send(CMD_START, sizeof(CMD_START));
        stats.keepalives++;
      }
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
      publishFrame(building_);
    } else {
      stats.framesDropped++;
      stats.dropIncomplete++;
    }
    building_.reset();
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

  int cmd_ = -1, vid_ = -1;
  sockaddr_in camAddr_ = {};
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
  uint32_t framesSeen_ = 0;  // for the heartbeat
  uint8_t loggedData_ = 0, loggedReplies_ = 0;
  uint32_t noJpegStart_ = 0;
  size_t lastSkip_ = 0;  // offset of the JPEG start in packet 0, logged when it changes
  Frame raw_;  // raw capture in progress
  uint16_t rawFno_ = 0;
  uint32_t lastData_ = 0, lastStart_ = 0;
};

}  // namespace

CamSession *createJhcmdSession(uint32_t camIp) { return new (std::nothrow) JhcmdSession(camIp); }
