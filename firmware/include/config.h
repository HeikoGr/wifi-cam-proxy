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

// --- ZB-GW03 v1.4: LAN8720-Anbindung (laut syssi/esphome-zb-gw03) ---------------
#define ETH_PHY_ADDR_GW    1
#define ETH_MDC_GPIO       23
#define ETH_MDIO_GPIO      18
#define ETH_POWER_GPIO     16
#define ETH_CLK_MODE_GW    ETH_CLOCK_GPIO17_OUT
#ifndef ETH_TX_STORE_FORWARD
#define ETH_TX_STORE_FORWARD 1          // Ethernet sendet erst mit vollständigem Paket, siehe main.cpp
#endif
#define ETH_10MBIT_DEFAULT true         // Ethernet nur mit 10 Mbit: WLAN stört sonst den Takt (siehe main.cpp)

// --- LEDs (beide invertiert: LOW = an) ------------------------------------------
#define LED_GREEN_GPIO     14                // an = Firmware läuft
#define LED_RED_GPIO       15                // an = Notfall-Modus

// Zigbee-Modul (EFR32): nRST an GPIO13, LOW = im Reset (laut syssi/esphome-zb-gw03)
#define ZIGBEE_NRST_GPIO   13

// --- Video ----------------------------------------------------------------------
#define CHUNK_HEADER_LEN   16                // 16-Byte-Kopf vor jedem JPEG-Stück
#define MAX_FRAME_BYTES    (48 * 1024)       // größere Bilder werden verworfen (gemessen bis ~41 KB)
#define STALL_TIMEOUT_MS   200               // so lange ohne Daten -> Handshake erneut (normal: alle ~58 ms ein Bild)
#define HANDSHAKE_RETRY_MS 800               // frühestens so lange nach einem START den nächsten
// Bilder mit verlorenen Paketen trotzdem anzeigen (1) oder verwerfen (0).
// 1: kurz fehlerhafte Streifen statt Stocken; 0: nur saubere Bilder
#define SHOW_DAMAGED_FRAMES 1
// WLAN-Modus zum Otoskop, falls im NVS nichts gespeichert ist ("bgn", "bg", "b").
// Umschalten zur Laufzeit auf der Update-Seite.
#define WIFI_MODE_DEFAULT  "bg"
// WLAN-Sendeleistung in 0,25 dBm, falls im NVS nichts steht (8..84; 44 = 11 dBm).
// Hat keinen Einfluss auf die Ethernet-Verluste (gemessen); 2 dBm reichten am Tisch.
// Umschalten zur Laufzeit: POST /wifi/tx/<Wert>
#define WIFI_TX_QDBM_DEFAULT 44
// START alle x ms wiederholen, solange Video läuft (0 = aus). Test gegen kurze
// Stillstände des Otoskops ohne Paketverlust (~alle 20 s beobachtet)
#define KEEPALIVE_INTERVAL_MS 0     // getestet: half nicht gegen die Pausen alle ~25 s

// --- Versuchsschalter (Stottern auf der LAN-Seite eingrenzen) -------------------
#ifndef USE_IRAM_CHUNKS
#define USE_IRAM_CHUNKS 1       // Bilddaten im IRAM-Rest ablegen
#endif
#ifndef SEND_BLOCK_SEGMENTS
#define SEND_BLOCK_SEGMENTS 4   // TCP-Segmente pro send() (2 brachte keine Besserung)
#endif
