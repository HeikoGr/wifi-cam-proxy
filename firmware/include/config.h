#pragma once

// Home Wi-Fi for rescue mode is in secrets.h (template: secrets.example.h)
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define HOME_WIFI_SSID     ""
#define HOME_WIFI_PASSWORD ""
#endif

// --- Cameras (Wi-Fi) ------------------------------------------------------------
// The camera is recognised by its SSID in a Wi-Fi scan (patterns in src/camera.cpp)
// and selected in the web UI under /cameras. The device remembers the last connected
// one in NVS and connects to it directly at startup.
#define CAM_CONNECT_TIMEOUT_MS 12000         // wait this long for a connection, then scan again
#define CAM_LOST_RESCAN_MS     15000         // connection gone this long -> scan again (camera off?)
#define CAM_RESCAN_MS          5000          // no camera found -> scan this often
#define CAM_CHOICE_RESCAN_MS   20000         // several found, none chosen -> scan this often

// --- Home network (Ethernet, DHCP) ------------------------------------------------
#define HOSTNAME           "wifi-cam"        // -> http://wifi-cam.local
#define HTTP_PORT          80
#define MAX_STREAM_CLIENTS 3                 // ~2.5-3.5 Mbit/s per viewer; 3 is the maximum on 10 Mbit Ethernet
#ifndef OTA_PASSWORD
#define OTA_PASSWORD       ""                // empty = updates without password; set in secrets.h
#endif

// --- Rescue mode (src/rescue.cpp) -------------------------------------------------
// If Ethernet has no IP for this long, Wi-Fi switches from the camera to the home
// Wi-Fi so the web UI and OTA stay reachable. If none is configured or reachable, the
// device opens its own access point with a setup page (http://192.168.4.1/wifi-setup).
// When Ethernet comes back, the device restarts.
#define RESCUE_TIMEOUT_MS     (30 * 1000)
#define RESCUE_STA_TIMEOUT_MS (10 * 1000)       // try home Wi-Fi this long, then AP
#define RESCUE_STA_RETRY_MS   (5 * 60 * 1000)   // retry home Wi-Fi while the AP is running
#define SETUP_AP_PREFIX       "WiFi-Cam-"       // + last 4 digits of the MAC
#ifndef SETUP_AP_PASSWORD
#define SETUP_AP_PASSWORD     "wificam-setup"   // at least 8 characters, else open; change in secrets.h
#endif

// ================================================================================
// Board-specific hardware configuration
// The board is selected via build_flags in platformio.ini:
//   -DBOARD_ZB_GW03      (default)
//   -DBOARD_WT32_ETH01
//   -DBOARD_CYD
// ================================================================================

#if defined(BOARD_CYD)
// --- CYD ESP32-2432S028R: display instead of Ethernet (src/main_cyd.cpp) -------------
// Display (HSPI 13/12/14, DC 2, CS 15, backlight 21) and touch (XPT2046 25/32/39/33)
// are configured by LovyanGFX itself, including ILI9341/ST7789 detection.
#define LED_GREEN_GPIO     -1
#define LED_RED_GPIO       -1
#define ZIGBEE_NRST_GPIO   -1
#define CYD_RGB_LED_PINS   {4, 16, 17}     // RGB LED, active LOW: switched off at startup
// Direction of setRotation() that turns the image by +90° (clockwise): +1 or -1. The
// rotation per camera model comes from cameraImageRotation() (otoscope: -90°).
#define CYD_ROTATE_DIR     1
#define CYD_MENU_TIMEOUT_MS 15000          // menu closes by itself
// Display SPI clock: shorter write per frame = less visible tearing. Autodetect uses
// 40 MHz for the ILI9341 variant. The ESP32 only divides 80 MHz (80, 40, 26.7, ...);
// if the image is garbled or has wrong colours, go back to 40000000.
#define CYD_SPI_WRITE_HZ   80000000
// Overlap decoding and display transfer: JPEGDEC's ping-pong buffers, the display gets
// one half by DMA while the next MCU group is decoded (include/jpeg_crop.h). 0 = blocking
// transfer as before.
#define CYD_USE_DMA        1
// Core of the display task (decode + draw). Core 0 also runs the Wi-Fi driver and lwIP
// (their interrupts and high-priority tasks interrupt the decoder); core 1 only the
// light video task and loop().
#define CYD_DISPLAY_CORE   1
// The display holds one frame while it decodes it (~140 ms), the next one is built
// meanwhile. With the 720p MAX-VIEW (33-98 KB per frame) both together left the Wi-Fi
// driver too little: its receive buffers (up to 32 x 1.6 KB) failed, heap down to 224
// bytes, hundreds of lost packets per second, streaks from damaged frames. So every
// chunk must leave room for a full burst of receive buffers; a large frame that does not
// fit beside the displayed one is dropped and the next one is taken after the decode.
#define FRAME_HEAP_RESERVE (48 * 1024)
#define FRAME_HEAP_FLOOR   (48 * 1024)
// A frame with lost packets shows as horizontal streaks from the gap down to the bottom,
// and on the CYD it stays on screen until the next decode (~200 ms): better the previous
// clean image a little longer
#define SHOW_DAMAGED_FRAMES 0
// JPEGDEC stops at runs of JPEG fill bytes (MAX-VIEW): cut them, see collapseFill()
#define JPEG_COLLAPSE_FILL 1

#elif defined(BOARD_WT32_ETH01)
// --- WT32-ETH01 (ESP32 + LAN8720 with its own 50 MHz oscillator on GPIO0) ------------
// Advantage over the ZB-GW03: the external oscillator is unaffected by Wi-Fi reception,
// so no 10 Mbit limit is needed. Up to 100 Mbit even with Wi-Fi active.
// Source: https://github.com/egnor/wt32-eth01
#define ETH_PHY_ADDR_GW    1
#define ETH_MDC_GPIO       23
#define ETH_MDIO_GPIO      18
#define ETH_POWER_GPIO     16               // -1 on some variants; 16 for v1.4
#define ETH_CLK_MODE_GW    ETH_CLOCK_GPIO0_IN  // external 50 MHz oscillator
#define LED_GREEN_GPIO     -1              // no free LED on the board (e.g. 2 for an external LED)
#define LED_RED_GPIO       -1
#define ZIGBEE_NRST_GPIO   -1              // no Zigbee module
#define ETH_10MBIT_DEFAULT false           // 100 Mbit possible thanks to the external clock

#else
// --- ZB-GW03 v1.4 (ESP32 + LAN8720, originally a Zigbee gateway) --------------------
// The ESP32 generates the 50 MHz clock for the LAN8720 itself (GPIO17). Wi-Fi
// reception disturbs it at 100 Mbit. Fix: lock Ethernet to 10 Mbit.
// Source: https://github.com/syssi/esphome-zb-gw03
#define ETH_PHY_ADDR_GW    1
#define ETH_MDC_GPIO       23
#define ETH_MDIO_GPIO      18
#define ETH_POWER_GPIO     16
#define ETH_CLK_MODE_GW    ETH_CLOCK_GPIO17_OUT
#define LED_GREEN_GPIO     14              // on = firmware running (active LOW)
#define LED_RED_GPIO       15              // on = rescue mode (active LOW)
#define ZIGBEE_NRST_GPIO   13             // nRST of the EFR32, LOW = held in reset
#define ETH_10MBIT_DEFAULT true
// 10 Mbit Ethernet: the 720p MAX-VIEW sends ~22 fps with 33-98 KB per frame (6-17
// Mbit/s), more than the link carries -> the stream stuttered. While a viewer sends a
// frame it also holds it, and the next one has to fit beside it. 10 fps still stuttered,
// so at most 5 fps per viewer for such large images (enough for a microscope). The
// otoscope (640x480, 20-45 KB) fits at full rate and is not capped (STREAM_LIMIT_ABOVE_PX).
#define STREAM_MAX_FPS     5

#endif  // board selection
#ifndef STREAM_MAX_FPS
#define STREAM_MAX_FPS     0               // default frames per second per viewer, 0 = unlimited (/settings, NVS stream_fps)
#endif
#define STREAM_LIMIT_ABOVE_PX (640 * 480)  // the fps limit applies only to images with more pixels

// LED polarity: ZB-GW03 = inverted (LOW = on), WT32-ETH01 = normal (HIGH = on)
#if defined(BOARD_WT32_ETH01)
#define LED_ACTIVE_HIGH    1
#else
#define LED_ACTIVE_HIGH    0
#endif

#ifndef ETH_TX_STORE_FORWARD
#define ETH_TX_STORE_FORWARD 1
#endif

// --- Video ----------------------------------------------------------------------
// Largest frame size. The Soulear otoscope delivers up to ~41 KB (480x480), 720p
// microscopes (MS5) considerably more. Without PSRAM two such frames barely fit in
// memory: chunks beyond FRAME_RESERVE_FROM are only accepted while FRAME_HEAP_RESERVE
// heap remains free afterwards (otherwise the frame is dropped, "drop_nomem").
#define MAX_FRAME_BYTES    (96 * 1024)
#define FRAME_RESERVE_FROM (48 * 1024)       // up to here as before (proven on the otoscope)
#ifndef FRAME_HEAP_RESERVE
#define FRAME_HEAP_RESERVE (40 * 1024)       // for the Wi-Fi driver, lwIP and HTTP tasks
#endif
// Below this no chunk is ever taken from the heap, whatever the frame size: without it
// the first 48 KB of a frame drained the heap to ~1.4 KB on the CYD with the MAX-VIEW
// (the display holds one 720p frame while the next is built), the Wi-Fi driver lost
// hundreds of packets per second and frames came out damaged.
#ifndef FRAME_HEAP_FLOOR
#define FRAME_HEAP_FLOOR   (16 * 1024)
#endif
#define STALL_TIMEOUT_MS   200               // no data this long -> handshake again (normally a frame every ~58 ms)
#define HANDSHAKE_RETRY_MS 800               // earliest next START after a START
// Show frames with lost packets anyway (1) or drop them (0).
#ifndef SHOW_DAMAGED_FRAMES
#define SHOW_DAMAGED_FRAMES 1
#endif
// Cut runs of fill bytes FF in the JPEG data to one (JHCMD, cam_jhcmd.cpp). Only the
// CYD's decoder needs it; browsers decode the frames as they are, so the bridge saves a
// pass over every frame.
#ifndef JPEG_COLLAPSE_FILL
#define JPEG_COLLAPSE_FILL 0
#endif
// Wi-Fi mode towards the camera if nothing is stored in NVS ("bgn", "bg", "b").
#define WIFI_MODE_DEFAULT  "bg"
// Wi-Fi transmit power in 0.25 dBm if nothing is stored in NVS (8..84; 44 = 11 dBm).
#define WIFI_TX_QDBM_DEFAULT 44

// --- Experimental switches ----------------------------------------------------------
#ifndef USE_IRAM_CHUNKS
#define USE_IRAM_CHUNKS 1
#endif
#ifndef SEND_BLOCK_SEGMENTS
#define SEND_BLOCK_SEGMENTS 4
#endif
