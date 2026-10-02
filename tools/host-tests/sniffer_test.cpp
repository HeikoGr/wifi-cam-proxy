#include "sniffer.h"
#include <esp_wifi.h>
#include <vector>
#include <string>
extern wifi_promiscuous_cb_t g_cb;
// 802.11 QoS data frame + LLC/SNAP + IPv4 + transport
static void frame(uint8_t proto, const uint8_t src[4], const uint8_t dst[4], const std::vector<uint8_t> &l4) {
  std::vector<uint8_t> f = {0x88, 0x01, 0, 0};                 // QoS data, to DS
  f.resize(24, 0x11); f.push_back(0); f.push_back(0);           // addresses, seq, QoS
  std::vector<uint8_t> snap = {0xAA, 0xAA, 0x03, 0, 0, 0, 0x08, 0x00};
  f.insert(f.end(), snap.begin(), snap.end());
  int iplen = 20 + l4.size();
  std::vector<uint8_t> ip = {0x45, 0, (uint8_t)(iplen >> 8), (uint8_t)iplen, 0, 0, 0, 0, 64, proto, 0, 0};
  ip.insert(ip.end(), src, src + 4); ip.insert(ip.end(), dst, dst + 4);
  f.insert(f.end(), ip.begin(), ip.end()); f.insert(f.end(), l4.begin(), l4.end());
  f.insert(f.end(), 4, 0xFC);                                    // FCS
  std::vector<uint8_t> buf(sizeof(wifi_promiscuous_pkt_t) + f.size());
  auto *p = (wifi_promiscuous_pkt_t *)buf.data(); p->rx_ctrl.sig_len = f.size();
  memcpy(p->payload, f.data(), f.size());
  g_cb(buf.data(), WIFI_PKT_DATA);
}
int main() {
  uint8_t cam[4] = {192, 168, 29, 1}, phone[4] = {192, 168, 29, 3}, other[4] = {8, 8, 8, 8};
  sniffStart(9, 0);
  frame(17, phone, cam, {0xd6, 0xd8, 0x4e, 0x20, 0, 16, 0, 0, 'J', 'H', 'C', 'M', 'D', 0x20, 0x02, 0x40});
  frame(6, phone, cam, {0xc0, 0x01, 0x1f, 0x90, 0, 0, 0, 1, 0, 0, 0, 2, 0x50, 0x10, 0x10, 0, 0, 0, 0, 0});  // TCP ACK
  frame(6, cam, phone, {0x1f, 0x90, 0xc0, 0x01, 0, 0, 0, 1, 0, 0, 0, 2, 0x50, 0x18, 0x10, 0, 0, 0, 0, 0, 'h', 'i'});
  frame(1, phone, cam, {8, 0, 0, 0, 0, 1, 0, 1, 'p', 'i', 'n', 'g'});   // ICMP echo
  frame(17, phone, other, {0, 53, 0, 53, 0, 9, 0, 0, 'x'});              // not the camera
  // video to the phone: a large packet, then small ones from the same port are only counted
  {
    std::vector<uint8_t> big = {0x2a, 0x62, 0x2a, 0x94, 0x02, 0x00, 0, 0};   // 10850 -> 10900
    big.resize(8 + 700, 0xAB);
    big[4] = (uint8_t)((8 + 700) >> 8); big[5] = (uint8_t)(8 + 700);
    frame(17, cam, phone, big);
    std::vector<uint8_t> small = {0x2a, 0x62, 0x2a, 0x94, 0, 8 + 40, 0, 0};
    small.resize(8 + 40, 0xCD);
    frame(17, cam, phone, small);
  }
  // the 105-byte info reply of the MAX-VIEW to the phone: has to be recorded completely
  {
    std::vector<uint8_t> reply = {0x4e, 0x20, 0x4e, 0x20, 0, 113, 0, 0};  // UDP 20000 -> 20000, length 8 + 105
    std::vector<uint8_t> payload(105, 0);
    memcpy(payload.data(), "JHCMD\x20\x00\x61", 8);
    memcpy(payload.data() + 24, "YPC320", 6);
    payload[43] = 0x07;
    memcpy(payload.data() + 69, "MAXVIEW-", 8);
    payload[104] = 0xA5;
    reply.insert(reply.end(), payload.begin(), payload.end());
    frame(17, cam, phone, reply);
  }
  std::string text;
  sniffText([](void *t, const char *l) { ((std::string *)t)->append(l); }, &text);
  fputs(text.c_str(), stdout);
  // what has to be in the recording (and what not)
  struct Check { const char *needle; bool want; } checks[] = {
      {"HT40-", true},
      {"beacon: 11b+ 11g+ 11n+, secondary channel below", true},
      {"UDP      192.168.29.3:55000 -> 192.168.29.1:20000    8  4a 48 43 4d 44 20 02 40", true},
      {"TCP A    192.168.29.3:49153 -> 192.168.29.1:8080    0 ", true},     // empty ACK
      {"TCP AP   192.168.29.1:8080 -> 192.168.29.3:49153    2  68 69", true},  // 2 payload bytes
      {"IP proto 1 192.168.29.3", true},
      {"8.8.8.8", false},                                                   // not the camera
      {"10850 ->", false},                                                  // video (large and small) is not listed
      {"Video (not listed): 2 packets", true},
      {"192.168.29.1:20000 -> 192.168.29.3:20000  105  4a 48 43 4d 44 20 00 61", true},
      {"4d 41 58 56 49 45 57 2d", true},  // offset 69: still in the recording
      {"00 a5  'JHCMD", true},             // last byte (offset 104)
  };
  int fails = 0;
  for (auto &c : checks)
    if ((text.find(c.needle) != std::string::npos) != c.want) {
      printf("FAIL: %s \"%s\"\n", c.want ? "missing" : "unexpected", c.needle);
      fails++;
    }
  printf(fails ? "FAIL\n" : "OK\n");
  return fails;
}
