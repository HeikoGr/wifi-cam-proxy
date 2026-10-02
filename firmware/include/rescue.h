#pragma once

// Rescue mode: Ethernet has had no IP for RESCUE_TIMEOUT_MS.
//
//   1. Home Wi-Fi known (NVS, else secrets.h) -> connect to it.
//   2. If that fails (RESCUE_STA_TIMEOUT_MS) or none is known -> own access point
//      "WiFi-Cam-XXXX" with captive portal: /wifi-setup scans for networks and
//      stores the home Wi-Fi (like the Arduino WiFiManager).
//   3. When Ethernet comes back, the device restarts (main.cpp).

#include <stddef.h>

void rescueBegin();      // in setup(): load home Wi-Fi from NVS
void rescueEnter();      // Ethernet gone -> rescue mode
void rescueLoop();       // in loop()
bool rescueApActive();   // own access point is running
// Store home Wi-Fi; in rescue mode connect to it right away. Empty SSID = delete
bool rescueSetHome(const char *ssid, const char *pass);
void rescueHomeSsid(char *out, size_t len);
void rescueApSsid(char *out, size_t len);
