#pragma once
#define WIFI_IF_STA 0
#define WIFI_PROTOCOL_11B 1
#define WIFI_PROTOCOL_11G 2
inline int esp_wifi_set_protocol(int, int) { return 0; }
