/*
 * i4season-Protokoll (libWifiCamera): Soulear/Hopefox-Otoskope, MS5- und vermutlich
 * MAX-VIEW-Mikroskope, Suear, inskam u.a.
 *
 * Ablauf (am Soulear Find T verifiziert): GetDeviceInfo an UDP 10005, danach START
 * an UDP 10006, beides vom selben Socket. Dann kommen JPEG-Stücke mit 16-Byte-Kopf
 * (Typ 1) bzw. 28-Byte-Kopf (Typ 6) an den gemeldeten Port.
 *
 * Quellen:
 *   king-cake/otoscope-windows docs/i4season-protocol.md  (Ghidra, am Find T verifiziert)
 *   Fyfar/ms5-wifi-microscope README                        (MS5-Mikroskop, 1280x720)
 *
 * Der Empfangsteil ist unverändert aus der am ZB-GW03 erprobten Firmware übernommen.
 * Neu: Akku (Devinfo + Status-Push auf UDP 10007), LED (Befehl 0x0A), Kopflänge nach
 * Typ-Byte, Lagesensor nur, wenn das Flag "hat G-Sensor" gesetzt ist.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>

#include "camera.h"
#include "crashlog.h"

namespace {

const uint8_t MAGIC[4] = {0xEE, 0xFF, 0xEE, 0xFF};
const uint16_t CMD_DEVINFO = 0x0001, CMD_OPEN_VIDEO = 0x0004, CMD_STATUS = 0x0009, CMD_LED = 0x000A;
const uint16_t DEVINFO_PORT = 10005;  // Anfragen (Devinfo, LED, ...)
const uint16_t VIDEO_CTRL_PORT = 10006;  // START / OpenVideo
const uint16_t NOTIFY_PORT = 10007;   // Kamera -> uns: Status-Push mit Akku, ~1x/s

// Lagesensor: Kopf-Bytes 6-9 (little-endian) enthalten drei 10-Bit-Werte (Bits 0-9 x,
// 10-19 y, 20-29 z), je Bit 9 = Vorzeichen, Bits 0-8 = Betrag. ~128 entsprechen 1 g.
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

// Akku: Bits 1-7 = %, Bit 0 = vermutlich "lädt" (100 % beobachtet als 0xC8)
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
    local.sin_port = 0;  // freien Port vom Stack holen
    bind(sock_, (sockaddr *)&local, sizeof(local));
    socklen_t slen = sizeof(local);
    getsockname(sock_, (sockaddr *)&local, &slen);
    myPort_ = ntohs(local.sin_port);

    timeval tv = {0, 200 * 1000};
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // START: magic, id 2, type 0x04, unk 1, err 0, length 2, Port (LE), 00 00
    const uint8_t start[16] = {0xEE, 0xFF, 0xEE, 0xFF, 0x02, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00,
                               (uint8_t)(myPort_ & 0xFF), (uint8_t)(myPort_ >> 8), 0x00, 0x00};
    memcpy(start_, start, sizeof(start_));

    discAddr_.sin_family = ctrlAddr_.sin_family = AF_INET;
    discAddr_.sin_addr.s_addr = ctrlAddr_.sin_addr.s_addr = camIp;
    discAddr_.sin_port = htons(DEVINFO_PORT);
    ctrlAddr_.sin_port = htons(VIDEO_CTRL_PORT);

    // Status-Push der Kamera (Akku). Optional: klappt das Binden nicht, bleibt der
    // Akkustand aus der Devinfo-Antwort beim Handshake.
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
    Serial.printf("[i4season] Empfange auf UDP-Port %u\n", myPort_);
  }

  ~I4seasonSession() override {
    if (notify_ >= 0) close(notify_);
    close(sock_);
  }

  void poll(uint8_t *pkt, size_t cap) override {
    int n = recvfrom(sock_, pkt, cap, 0, nullptr, nullptr);

    // Lebenszeichen: START vorsorglich wiederholen, solange Video läuft (0 = aus)
    if (KEEPALIVE_INTERVAL_MS > 0 && haveSeq_ && cameraLinkUp() &&
        millis() - lastStart_ >= KEEPALIVE_INTERVAL_MS) {
      sendto(sock_, start_, sizeof(start_), 0, (sockaddr *)&ctrlAddr_, sizeof(ctrlAddr_));
      lastStart_ = millis();
      stats.keepalives++;
    }

    // Stillstand erkennen, aber nach einem START der Kamera Zeit zum Anlaufen lassen:
    // ein neuer START vor dem Anlaufen startet sie erneut (Kaskade bei 200 ms beobachtet).
    // Erstes Paket nach Leerlauf geht oft verloren -> Handshake wiederholen.
    if (millis() - lastData_ > STALL_TIMEOUT_MS && millis() - lastStart_ >= HANDSHAKE_RETRY_MS) {
      if (haveSeq_) {  // Video lief bis eben
        bool loss = lastLoss_ && millis() - lastLoss_ < STALL_TIMEOUT_MS + 2000;
        if (loss) {
          stats.stallsLoss++;
        } else {
          stats.cleanStallAt[stats.stallsClean % VideoStats::CLEAN_STALL_TIMES] = millis() / 1000;
          stats.stallsClean++;
        }
        crumb("stall: %u ms keine Daten -> Handshake (%s)", STALL_TIMEOUT_MS,
              loss ? "nach Paketverlust" : "ohne Paketverlust");
      }
      haveSeq_ = false;
      if (cameraLinkUp()) {
        // GetDeviceInfo: magic, id 0, type 0x01, unk 1, err 0, length 0
        static const uint8_t discovery[12] = {0xEE, 0xFF, 0xEE, 0xFF, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00};
        sendto(sock_, discovery, sizeof(discovery), 0, (sockaddr *)&discAddr_, sizeof(discAddr_));
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
    if (n >= 12 && memcmp(pkt, MAGIC, 4) == 0) return handleReply(pkt, n);  // Antworten/ACKs
    // Typ 1 = 16-Byte-Kopf, Typ 6 = 28-Byte-Kopf (6-Achsen-Sensor), 5 = Ton, 2 = ?
    size_t hdrLen = pkt[0] == 6 ? 28 : 16;
    if (pkt[0] != 1 && pkt[0] != 6) return;
    if (n <= (int)hdrLen) return;
    lastData_ = millis();

    const uint8_t *payload = pkt + hdrLen;
    size_t plen = n - hdrLen;

    // Lage steht in jedem Paket des Bildes; beim ersten ankommenden Paket eines neuen
    // Bildes (Byte 2) übernehmen, damit ein verlorenes Startpaket keinen Messwert kostet.
    // Nur wenn Kopf-Byte 5 Bit 0 ("hat G-Sensor") gesetzt ist (Soulear: immer, MS5: nie)
    if (!haveSensorFrame_ || pkt[2] != sensorFrame_) {
      if (pkt[5] & 0x01) decodeOrientation(pkt);
      telemetry.width = pkt[12] | (pkt[13] << 8);
      telemetry.height = pkt[14] | (pkt[15] << 8);
      sensorFrame_ = pkt[2];
      haveSensorFrame_ = true;
    }

    // Laufende Paketnummer (Byte 1, 8 Bit) -> verlorene Pakete erkennen
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
      if (inFrame_) {  // vorheriges Bild wurde nie fertig (Ende verloren)
        stats.framesDropped++;
        stats.dropIncomplete++;
      }
      building_ = Frame::create();  // neues Bild beginnt
      inFrame_ = (bool)building_;
      frameBroken_ = false;
      if (!inFrame_) {
        stats.framesDropped++;
        stats.dropNoMem++;
        return;
      }
    } else if (!inFrame_) {
      return;  // mittendrin eingestiegen -> auf nächsten Bildanfang warten
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

    // Endet hier das Bild? (FF D9, eventuell gefolgt von Füll-Nullen)
    size_t end = plen;
    while (end > 0 && payload[end - 1] == 0x00) end--;
    if (end >= 2 && payload[end - 2] == 0xFF && payload[end - 1] == 0xD9) {
      if (frameBroken_ && !SHOW_DAMAGED_FRAMES) {  // kaputtes JPEG nicht weitergeben
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
  // Antworten der Kamera auf unseren Socket: Devinfo (Akku, Produkt) und LED
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
      telemetry.led = p[1] ? 1 : 0;  // Antwort = resultierender Zustand
      int want = ledRequest;
      if (want == telemetry.led) ledRequest.compare_exchange_strong(want, -1);
      crumb("LED bestätigt: %s", p[1] ? "an" : "aus");
    }
  }

  // LED-Wunsch senden und wiederholen, bis die Kamera ihn bestätigt (max. 5x).
  // Payload: op (LED 1 | 0x10 = schreiben), Status 0/1, Helligkeit (am Find T verifiziert)
  void handleLed() {
    int want = ledRequest;
    if (want < 0) {
      ledTries_ = 0;
      return;
    }
    if (!cameraLinkUp() || (ledTries_ && millis() - ledSent_ < 300)) return;
    if (ledTries_ >= 5) {
      crumb("LED: keine Bestätigung");
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

  // Status-Push auf UDP 10007: cmd 0x0009, Payload-Typ 0x02, Payload-Byte 1 = Akku
  void pollNotify() {
    lastNotify_ = millis();
    if (notify_ < 0) return;
    uint8_t buf[64];
    for (int i = 0; i < 4; i++) {
      int m = recv(notify_, buf, sizeof(buf), MSG_DONTWAIT);
      if (m <= 0) return;
      if (m >= 14 && memcmp(buf, MAGIC, 4) == 0 && (buf[6] | (buf[7] << 8)) == CMD_STATUS &&
          buf[12] == 0x02)
        setBattery(buf[13]);
    }
  }

  int sock_ = -1, notify_ = -1;
  uint16_t myPort_ = 0;
  uint8_t start_[16];
  sockaddr_in discAddr_ = {}, ctrlAddr_ = {};
  Frame building_;             // hier wächst das aktuelle JPEG
  bool inFrame_ = false;
  bool frameBroken_ = false;   // im aktuellen Bild fehlt ein Paket
  bool haveSeq_ = false;
  uint8_t nextSeq_ = 0;
  uint8_t sensorFrame_ = 0;    // Bildnummer des zuletzt gelesenen Lagewerts
  bool haveSensorFrame_ = false;
  uint32_t lastData_ = 0;
  uint32_t lastStart_ = 0;     // letzter START (Handshake oder Lebenszeichen)
  uint32_t lastLoss_ = 0;      // letzte Lücke in der Paketnummer
  uint32_t lastNotify_ = 0;
  uint16_t cmdSeq_ = 5;
  uint32_t ledSent_ = 0;
  int ledTries_ = 0;
};

}  // namespace

CamSession *createI4seasonSession(uint32_t camIp) { return new (std::nothrow) I4seasonSession(camIp); }
