/*
 * Network of the Ethernet bridge: Ethernet (LAN8720, DHCP) towards the home network,
 * Wi-Fi settings towards the camera, and the network events of both.
 */

#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <soc/emac_dma_struct.h>

#include <atomic>

#include "camera.h"
#include "config.h"
#include "crashlog.h"
#include "device.h"
#include "settings.h"

static volatile bool ethUp = false;
static volatile bool ethStarted = false;  // LAN8720 initialised
static bool ethBeginOk = false;
static std::atomic<bool> eth10{ETH_10MBIT_DEFAULT};  // Ethernet 10 Mbit only, see ethApplySpeed()

// --- Wi-Fi mode towards the camera (switchable on the update page, in NVS) ---------
// "bgn": 802.11n with packet aggregation (fast, but one missing sub-packet holds up
//        the whole block); "bg": without 11n, every packet on its own (default);
// "b":   802.11b only, slow (max. 11 Mbit/s), but most robust with a weak signal
static const char *const WIFI_MODES[] = {"bgn", "bg", "b"};
static int wifiModeIndex(const char *m) {
  for (size_t i = 0; i < sizeof(WIFI_MODES) / sizeof(WIFI_MODES[0]); i++)
    if (!strcmp(m, WIFI_MODES[i])) return i;
  return -1;
}
static std::atomic<int> wifiMode{wifiModeIndex(WIFI_MODE_DEFAULT)};
// Wi-Fi transmit power in 0.25 dBm (8..84). High power disturbs the Ethernet clock the
// ESP32 generates itself on GPIO17 -> lost Ethernet packets (measured on the device)
static std::atomic<int> wifiTxQdbm{WIFI_TX_QDBM_DEFAULT};

void wifiApplyMode() {
  int mode = wifiMode;
  uint8_t proto = mode == 2   ? WIFI_PROTOCOL_11B
                  : mode == 1 ? (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G)
                              : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  esp_wifi_set_protocol(WIFI_IF_STA, proto);
  if (mode == 0) esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);  // 20 instead of 40 MHz
  esp_wifi_set_max_tx_power(wifiTxQdbm);
}

const char *wifiModeName() { return WIFI_MODES[wifiMode]; }

bool wifiSetMode(const char *name) {
  int i = wifiModeIndex(name);
  if (i < 0) return false;
  nvsWrite([&](Preferences &p) { p.putString("wifimode", name); });
  wifiMode = i;
  crumb("Wi-Fi mode -> %s, reconnecting", name);
  // The protocol change only takes effect on a new connection
  if (!rescueMode) cameraRestartWifi();  // in rescue mode only at the next normal operation
  return true;
}

bool wifiSetTxPower(int qdbm) {
  if (qdbm < 8 || qdbm > 84) return false;
  nvsWrite([&](Preferences &p) { p.putInt("wifitx", qdbm); });
  wifiTxQdbm = qdbm;
  esp_wifi_set_max_tx_power(qdbm);
  crumb("Wi-Fi transmit power -> %.2f dBm", qdbm / 4.0);
  return true;
}

float wifiTxDbm() {
  int8_t q = 0;
  return esp_wifi_get_max_tx_power(&q) == ESP_OK ? q / 4.0f : 0;
}

static void loadSettings() {
  nvsRead([](Preferences &p) {
    int i = wifiModeIndex(p.getString("wifimode", WIFI_MODE_DEFAULT).c_str());
    int tx = p.getInt("wifitx", WIFI_TX_QDBM_DEFAULT);
    eth10 = p.getBool("eth10", ETH_10MBIT_DEFAULT);
    if (i >= 0) wifiMode = i;
    if (tx >= 8 && tx <= 84) wifiTxQdbm = tx;
  });
}

// --- Ethernet: transmit only with a complete packet in the FIFO --------------------
// ESP-IDF starts transmitting as soon as 64 bytes are in the TX FIFO. If the Wi-Fi DMA
// blocks the memory bus meanwhile, the FIFO runs empty and the packet goes out mangled;
// the switch drops it, TCP retransmits only after ~1 s (measured: only with Wi-Fi RX).
// "Store and forward" transmits only once the whole packet is in the FIFO (2 KB = 1 packet).
static void ethStoreForward() {
  if (!ETH_TX_STORE_FORWARD || EMAC_DMA.dmaoperation_mode.tx_str_fwd) return;
  EMAC_DMA.dmaoperation_mode.start_stop_transmission_command = 0;  // stop transmitting
  delay(2);
  EMAC_DMA.dmaoperation_mode.tx_str_fwd = 1;
  EMAC_DMA.dmaoperation_mode.start_stop_transmission_command = 1;
  crumb("eth: store and forward on");
}

// --- Ethernet at 10 Mbit ------------------------------------------------------------
// The ESP32 generates the 50 MHz clock for the LAN8720 itself (GPIO17). Wi-Fi reception
// disturbs it; at 100 Mbit ~2-3 % of the Ethernet packets are then lost (measured,
// independent of Wi-Fi transmit power, buffers and Zigbee). At 10 Mbit every bit is
// held for 10 clock cycles and is immune. Implemented via auto-negotiation (only offer
// "10 Mbit full duplex") so the switch does not fall back to half duplex.

static bool phyRead(uint32_t reg, uint32_t &val) {
  esp_eth_phy_reg_rw_data_t rw = {reg, &val};
  return esp_eth_ioctl(ETH.handle(), ETH_CMD_READ_PHY_REG, &rw) == ESP_OK;
}
static bool phyWrite(uint32_t reg, uint32_t val) {
  esp_eth_phy_reg_rw_data_t rw = {reg, &val};
  return esp_eth_ioctl(ETH.handle(), ETH_CMD_WRITE_PHY_REG, &rw) == ESP_OK;
}

static void ethApplySpeed() {
  const uint32_t ANAR = 4, BMCR = 0;
  uint32_t anar = 0, bmcr = 0;
  if (!phyRead(ANAR, anar) || !phyRead(BMCR, bmcr)) return;
  // bits 5-8: 10HD, 10FD, 100HD, 100FD; pause bits (10/11) and selector stay
  uint32_t want = (anar & ~0x01E0u) | (eth10 ? 0x0040u : 0x01E0u);
  if (want == anar) return;  // already negotiated like this -> no endless loop
  phyWrite(ANAR, want);
  phyWrite(BMCR, bmcr | 0x1000 | 0x0200);  // enable + restart auto-negotiation
  crumb("eth: offering %s, renegotiating", eth10 ? "10 Mbit only" : "100 Mbit");
}

bool eth10Mbit() { return eth10; }

void ethSet10Mbit(bool on) {
  nvsWrite([&](Preferences &p) { p.putBool("eth10", on); });
  eth10 = on;
  crumb("eth10 -> %d", on);
  ethApplySpeed();
}

bool ethIsUp() { return ethUp; }

void ethStatus(EthStatus &out) {
  out.beginOk = ethBeginOk;
  out.started = ethStarted;
  out.link = ethStarted && ETH.linkUp();
  out.fullDuplex = out.link && ETH.fullDuplex();
  out.speed = out.link ? (int)ETH.linkSpeed() : 0;
  out.storeForward = ethStarted ? (int)EMAC_DMA.dmaoperation_mode.tx_str_fwd : -1;
  strlcpy(out.ip, ethUp ? ETH.localIP().toString().c_str() : "", sizeof(out.ip));
}

// --- Network events -----------------------------------------------------------------
static void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      ETH.setHostname(HOSTNAME);
      ethStarted = true;
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      ethStoreForward();
      ethApplySpeed();
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.printf("[eth] IP %s\r\n", ETH.localIP().toString().c_str());
      crumb("eth IP %s, %d Mbit %s", ETH.localIP().toString().c_str(), (int)ETH.linkSpeed(),
            ETH.fullDuplex() ? "full duplex" : "HALF DUPLEX");
      ethUp = true;
      ETH.setDefault();  // default route into the home network, not to the camera
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("[eth] disconnected");
      crumb("eth disconnected (event %d)", (int)event);
      ethUp = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      cameraOnWifiGotIp();
      crumb("wifi connected %s, RSSI %d", WiFi.SSID().c_str(), WiFi.RSSI());
      Serial.printf("[wifi] connected to %s, IP %s, RSSI %d dBm\r\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (ethUp) ETH.setDefault();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[wifi] disconnected (reason %u)\r\n", info.wifi_sta_disconnected.reason);
      crumb("wifi disconnected, reason %u", info.wifi_sta_disconnected.reason);
      break;
    default:
      break;
  }
}

void networkBegin() {
  loadSettings();
  Network.onEvent(onNetworkEvent);
  ethBeginOk = ETH.begin(ETH_PHY_LAN8720, ETH_PHY_ADDR_GW, ETH_MDC_GPIO, ETH_MDIO_GPIO, ETH_POWER_GPIO,
                         ETH_CLK_MODE_GW);
  Serial.printf("[eth] begin %s\r\n", ethBeginOk ? "ok" : "FAILED");
}
