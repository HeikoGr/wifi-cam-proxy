/*
 * MaxSee/JoyHonest protocol ("JHCMD"): older Wi-Fi microscopes, camera fixed at
 * 192.168.29.1. Commands to UDP 20000, video arrives at the fixed port 10900.
 *
 * Source: czietz/wifimicroscope (wifi_microscope_dump.py, BSD-2-Clause) and
 * https://www.chzsoft.de/site/hardware/reverse-engineering-a-wifi-microscope/
 * Not yet tested on a real device.
 *
 * Video packet: 8-byte header (bytes 0-1 frame number LE, byte 3 packet number within
 * the frame, 0 = first), then JPEG data. Orientation, battery and LED are not known.
 */

#include <Arduino.h>
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
    Serial.printf("[jhcmd] receiving on UDP port %u\n", VIDEO_PORT);
  }

  ~JhcmdSession() override {
    send(CMD_STOP, sizeof(CMD_STOP));
    close(vid_);
    close(cmd_);
  }

  void poll(uint8_t *pkt, size_t cap) override {
    int n = recvfrom(vid_, pkt, cap, 0, nullptr, nullptr);

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
      inFrame_ = false;
      building_.reset();
    }

    if (n <= (int)HDR_LEN) return;
    lastData_ = millis();
    running_ = true;

    uint16_t fno = pkt[0] | (pkt[1] << 8);
    uint8_t idx = pkt[3];
    const uint8_t *payload = pkt + HDR_LEN;
    size_t plen = n - HDR_LEN;

    if (idx == 0) {  // new frame starts
      if (inFrame_) {  // previous frame ended without FF D9
        stats.framesDropped++;
        stats.dropIncomplete++;
      }
      inFrame_ = plen >= 2 && payload[0] == 0xFF && payload[1] == 0xD8;
      if (!inFrame_) return;
      building_ = Frame::create();
      if (!building_) {
        inFrame_ = false;
        stats.framesDropped++;
        stats.dropNoMem++;
        return;
      }
      frame_ = fno;
      nextIdx_ = 0;
      broken_ = false;
      // the heartbeat keeps the data stream running
      if (fno % JH_HEARTBEAT_FRAMES == 0) {
        send(CMD_START, sizeof(CMD_START));
        stats.keepalives++;
      }
    }
    if (!inFrame_ || fno != frame_) return;

    if (idx != nextIdx_) {  // packet(s) of the frame lost
      stats.packetsLost += (uint8_t)(idx - nextIdx_);
      broken_ = true;
    }
    nextIdx_ = idx + 1;

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

    size_t end = plen;
    while (end > 0 && payload[end - 1] == 0x00) end--;
    if (end >= 2 && payload[end - 2] == 0xFF && payload[end - 1] == 0xD9) {
      if (broken_ && !SHOW_DAMAGED_FRAMES) {
        stats.framesDropped++;
        stats.dropIncomplete++;
      } else {
        if (broken_) stats.framesDamaged++;
        building_.trimLast(plen - end);
        publishFrame(building_);
      }
      building_.reset();
      inFrame_ = false;
    }
  }

 private:
  void send(const uint8_t *data, size_t len) {
    sendto(cmd_, data, len, 0, (sockaddr *)&camAddr_, sizeof(camAddr_));
  }

  int cmd_ = -1, vid_ = -1;
  sockaddr_in camAddr_ = {};
  Frame building_;
  bool inFrame_ = false, broken_ = false, running_ = false;
  uint16_t frame_ = 0;
  uint8_t nextIdx_ = 0;
  uint32_t lastData_ = 0, lastStart_ = 0;
};

}  // namespace

CamSession *createJhcmdSession(uint32_t camIp) { return new (std::nothrow) JhcmdSession(camIp); }
