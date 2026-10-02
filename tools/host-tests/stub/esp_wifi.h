#pragma once
#include <stdint.h>
typedef enum { WIFI_PKT_MGMT, WIFI_PKT_CTRL, WIFI_PKT_DATA, WIFI_PKT_MISC } wifi_promiscuous_pkt_type_t;
typedef struct { unsigned sig_len:12; } wifi_pkt_rx_ctrl_t;
typedef struct { wifi_pkt_rx_ctrl_t rx_ctrl; uint8_t payload[0]; } wifi_promiscuous_pkt_t;
typedef struct { uint32_t filter_mask; } wifi_promiscuous_filter_t;
#define WIFI_PROMIS_FILTER_MASK_DATA 4
typedef enum { WIFI_SECOND_CHAN_NONE = 0, WIFI_SECOND_CHAN_ABOVE, WIFI_SECOND_CHAN_BELOW } wifi_second_chan_t;
#define WIFI_BW_HT40 2
typedef struct { uint8_t ssid[33]; wifi_second_chan_t second; unsigned phy_11b:1, phy_11g:1, phy_11n:1; } wifi_ap_record_t;
#define WIFI_IF_STA 0
#define WIFI_PROTOCOL_11B 1
#define WIFI_PROTOCOL_11G 2
#define WIFI_PROTOCOL_11N 4
#define WIFI_BW_HT20 1
inline int esp_wifi_set_protocol(int, int) { return 0; }
inline int esp_wifi_set_bandwidth(int, int) { return 0; }
typedef void (*wifi_promiscuous_cb_t)(void *buf, wifi_promiscuous_pkt_type_t type);
extern wifi_promiscuous_cb_t g_cb;
inline int esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *) { return 0; }
inline int esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb_t cb) { g_cb = cb; return 0; }
inline int esp_wifi_set_promiscuous(bool) { return 0; }
inline int esp_wifi_set_channel(int, wifi_second_chan_t) { return 0; }
