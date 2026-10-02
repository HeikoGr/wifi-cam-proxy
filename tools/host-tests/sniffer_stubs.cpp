// Stand-ins for the parts of the firmware that sniffer.cpp calls
#include "camera.h"
#include "crashlog.h"
#include <esp_wifi.h>
wifi_promiscuous_cb_t g_cb;
void cameraPause(bool) {}
void crumb(const char *fmt, ...) {}
void wifiApplyMode() {}
const CamProtocol PROTOCOL_JHCMD = {CamProto::Jhcmd, "jhcmd", "", ipv4(192, 168, 29, 1), nullptr, nullptr};
