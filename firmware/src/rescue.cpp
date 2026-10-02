#include "rescue.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <atomic>
#include <mutex>

#include "camera.h"
#include "config.h"
#include "crashlog.h"

static std::mutex homeMutex;  // Heim-WLAN: HTTP-Task schreibt, loop liest
static char homeSsid[33] = "", homePass[65] = "";
static std::atomic<bool> connectPending{false};
static std::atomic<bool> apActive{false};
static DNSServer *dns = nullptr;  // nur im Notfall-Modus angelegt
static uint32_t staSince = 0;     // Beginn des Versuchs bzw. zuletzt verbunden
static bool staTrying = false;    // Heim-WLAN wird gerade versucht (Auto-Reconnect an)

void rescueBegin() {
  Preferences p;
  if (p.begin("otoskop", true)) {
    strlcpy(homeSsid, p.getString("home_ssid", "").c_str(), sizeof(homeSsid));
    strlcpy(homePass, p.getString("home_pass", "").c_str(), sizeof(homePass));
    p.end();
  }
  if (!*homeSsid) {  // Vorgabe aus secrets.h
    strlcpy(homeSsid, HOME_WIFI_SSID, sizeof(homeSsid));
    strlcpy(homePass, HOME_WIFI_PASSWORD, sizeof(homePass));
  }
}

bool rescueApActive() { return apActive; }

void rescueHomeSsid(char *out, size_t len) {
  std::lock_guard<std::mutex> lock(homeMutex);
  strlcpy(out, homeSsid, len);
}

void rescueApSsid(char *out, size_t len) {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(out, len, "%s%02X%02X", SETUP_AP_PREFIX, mac[4], mac[5]);
}

static void connectHome() {
  char ssid[33], pass[65];
  {
    std::lock_guard<std::mutex> lock(homeMutex);
    strlcpy(ssid, homeSsid, sizeof(ssid));
    strlcpy(pass, homePass, sizeof(pass));
  }
  staSince = millis();
  staTrying = *ssid;
  if (!*ssid) return;
  Serial.printf("[rescue] verbinde mit Heim-WLAN \"%s\"\n", ssid);
  WiFi.disconnect();
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, *pass ? pass : nullptr);
}

static void startAp() {
  char ssid[33];
  rescueApSsid(ssid, sizeof(ssid));
  // Verbindungsversuche ins Heim-WLAN wechseln den Kanal und stören den AP: anhalten,
  // rescueLoop() versucht es in Abständen erneut, solange niemand am AP hängt
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid, strlen(SETUP_AP_PASSWORD) >= 8 ? SETUP_AP_PASSWORD : nullptr);
  dns = new (std::nothrow) DNSServer;
  if (dns) dns->start(53, "*", WiFi.softAPIP());  // jede Adresse -> wir (Captive Portal)
  apActive = true;
  staTrying = false;
  staSince = millis();
  Serial.printf("[rescue] Access Point \"%s\" an, Einrichtung unter http://%s/wifi-setup\n", ssid,
                WiFi.softAPIP().toString().c_str());
  crumb("Setup-AP %s an", ssid);
}

void rescueEnter() {
  rescueMode = true;
  esp_wifi_scan_stop();  // falls die Kamerasuche gerade läuft
  WiFi.scanDelete();
  char ssid[33];
  rescueHomeSsid(ssid, sizeof(ssid));
  Serial.printf("[rescue] Ethernet ohne IP -> %s\n", *ssid ? "Heim-WLAN" : "eigener Access Point");
  if (*ssid) connectHome();
  else startAp();
}

void rescueLoop() {
  if (!rescueMode) return;
  if (dns) dns->processNextRequest();
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) staSince = millis();
  if (connectPending.exchange(false)) {
    connectHome();
  } else if (staTrying && !connected && millis() - staSince > RESCUE_STA_TIMEOUT_MS) {
    // Heim-WLAN nicht erreichbar: AP öffnen bzw. (AP läuft schon) Versuche anhalten
    staTrying = false;
    if (!apActive) {
      startAp();
    } else {
      WiFi.setAutoReconnect(false);
      WiFi.disconnect();
    }
  } else if (apActive && !staTrying && !connected && WiFi.softAPgetStationNum() == 0 &&
             millis() - staSince > RESCUE_STA_RETRY_MS) {
    connectHome();  // vielleicht war nur der Router kurz weg
  }
}

bool rescueSetHome(const char *ssid, const char *pass) {
  if (strlen(ssid) > 32 || strlen(pass) > 64 || (*pass && strlen(pass) < 8)) return false;
  {
    std::lock_guard<std::mutex> lock(homeMutex);
    strlcpy(homeSsid, ssid, sizeof(homeSsid));
    strlcpy(homePass, pass, sizeof(homePass));
  }
  Preferences p;
  bool ok = p.begin("otoskop", false);
  if (ok) {
    p.putString("home_ssid", ssid);
    p.putString("home_pass", pass);
    p.end();
  }
  crumb("Heim-WLAN gespeichert: %s", ssid);
  if (rescueMode && *ssid) connectPending = true;
  return ok;
}
