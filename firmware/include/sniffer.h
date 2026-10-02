#pragma once

// Wi-Fi sniffer for working out camera commands (e.g. LED dimming in the vendor app):
// the bridge leaves the camera Wi-Fi, listens in promiscuous mode on its channel and
// records the UDP and TCP packets to and from the camera (without the UDP video) and
// notes other IP protocols. Works because the camera Wi-Fi is open (unencrypted). The
// bridge stays reachable via Ethernet. The recording (18 KB) is freed when the sniffer
// stops, so read it before.

#include <Arduino.h>

// Start on the given channel (0 = channel of the current camera connection) for the
// camera at camIp (0 = 192.168.29.1). second: HT40 secondary channel, 'a'bove, 'b'elow,
// 'n'one (20 MHz) or 0 = as announced by the camera's beacon (short scan while still
// connected). Returns false if no channel is known.
bool sniffStart(int channel, uint32_t camIp, char second = 0);
void sniffStop();
bool sniffActive();
// Write the recording as text (one line per packet), returns false without memory
bool sniffText(void (*put)(void *ctx, const char *line), void *ctx);
