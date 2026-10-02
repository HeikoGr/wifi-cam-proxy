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

static std::mutex homeMutex;  // home Wi-Fi: HTTP task writes, loop reads
static char homeSsid[33] = "", homePass[65] = "";
static std::atomic<bool> connectPending{false};
static std::atomic<bool> apActive{false};
static DNSServer *dns = nullptr;  // only created in rescue mode
static uint32_t staSince = 0;     // start of the attempt or last connected
static bool staTrying = false;    // home Wi-Fi is being tried (auto-reconnect on)

void rescueBegin() {
  Preferences p;
  if (p.begin("otoskop", true)) {
    strlcpy(homeSsid, p.getString("home_ssid", "").c_str(), sizeof(homeSsid));
    strlcpy(homePass, p.getString("home_pass", "").c_str(), sizeof(homePass));
    p.end();
  }
  if (!*homeSsid) {  // default from secrets.h
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
  Serial.printf("[rescue] connecting to home Wi-Fi \"%s\"\r\n", ssid);
  WiFi.disconnect();
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, *pass ? pass : nullptr);
}

static void startAp() {
  char ssid[33];
  rescueApSsid(ssid, sizeof(ssid));
  // Connection attempts to the home Wi-Fi switch channels and disturb the AP: stop them,
  // rescueLoop() retries at intervals while nobody is connected to the AP
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid, strlen(SETUP_AP_PASSWORD) >= 8 ? SETUP_AP_PASSWORD : nullptr);
  dns = new (std::nothrow) DNSServer;
  if (dns) dns->start(53, "*", WiFi.softAPIP());  // every address -> us (captive portal)
  apActive = true;
  staTrying = false;
  staSince = millis();
  Serial.printf("[rescue] access point \"%s\" on, setup at http://%s/wifi-setup\r\n", ssid,
                WiFi.softAPIP().toString().c_str());
  crumb("setup AP %s on", ssid);
}

void rescueEnter() {
  rescueMode = true;
  esp_wifi_scan_stop();  // in case the camera scan is running
  WiFi.scanDelete();
  char ssid[33];
  rescueHomeSsid(ssid, sizeof(ssid));
  Serial.printf("[rescue] Ethernet without IP -> %s\r\n", *ssid ? "home Wi-Fi" : "own access point");
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
    // home Wi-Fi unreachable: open the AP or (AP already running) stop the attempts
    staTrying = false;
    if (!apActive) {
      startAp();
    } else {
      WiFi.setAutoReconnect(false);
      WiFi.disconnect();
    }
  } else if (apActive && !staTrying && !connected && WiFi.softAPgetStationNum() == 0 &&
             millis() - staSince > RESCUE_STA_RETRY_MS) {
    connectHome();  // maybe the router was just briefly gone
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
  crumb("home Wi-Fi stored: %s", ssid);
  if (rescueMode && *ssid) connectPending = true;
  return ok;
}
