// Protocol probes ("automatic") against fake cameras on loopback addresses 127.0.0.x
#include "camera.h"
#include <lwip/sockets.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> stop{false};

// A fake camera at 127.0.0.<host>:port: answers every request from its sender's port,
// optionally not the first one (lost on the air)
static void fakeCamera(int host, uint16_t port, std::string reply, bool dropFirst) {
  int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));  // the JHCMD probe binds 20000 as well
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = ipv4(127, 0, 0, host);
  if (bind(s, (sockaddr *)&a, sizeof(a))) perror("bind");
  timeval tv = {0, 20 * 1000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  for (int n = 0; !stop;) {
    uint8_t b[256];
    sockaddr_in from;
    socklen_t fl = sizeof(from);
    if (recvfrom(s, b, sizeof(b), 0, (sockaddr *)&from, &fl) > 0 && !(dropFirst && n++ == 0))
      sendto(s, reply.data(), reply.size(), 0, (sockaddr *)&from, fl);
  }
  close(s);
}

int main() {
  int fails = 0;
  auto check = [&](const char *what, bool ok) { printf("%-62s %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; };
  std::string devinfo("\xEE\xFF\xEE\xFF\x00\x00\x01\x00\x01\x00\x79\x00", 12);  // GetDeviceInfo reply
  devinfo.resize(12 + 0x79, 0);
  std::string info("JHCMD\x20\x00\x61", 8);  // the MAX-VIEW's reply to INIT2
  info.resize(105, 0);
  std::vector<std::thread> cams;
  cams.emplace_back(fakeCamera, 2, 10005, devinfo, false);  // .2: i4season
  cams.emplace_back(fakeCamera, 3, 20000, info, false);     // .3: JHCMD
  cams.emplace_back(fakeCamera, 4, 10005, devinfo, true);   // .4: i4season, first request lost
  cams.emplace_back(fakeCamera, 5, 10005, "hello", false);  // .5: something else on 10005
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto timed = [](const CamProtocol &p, uint32_t ip, unsigned long &ms) {
    unsigned long t0 = millis();
    bool r = p.probe(ip);
    ms = millis() - t0;
    return r;
  };
  unsigned long ms;
  check("i4season camera answers the i4season probe", timed(PROTOCOL_I4SEASON, ipv4(127, 0, 0, 2), ms) && ms < 100);
  check("i4season camera does not answer the JHCMD probe", !PROTOCOL_JHCMD.probe(ipv4(127, 0, 0, 2)));
  check("JHCMD camera answers the JHCMD probe", PROTOCOL_JHCMD.probe(ipv4(127, 0, 0, 3)));
  check("JHCMD camera does not answer the i4season probe", !PROTOCOL_I4SEASON.probe(ipv4(127, 0, 0, 3)));
  check("first request lost: the second one gets the answer", PROTOCOL_I4SEASON.probe(ipv4(127, 0, 0, 4)));
  check("an answer that is not GetDeviceInfo does not count", !PROTOCOL_I4SEASON.probe(ipv4(127, 0, 0, 5)));
  check("no camera: no answer, gives up after ~400 ms", !timed(PROTOCOL_I4SEASON, ipv4(127, 0, 0, 9), ms) && ms < 600);
  stop = true;
  for (auto &t : cams) t.join();
  printf(fails ? "FAIL\n" : "OK\n");
  return fails;
}
