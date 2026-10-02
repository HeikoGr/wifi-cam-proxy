#pragma once

// Heim-WLAN für den Notfall-Modus steht in secrets.h (Vorlage: secrets.example.h)
#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "include/secrets.h fehlt - Notfall-Modus ist deaktiviert"
#define HOME_WIFI_SSID     ""
#define HOME_WIFI_PASSWORD ""
#endif

// --- Otoskop (WLAN) -------------------------------------------------------------
#define OTOSCOPE_SSID      "Soulear-6b1c9"   // offenes Netz, kein Passwort
#define OTOSCOPE_IP        "192.168.1.1"
#define DISCOVERY_PORT     10005             // GetDeviceInfo, muss vor START kommen
#define VIDEO_CTRL_PORT    10006             // START / OpenVideo

// --- Heimnetz (Ethernet, DHCP) --------------------------------------------------
#define HOSTNAME           "otoskop"         // -> http://otoskop.local
#define HTTP_PORT          80
#define MAX_STREAM_CLIENTS 3                 // je Zuschauer ~2,5-3,5 Mbit/s; bei 10 Mbit Ethernet ist 3 das Maximum
#ifndef OTA_PASSWORD
#define OTA_PASSWORD       ""                // leer = Updates ohne Passwort; setzen in secrets.h
#endif

// --- Notfall-Modus --------------------------------------------------------------
// Hat Ethernet so lange keine IP, wechselt das WLAN vom Otoskop ins Heim-WLAN,
// damit Weboberfläche und OTA erreichbar bleiben. Kommt Ethernet zurück, startet
// das Gerät neu und läuft wieder normal.
#define RESCUE_TIMEOUT_MS  (30 * 1000)

// ================================================================================
// Board-spezifische Hardware-Konfiguration
// Board wird per build_flags in platformio.ini gesetzt:
//   -DBOARD_ZB_GW03      (Standard)
//   -DBOARD_WT32_ETH01
// ================================================================================

#if defined(BOARD_WT32_ETH01)
// --- WT32-ETH01 (ESP32 + LAN8720 mit eigenem 50-MHz-Quarz auf GPIO0) ------------
// Vorteil gegenüber ZB-GW03: Externer Quarz ist unabhängig vom WLAN-Empfang,
// daher kein 10-Mbit-Limit nötig. Bis zu 100 Mbit auch bei gleichzeitigem WLAN.
// Quelle: https://github.com/egnor/wt32-eth01
#define ETH_PHY_ADDR_GW    1
#define ETH_MDC_GPIO       23
#define ETH_MDIO_GPIO      18
#define ETH_POWER_GPIO     16               // -1 bei manchen Varianten; 16 für v1.4
#define ETH_CLK_MODE_GW    ETH_CLOCK_GPIO0_IN  // externer 50-MHz-Quarz
#define LED_GREEN_GPIO     2               // blaue LED am Board, active HIGH
#define LED_RED_GPIO       -1              // kein rotes LED vorhanden -> deaktiviert
#define ZIGBEE_NRST_GPIO   -1              // kein Zigbee-Modul
#define ETH_10MBIT_DEFAULT false           // 100 Mbit möglich, da externer Takt

#else
// --- ZB-GW03 v1.4 (ESP32 + LAN8720, eigentlich Zigbee-Gateway) -----------------
// ESP32 erzeugt den 50-MHz-Takt für LAN8720 selbst (GPIO17). WLAN-Empfang stört
// ihn bei 100 Mbit. Lösung: Ethernet auf 10 Mbit festsetzen.
// Quelle: https://github.com/syssi/esphome-zb-gw03
#define ETH_PHY_ADDR_GW    1
#define ETH_MDC_GPIO       23
#define ETH_MDIO_GPIO      18
#define ETH_POWER_GPIO     16
#define ETH_CLK_MODE_GW    ETH_CLOCK_GPIO17_OUT
#define LED_GREEN_GPIO     14              // an = Firmware läuft (active LOW)
#define LED_RED_GPIO       15              // an = Notfall-Modus (active LOW)
#define ZIGBEE_NRST_GPIO   13             // nRST des EFR32, LOW = im Reset
#define ETH_10MBIT_DEFAULT true

#endif  // Board-Auswahl

// LED-Polarität: ZB-GW03 = invertiert (LOW = an), WT32-ETH01 = normal (HIGH = an)
#if defined(BOARD_WT32_ETH01)
#define LED_ACTIVE_HIGH    1
#else
#define LED_ACTIVE_HIGH    0
#endif

#ifndef ETH_TX_STORE_FORWARD
#define ETH_TX_STORE_FORWARD 1
#endif

// --- Video ----------------------------------------------------------------------
#define CHUNK_HEADER_LEN   16                // 16-Byte-Kopf vor jedem JPEG-Stück
#define MAX_FRAME_BYTES    (48 * 1024)       // größere Bilder werden verworfen (gemessen bis ~41 KB)
#define STALL_TIMEOUT_MS   200               // so lange ohne Daten -> Handshake erneut (normal: alle ~58 ms ein Bild)
#define HANDSHAKE_RETRY_MS 800               // frühestens so lange nach einem START den nächsten
// Bilder mit verlorenen Paketen trotzdem anzeigen (1) oder verwerfen (0).
#define SHOW_DAMAGED_FRAMES 1
// WLAN-Modus zum Otoskop, falls im NVS nichts gespeichert ist ("bgn", "bg", "b").
#define WIFI_MODE_DEFAULT  "bg"
// WLAN-Sendeleistung in 0,25 dBm, falls im NVS nichts steht (8..84; 44 = 11 dBm).
#define WIFI_TX_QDBM_DEFAULT 44
// START alle x ms wiederholen, solange Video läuft (0 = aus).
#define KEEPALIVE_INTERVAL_MS 0

// --- Versuchsschalter -----------------------------------------------------------
#ifndef USE_IRAM_CHUNKS
#define USE_IRAM_CHUNKS 1
#endif
#ifndef SEND_BLOCK_SEGMENTS
#define SEND_BLOCK_SEGMENTS 4
#endif
