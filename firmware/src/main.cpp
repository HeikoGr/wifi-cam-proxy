/*
 * WiFi-Cam-Proxy for ESP32 + LAN8720 (ZB-GW03 v1.4, WT32-ETH01)
 *
 * - Wi-Fi (client): looks for Wi-Fi cameras (otoscopes, microscopes) by SSID and
 *   connects to one of them (src/camera.cpp, protocols in src/cam_*.cpp)
 * - Ethernet (DHCP, src/network.cpp): provides an MJPEG server in the home network
 *   (src/http.cpp)
 *     http://otoskop.local/          browser
 *     http://otoskop.local/stream    VLC / Home Assistant
 *     http://otoskop.local/snapshot  single frame
 *     http://otoskop.local/cameras   choose camera
 *     http://otoskop.local/status    JSON with statistics
 *     http://otoskop.local/update    firmware update in the browser
 *
 * Rescue mode (src/rescue.cpp): if Ethernet has no IP for RESCUE_TIMEOUT_MS, Wi-Fi
 * joins the home Wi-Fi or opens its own access point with a setup page.
 *
 * This file: start, the loop (camera, rescue mode, OTA, statistics), LEDs.
 */

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ETH.h>
#include <WiFi.h>

#include <atomic>

#include "camera.h"
#include "config.h"
#include "cpuload.h"
#include "crashlog.h"
#include "device.h"
#include "rescue.h"
#include "sniffer.h"

#ifndef GIT_REV
#define GIT_REV "unknown"  // set by git_rev.py
#endif
const char FW_VERSION[] = __DATE__ " " __TIME__;
const char FW_COMMIT[] = GIT_REV;

volatile bool rescueMode = false;
std::atomic<bool> updating{false};

// Survives restarts (but not power loss) -> counts resets since power-on
RTC_NOINIT_ATTR static uint32_t bootMagic;
RTC_NOINIT_ATTR static uint32_t bootResets;

uint32_t bootCount() { return bootResets; }

const char *resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_EXT: return "ext";
    default: return "unknown";
  }
}

static float fps = 0;  // written by loop() every 5 s
float currentFps() { return fps; }

// The Zigbee module (ZB-GW03 only) is not needed: hold it in reset, saves power
static void setZigbee(bool on) {
  if (ZIGBEE_NRST_GPIO < 0) return;
  pinMode(ZIGBEE_NRST_GPIO, OUTPUT);
  digitalWrite(ZIGBEE_NRST_GPIO, on ? HIGH : LOW);
}

static void setLed(int pin, bool on) {
  if (pin < 0) return;
  digitalWrite(pin, LED_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH));
}

static void startOta() {
  ArduinoOTA.setHostname(HOSTNAME);  // also starts mDNS -> otoskop.local
  if (strlen(OTA_PASSWORD) > 0) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    updating = true;
    Serial.println("[ota] update starting");
  });
  ArduinoOTA.onError([](ota_error_t) { updating = false; });
  ArduinoOTA.begin();
  IPAddress ip = ethIsUp() ? ETH.localIP() : WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP();
  Serial.printf("[http] viewer: http://%s.local/  or http://%s/\r\n", HOSTNAME,
                ip.toString().c_str());
}

void setup() {
  if (LED_GREEN_GPIO >= 0) pinMode(LED_GREEN_GPIO, OUTPUT);
  if (LED_RED_GPIO >= 0) pinMode(LED_RED_GPIO, OUTPUT);
  setZigbee(false);  // Zigbee is not needed: silence it
  setLed(LED_GREEN_GPIO, true);
  setLed(LED_RED_GPIO, false);

  Serial.begin(115200);
  if (bootMagic != 0xB007B007 || esp_reset_reason() == ESP_RST_POWERON) {
    bootMagic = 0xB007B007;
    bootResets = 0;
  }
  bootResets++;
  crashlogInit();
  Serial.printf("\r\n[boot] WiFi-Cam-Proxy %s (%s), Reset: %s, Boot #%u\r\n", FW_VERSION, FW_COMMIT,
                resetReasonText(), (unsigned)bootResets);

  rescueBegin();
  networkBegin();
  cameraBegin();  // Wi-Fi to the camera and video task
  httpBegin();
}

void loop() {
  static bool otaStarted = false;
  static uint32_t lastStats = 0, lastFrames = 0;
  static uint32_t ethDownSince = 0, ethUpSince = 0;

  // Rescue mode: enter when Ethernet is gone too long; leave via restart once it is
  // stable again
  if (ethIsUp()) {
    ethDownSince = millis();
    if (rescueMode && !updating && millis() - ethUpSince > 10000) {
      Serial.println("[rescue] Ethernet is back -> restarting into normal operation");
      delay(200);
      ESP.restart();
    }
  } else {
    ethUpSince = millis();
    if (!rescueMode && millis() - ethDownSince > RESCUE_TIMEOUT_MS) {
      setLed(LED_RED_GPIO, true);
      rescueEnter();
    }
  }

  rescueLoop();
  if (!otaStarted && (ethIsUp() || (rescueMode && (WiFi.status() == WL_CONNECTED || rescueApActive())))) {
    startOta();
    otaStarted = true;
  }
  if (otaStarted) ArduinoOTA.handle();
  sniffLoop();
  cameraLoop();

  if (millis() - lastStats >= 5000) {
    uint32_t total = stats.framesTotal;
    fps = (total - lastFrames) * 1000.0f / (millis() - lastStats);
    lastFrames = total;
    lastStats = millis();
    cpuLoadUpdate();
    Serial.printf("[stats] %.1f fps, %u frames, %u dropped, %d viewers, heap %u, CPU %d/%d %%%s\r\n",
                  fps, (unsigned)total, (unsigned)stats.framesDropped, streamViewers(),
                  heapFree(), cpuLoad(0), cpuLoad(1), rescueMode ? ", RESCUE MODE" : "");
  }
  delay(10);
}
