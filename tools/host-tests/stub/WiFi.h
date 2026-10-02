#pragma once
#include <esp_wifi.h>
#define WL_CONNECTED 3
#include <string>
struct StrStub : std::string { using std::string::string; bool operator!=(const char *o) const { return compare(o) != 0; } };
using String = StrStub;
struct WiFiStub { int status() { return 3; } int channel() { return 9; } StrStub SSID() { return "MAXVIEW-7762"; }
  int scanNetworks(bool, bool, bool, int, int, const char *) { return 1; }
  void *getScanInfoByIndex(int) { static wifi_ap_record_t r = {"MAXVIEW-7762", WIFI_SECOND_CHAN_BELOW, 1, 1, 1}; return &r; }
  void scanDelete() {} };
static WiFiStub WiFi;
