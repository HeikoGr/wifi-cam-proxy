#include "sniffer.h"

#include <WiFi.h>
#include <esp_wifi.h>

#include "camera.h"
#include "crashlog.h"

void wifiApplyMode();  // main.cpp: Wi-Fi mode towards the camera (b/g by default)

namespace {

const int MAX_ENTRIES = 128;  // ring: the newest packets win
const int DATA_BYTES = 112;   // payload bytes kept per packet (the MAX-VIEW info reply has 105)
const int VIDEO_MIN = 600;    // UDP from the camera at least this big = video (not kept)

struct Entry {
  uint32_t ms, lastMs;  // first and last time (identical repeats are merged)
  uint32_t src, dst;    // IPv4 addresses (network order)
  uint16_t sport, dport, len, repeats;
  uint8_t proto, flags;  // IP protocol (17 UDP, 6 TCP, ...), TCP flags
  uint8_t data[DATA_BYTES];
};

Entry *ring = nullptr;
int head = 0, count = 0;  // next slot, entries in use
uint32_t camAddr = 0, startMs = 0, videoPackets = 0, videoBytes = 0;
uint16_t videoPort = 0;
uint16_t videoSport = 0;  // source port of the large packets from the camera: all of its packets are video
int sniffChannel = 0;
char apInfo[64] = "";
int sniffSecond = 0;  // what the camera's beacon says about 11n/HT40
bool active = false;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Called by the Wi-Fi driver for every received data frame
void onFrame(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_DATA) return;
  const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
  const uint8_t *f = p->payload;
  int len = (int)p->rx_ctrl.sig_len - 4;  // without FCS
  if (len < 24) return;
  uint16_t fc = f[0] | (f[1] << 8);
  if (fc & 0x4000) return;                // protected: not an open network
  if (fc & 0x0800) return;                // retry: the same frame was already seen
  int subtype = (fc >> 4) & 0xF;
  if (subtype & 0x4) return;              // null data, no payload
  int hdr = 24;
  if (((fc >> 8) & 3) == 3) hdr += 6;     // 4 addresses (WDS)
  if (subtype & 0x8) {                    // QoS data
    hdr += 2;
    if (fc & 0x8000) hdr += 4;            // HT control
  }
  const uint8_t *l = f + hdr;
  if (len < hdr + 8 + 20 + 8) return;
  // LLC/SNAP with IPv4
  if (l[0] != 0xAA || l[1] != 0xAA || l[2] != 0x03 || l[6] != 0x08 || l[7] != 0x00) return;
  const uint8_t *ip = l + 8;
  if ((ip[0] >> 4) != 4) return;  // IPv4
  uint8_t proto = ip[9];
  int ihl = (ip[0] & 0x0F) * 4;
  if (len < hdr + 8 + ihl + 4) return;
  uint32_t src, dst;
  memcpy(&src, ip + 12, 4);
  memcpy(&dst, ip + 16, 4);
  if (src != camAddr && dst != camAddr) return;
  const uint8_t *u = ip + ihl;
  uint16_t sport = 0, dport = 0;
  uint8_t flags = 0;
  int ulen, l4len;  // payload length, length of the transport header
  int iplen = (ip[2] << 8) | ip[3];
  if (proto == 17) {  // UDP
    sport = (u[0] << 8) | u[1];
    dport = (u[2] << 8) | u[3];
    l4len = 8;
    ulen = ((u[4] << 8) | u[5]) - 8;
  } else if (proto == 6) {  // TCP: also log empty segments (ACKs show a connection)
    if (len < hdr + 8 + ihl + 20) return;
    sport = (u[0] << 8) | u[1];
    dport = (u[2] << 8) | u[3];
    l4len = (u[12] >> 4) * 4;
    flags = u[13];
    ulen = iplen - ihl - l4len;
  } else {  // ICMP, ...: only the protocol number
    l4len = 0;
    ulen = iplen - ihl;
  }
  int avail = len - (hdr + 8 + ihl + l4len);
  if (ulen < 0 || avail < 0) return;
  u += l4len - 8;  // so that u + 8 points at the payload, as for UDP
  // video: only count it. The stream to the phone also has packets below VIDEO_MIN, which
  // would flood the ring: so everything from the port the large ones come from counts too.
  if (proto == 17 && src == camAddr && (ulen >= VIDEO_MIN || (videoSport && sport == videoSport))) {
    portENTER_CRITICAL(&mux);
    if (ulen >= VIDEO_MIN) videoSport = sport;
    videoPackets++;
    videoBytes += ulen;
    videoPort = dport;
    portEXIT_CRITICAL(&mux);
    return;
  }
  int keep = min(min(ulen, avail), DATA_BYTES);
  uint32_t now = millis();
  portENTER_CRITICAL(&mux);
  if (ring) {
    Entry *last = count ? &ring[(head + MAX_ENTRIES - 1) % MAX_ENTRIES] : nullptr;
    if (last && last->proto == proto && last->flags == flags && last->src == src && last->dst == dst &&
        last->sport == sport && last->dport == dport && last->len == ulen && !memcmp(last->data, u + 8, keep)) {
      last->repeats++;  // e.g. the app's heartbeat: one line with a counter
      last->lastMs = now;
    } else {
      Entry &e = ring[head];
      e.ms = e.lastMs = now;
      e.src = src;
      e.dst = dst;
      e.sport = sport;
      e.dport = dport;
      e.len = ulen;
      e.repeats = 1;
      e.proto = proto;
      e.flags = flags;
      memset(e.data, 0, sizeof(e.data));
      memcpy(e.data, u + 8, keep);
      head = (head + 1) % MAX_ENTRIES;
      if (count < MAX_ENTRIES) count++;
    }
  }
  portEXIT_CRITICAL(&mux);
}

}  // namespace

bool sniffStart(int channel, uint32_t camIp, char second) {
  if (active) return true;
  // While still connected: ask the camera's beacon for 11n and the HT40 secondary
  // channel (scan of this one channel only)
  wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
  strlcpy(apInfo, "beacon not read", sizeof(apInfo));
  if (WiFi.status() == WL_CONNECTED) {
    String ssid = WiFi.SSID();
    int ch = WiFi.channel();
    int n = WiFi.scanNetworks(false, false, false, 300, ch, ssid.c_str());
    for (int i = 0; i < n; i++) {
      const wifi_ap_record_t *ap = (const wifi_ap_record_t *)WiFi.getScanInfoByIndex(i);
      if (!ap || ssid != (const char *)ap->ssid) continue;
      sec = ap->second;
      snprintf(apInfo, sizeof(apInfo), "beacon: 11b%s%s%s, secondary channel %s", ap->phy_11b ? "+" : "-",
               ap->phy_11g ? " 11g+" : " 11g-", ap->phy_11n ? " 11n+" : " 11n-",
               sec == WIFI_SECOND_CHAN_ABOVE ? "above" : sec == WIFI_SECOND_CHAN_BELOW ? "below" : "none");
      break;
    }
    WiFi.scanDelete();
  }
  if (second == 'a') sec = WIFI_SECOND_CHAN_ABOVE;
  else if (second == 'b') sec = WIFI_SECOND_CHAN_BELOW;
  else if (second == 'n') sec = WIFI_SECOND_CHAN_NONE;
  portENTER_CRITICAL(&mux);
  Entry *old = ring;  // recording of the previous run
  ring = nullptr;
  portEXIT_CRITICAL(&mux);
  free(old);
  if (!channel && WiFi.status() == WL_CONNECTED) channel = WiFi.channel();
  if (channel < 1 || channel > 14) return false;
  Entry *r = (Entry *)malloc(sizeof(Entry) * MAX_ENTRIES);
  if (!r) return false;
  cameraPause(true);  // leave the camera Wi-Fi, so the app can connect
  portENTER_CRITICAL(&mux);
  ring = r;
  head = count = 0;
  videoPackets = videoBytes = 0;
  videoPort = 0;
  videoSport = 0;
  portEXIT_CRITICAL(&mux);
  camAddr = camIp ? camIp : (uint32_t)IPAddress(192, 168, 29, 1);
  sniffChannel = channel;
  startMs = millis();
  // Receive 802.11n as well: the bridge itself works in b/g, but the camera talks to the
  // phone in 11n, and those frames are only decoded with n enabled
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  esp_wifi_set_bandwidth(WIFI_IF_STA, sec == WIFI_SECOND_CHAN_NONE ? WIFI_BW_HT20 : WIFI_BW_HT40);
  wifi_promiscuous_filter_t filter = {WIFI_PROMIS_FILTER_MASK_DATA};
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(onFrame);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, sec);
  sniffSecond = sec;
  active = true;
  crumb("sniffer: on, channel %d, camera %s", channel, IPAddress(camAddr).toString().c_str());
  return true;
}

void sniffStop() {
  if (!active) return;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  active = false;
  wifiApplyMode();  // back to the bridge's own mode
  // Free the recording (18 KB): it would stay missing in the frame store of the 720p
  // cameras. So read it with GET /sniff before stopping.
  portENTER_CRITICAL(&mux);
  Entry *old = ring;
  ring = nullptr;
  head = count = 0;
  portEXIT_CRITICAL(&mux);
  free(old);
  cameraPause(false);
  crumb("sniffer: off");
}

bool sniffActive() { return active; }

bool sniffText(void (*put)(void *ctx, const char *line), void *ctx) {
  char line[700];
  if (!ring) {
    put(ctx, "No recording (it is freed on /sniff/stop). Start with POST /sniff/start\n");
    return true;
  }
  int n, first;
  uint32_t vp, vb;
  uint16_t vport;
  portENTER_CRITICAL(&mux);
  n = count;
  first = (head + MAX_ENTRIES - count) % MAX_ENTRIES;
  vp = videoPackets;
  vb = videoBytes;
  vport = videoPort;
  portEXIT_CRITICAL(&mux);

  snprintf(line, sizeof(line),
           "Sniffer %s, channel %d%s, camera %s, %u s. Video (not listed): %u packets, %u KB, to port %u\n%s\n"
           "time s (since start)  protocol  from -> to  payload length  payload (max. %d bytes)  [xN = repeated, until]\n",
           active ? "running" : "stopped", sniffChannel,
           sniffSecond == WIFI_SECOND_CHAN_ABOVE ? " HT40+" : sniffSecond == WIFI_SECOND_CHAN_BELOW ? " HT40-" : " HT20",
           IPAddress(camAddr).toString().c_str(), (unsigned)((millis() - startMs) / 1000), (unsigned)vp,
           (unsigned)(vb / 1024), vport, apInfo, DATA_BYTES);
  put(ctx, line);
  for (int i = 0; i < n; i++) {
    Entry e;  // one at a time: no second copy of the whole ring
    portENTER_CRITICAL(&mux);
    e = ring[(first + i) % MAX_ENTRIES];
    portEXIT_CRITICAL(&mux);
    char kind[24];
    if (e.proto == 17) strlcpy(kind, "UDP", sizeof(kind));
    else if (e.proto == 6)
      snprintf(kind, sizeof(kind), "TCP %s%s%s%s%s", e.flags & 0x02 ? "S" : "", e.flags & 0x10 ? "A" : "",
               e.flags & 0x08 ? "P" : "", e.flags & 0x01 ? "F" : "", e.flags & 0x04 ? "R" : "");
    else snprintf(kind, sizeof(kind), "IP proto %u", e.proto);
    int o = snprintf(line, sizeof(line), "%8.3f  %-8s %s:%u -> ", (e.ms - startMs) / 1000.0f, kind,
                     IPAddress(e.src).toString().c_str(), e.sport);
    o += snprintf(line + o, sizeof(line) - o, "%s:%u  %3u  ", IPAddress(e.dst).toString().c_str(), e.dport, e.len);
    int k = min((int)e.len, DATA_BYTES);
    for (int b = 0; b < k && o < (int)sizeof(line) - 140; b++) o += snprintf(line + o, sizeof(line) - o, "%02x ", e.data[b]);
    o += snprintf(line + o, sizeof(line) - o, " '");
    for (int b = 0; b < k && o < (int)sizeof(line) - 100; b++)
      line[o++] = e.data[b] >= 32 && e.data[b] < 127 ? e.data[b] : '.';
    o += snprintf(line + o, sizeof(line) - o, "'");
    if (e.repeats > 1)
      o += snprintf(line + o, sizeof(line) - o, "  x%u until %.3f", e.repeats, (e.lastMs - startMs) / 1000.0f);
    snprintf(line + o, sizeof(line) - o, "\n");
    put(ctx, line);
  }
  return true;
}
