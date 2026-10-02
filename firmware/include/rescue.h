#pragma once

// Notfall-Modus: Ethernet hat RESCUE_TIMEOUT_MS lang keine IP.
//
//   1. Heim-WLAN bekannt (NVS, sonst secrets.h) -> damit verbinden.
//   2. Klappt das nicht (RESCUE_STA_TIMEOUT_MS) oder ist keins bekannt -> eigener
//      Access Point "WiFi-Cam-XXXX" mit Captive Portal: /wifi-setup sucht Netze und
//      speichert das Heim-WLAN (wie der Arduino-WiFiManager).
//   3. Kommt Ethernet zurück, startet das Gerät neu (main.cpp).

#include <stddef.h>

void rescueBegin();      // in setup(): Heim-WLAN aus NVS laden
void rescueEnter();      // Ethernet weg -> Notfall-Modus
void rescueLoop();       // in loop()
bool rescueApActive();   // eigener Access Point läuft
// Heim-WLAN speichern; im Notfall-Modus sofort damit verbinden. Leere SSID = löschen
bool rescueSetHome(const char *ssid, const char *pass);
void rescueHomeSsid(char *out, size_t len);
void rescueApSsid(char *out, size_t len);
