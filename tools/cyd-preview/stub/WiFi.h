#pragma once
#include <esp_wifi.h>
#include <string>
struct WiFiStub {
  int RSSI() { return -50; }
  std::string SSID() { return ""; }
};
static WiFiStub WiFi;
enum arduino_event_id_t { ARDUINO_EVENT_WIFI_STA_GOT_IP, ARDUINO_EVENT_WIFI_STA_DISCONNECTED };
struct arduino_event_info_t {
  struct { int reason; } wifi_sta_disconnected;
};
struct NetworkStub {
  void onEvent(void (*)(arduino_event_id_t, arduino_event_info_t)) {}
};
static NetworkStub Network;
