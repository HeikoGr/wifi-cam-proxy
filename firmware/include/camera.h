#pragma once

// Camera layer: detection (Wi-Fi scan for SSID patterns), connection, protocols.
//
// Memory: the code of all protocols lives in flash and is executed from there (XIP
// via the cache), so it costs no RAM. Only buffers and state occupy RAM, and they
// exist only for the currently connected camera: the session of the active protocol
// is created with new on connect and deleted again on change. Loading code from
// flash on demand is therefore not necessary.

#include <Arduino.h>

#include <atomic>

#include "config.h"
#include "frame.h"

// --- Protocols ------------------------------------------------------------------
enum class CamProto : uint8_t {
  None = 0,
  I4season,  // Soulear/Hopefox otoscopes, MS5 microscopes (UDP 10005/10006)
  Jhcmd,     // MaxSee/JoyHonest/MAX-VIEW microscopes (UDP 20000/10900, "JHCMD")
  Auto = 0xFF,
};
const char *protoKey(CamProto p);   // "i4season", "jhcmd", "auto", ""
const char *protoName(CamProto p);  // for the web UI
CamProto protoFromKey(const char *key);
// Recognises the camera family by its SSID (table in camera.cpp), None = unknown
CamProto protoForSsid(const char *ssid);
// Protocols the user can choose (web UI /cameras, CYD camera choice), "automatic"
// first. A new protocol only has to be added here (and in protoKey/protoName).
extern const CamProto PROTO_CHOICES[];
extern const int PROTO_CHOICE_COUNT;

// A connected camera. Runs entirely in the video task.
class CamSession {
 public:
  virtual ~CamSession() = default;
  // Receives at most one packet, returns after ~200 ms at the latest
  virtual void poll(uint8_t *pkt, size_t cap) = 0;
};
CamSession *createI4seasonSession(uint32_t camIp);
CamSession *createJhcmdSession(uint32_t camIp);

// --- State shared by protocols and web UI ---------------------------------------
struct VideoStats {
  std::atomic<uint32_t> framesTotal{0};
  std::atomic<uint32_t> framesDropped{0};   // sum of the next three
  std::atomic<uint32_t> dropNoMem{0};       // no heap for the finished frame
  std::atomic<uint32_t> dropTooBig{0};      // larger than MAX_FRAME_BYTES
  std::atomic<uint32_t> dropIncomplete{0};  // packet(s) of the frame lost
  std::atomic<uint32_t> packetsLost{0};     // gaps in the sequence number
  std::atomic<uint32_t> framesDamaged{0};   // shown despite packet loss
  std::atomic<uint32_t> maxFrameBytes{0};
  std::atomic<uint32_t> handshakes{0};
  std::atomic<uint32_t> keepalives{0};
  // Stalls: with packet loss in the 2 s before (radio) or without (camera pauses on
  // its own). For the clean ones the last points in time (s since start).
  std::atomic<uint32_t> stallsLoss{0};
  std::atomic<uint32_t> stallsClean{0};
  static const int CLEAN_STALL_TIMES = 8;
  uint32_t cleanStallAt[CLEAN_STALL_TIMES];
};
extern VideoStats stats;

// Camera telemetry, -1 = unknown
struct CamTelemetry {
  std::atomic<bool> hasOrientation{false};  // orientation sensor delivers values
  std::atomic<int16_t> accX{0}, accY{0}, accZ{0};
  std::atomic<uint32_t> accSeq{0};
  std::atomic<int8_t> battery{-1};          // battery in %
  std::atomic<int8_t> charging{-1};         // 1 = charging (meaning uncertain)
  std::atomic<int8_t> led{-1};              // last state confirmed by the camera
  std::atomic<bool> ledSupported{false};
  std::atomic<bool> ledDimmable{false};     // brightness adjustable (JHCMD/MAX-VIEW)
  std::atomic<uint16_t> width{0}, height{0};  // per video header (Soulear wrongly reports 640x480)
  char vendor[33] = "", product[33] = "", firmware[17] = "";  // guarded by infoMux
  void reset();
};
extern CamTelemetry telemetry;
extern portMUX_TYPE infoMux;

// LED request from the web UI (-1 = none, 0/1); the session sends it
extern std::atomic<int> ledRequest;
// Brightness in % used when the LED is on (dimmable cameras only), default 100
extern std::atomic<int> ledLevel;

// From main.cpp
extern volatile bool rescueMode;
extern std::atomic<bool> updating;
// May the session send commands to the camera right now?
bool cameraLinkUp();

// --- Camera management (camera.cpp) -----------------------------------------------
void cameraBegin();  // in setup(): load NVS, start video task, first connection
void cameraLoop();   // in loop(): scan, selection, reconnect
void cameraOnWifiGotIp();
void cameraRestartWifi();  // reconnect after the Wi-Fi mode changed
// Selection from the web UI. Empty SSID = clear the preference, automatic again
bool cameraSelect(const char *ssid, const char *pass, CamProto proto);
void cameraRequestScan();
// Automatic scan (NVS, default on). Off: no scans of its own, only reconnect to the
// remembered camera; scanning only on request (cameraRequestScan).
bool cameraAutoScan();
void cameraSetAutoScan(bool on);
// Pause: end the session, leave the camera Wi-Fi and do nothing until resumed (for the
// sniffer, so the vendor app can connect). Resume reconnects to the current camera.
void cameraPause(bool on);
bool cameraPaused();
// For devices without a web UI (CYD): query state and scan list directly
struct ScanEntry {
  char ssid[33];
  int8_t rssi;
  bool open;
  CamProto proto;
};
const char *cameraStateKey();  // "connected", "connecting", "scanning", "choose", "searching", "restart"
int cameraNetworks(ScanEntry *out, int max);
void cameraCurrentSsid(char *out, size_t len);
// Diagnostics of the current session (first packets as hex, see cam_i4season.cpp),
// also served as /camdiag. diagLog() appends a line, diagReset() starts over.
void diagLog(const char *fmt, ...);
void diagReset();
void diagCopy(char *out, size_t len);
// Raw capture of one whole frame (all UDP packets including headers, unparsed), for
// /camdiag/raw: the HTTP side requests it, the session fills it once and hands it over.
// Experiments: send raw bytes to the camera from the session's sockets
// (/camdiag/send/<port>/<hex>); the session takes the request in its poll loop
void diagSendPut(uint16_t port, const uint8_t *data, size_t len);
bool diagSendTake(uint16_t &port, uint8_t *data, size_t &len);  // len: in = capacity
void diagRawRequest();
bool diagRawWanted();
void diagRawPut(const Frame &f);
bool diagRawTake(Frame &out);  // hands the capture over and frees the slot
// Write the JSON for /cameras into out, returns the length
size_t cameraJson(char *out, size_t len);
CamProto cameraProto();            // protocol of the active session
