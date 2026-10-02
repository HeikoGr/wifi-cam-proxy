#pragma once

// Ethernet bridge (ZB-GW03, WT32-ETH01): what main.cpp, network.cpp and http.cpp share.
// Not used on the CYD.

#include <Arduino.h>

// --- Firmware and boot (main.cpp) -------------------------------------------------
extern const char FW_VERSION[];  // build date and time
extern const char FW_COMMIT[];   // git describe of the build (git_rev.py)
const char *resetReasonText();
uint32_t bootCount();            // resets since power-on
float currentFps();              // frames per second received, over the last 5 s

// --- Network (network.cpp) --------------------------------------------------------
void networkBegin();  // in setup(): Wi-Fi/Ethernet settings from NVS, network events, Ethernet
bool ethIsUp();       // Ethernet has an IP

struct EthStatus {
  bool beginOk;      // ETH.begin() succeeded
  bool started;      // LAN8720 initialised
  bool link;
  bool fullDuplex;
  int speed;         // Mbit/s, 0 without link
  int storeForward;  // EMAC "store and forward" (1/0), -1 = not started
  char ip[16];       // "" without IP
};
void ethStatus(EthStatus &out);
bool eth10Mbit();               // Ethernet 10 Mbit only (NVS eth10)
void ethSet10Mbit(bool on);     // store and renegotiate (link briefly down)

// Wi-Fi towards the camera; wifiApplyMode() (camera.h) applies mode and power
const char *wifiModeName();          // "bgn", "bg", "b"
bool wifiSetMode(const char *name);  // store; takes effect on the next connection
bool wifiSetTxPower(int qdbm);       // 8..84 in 0.25 dBm, store and apply
int wifiTxPower();                   // the set value in 0.25 dBm
float wifiTxDbm();                   // current transmit power

// --- HTTP server (http.cpp) -------------------------------------------------------
void httpBegin();      // in setup(): calibration from NVS, start the server task
int streamViewers();   // open /stream connections
