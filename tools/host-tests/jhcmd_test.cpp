// Feeds the captured MAX-VIEW packets to the JHCMD session over UDP (port 10900)
#include "camera.h"
#include <lwip/sockets.h>
#include <vector>
#include <string>
extern std::vector<std::string> published;
static std::vector<std::string> load(const char *path) {
  FILE *f = fopen(path, "rb"); std::string d; char b[4096]; size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) d.append(b, n); fclose(f);
  std::vector<std::string> p; for (size_t i = 0; i < d.size(); i += 1450) p.push_back(d.substr(i, 1450)); return p;
}
int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  auto pk = load(argv[1]);
  std::string jpeg; { std::string all; for (auto &p : pk) all += p.substr(8); size_t a = all.find("\xff\xd8\xff"); size_t e = all.rfind("\xff\xd9"); jpeg = all.substr(a, e + 2 - a); }
  // Expected with JPEG_COLLAPSE_FILL (CYD): the fill bytes before the RSTn markers cut to
  // one FF (JPEGDEC stops at two); without it (bridge) the JPEG as the camera sent it
  if (JPEG_COLLAPSE_FILL) {
    size_t scan = jpeg.find("\xff\xda"); scan += 2 + ((uint8_t)jpeg[scan + 2] << 8 | (uint8_t)jpeg[scan + 3]);
    size_t rawSize = jpeg.size();
    for (size_t i; (i = jpeg.find("\xff\xff", scan)) != std::string::npos;) jpeg.erase(i, 1);
    printf("fill bytes             %zu removed, no FF FF left in the scan %d\n", rawSize - jpeg.size(),
           jpeg.find("\xff\xff", scan) == std::string::npos);
  } else {
    printf("fill bytes             kept (JPEG_COLLAPSE_FILL 0)\n");
  }
  CamSession *s = PROTOCOL_JHCMD.create(IPAddress(127, 0, 0, 1));
  int tx = socket(AF_INET, SOCK_DGRAM, 0); sockaddr_in to = {}; to.sin_family = AF_INET; to.sin_port = htons(10900); to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  uint8_t buf[2048];
  auto feed = [&](const std::vector<int> &order) {
    for (int i : order) { sendto(tx, pk[i].data(), pk[i].size(), 0, (sockaddr *)&to, sizeof to); s->poll(buf, sizeof buf); }
  };
  int N = pk.size(); std::vector<int> inorder; for (int i = 0; i < N; i++) inorder.push_back(i);
  struct Case { const char *name; std::vector<int> order; int expectOk; } cases[] = {
    {"in order", inorder, 1},
    {"swap 2/3 and 10/11", [&] { auto o = inorder; std::swap(o[2], o[3]); std::swap(o[10], o[11]); return o; }(), 1},
    {"last before middle", [&] { auto o = inorder; o.erase(o.begin() + 5); o.push_back(5); return o; }(), 1},
    {"packet 0 last", [&] { auto o = inorder; o.erase(o.begin()); o.push_back(0); return o; }(), 1},
    {"packet 7 lost", [&] { auto o = inorder; o.erase(o.begin() + 7); return o; }(), 0},
  };
  int fails = 0;
  for (auto &c : cases) {
    size_t before = published.size(); uint32_t lost0 = stats.packetsLost, dmg0 = stats.framesDamaged, inc0 = stats.dropIncomplete;
    feed(c.order);
    feed({0});  // next frame starts -> an incomplete frame gets finished
    bool got = published.size() > before;
    bool ok = got && published[before] == jpeg;
    printf("%-22s published %zu, identical %d, lost +%u, damaged +%u, incomplete +%u\n", c.name, published.size() - before, ok,
           stats.packetsLost - lost0, stats.framesDamaged - dmg0, stats.dropIncomplete - inc0);
    if (c.expectOk && !ok) fails++;
    if (c.expectOk && got) {
      extern int publishedW, publishedH;  // the stream's fps limit depends on it
      if (publishedW != 1280 || publishedH != 720) { printf("FAIL: image size %dx%d, expected 1280x720\n", publishedW, publishedH); fails++; }
    }
    // flush: finish the partial next frame (packet 0 only) by sending the rest
    std::vector<int> rest; for (int i = 1; i < N; i++) rest.push_back(i); feed(rest);
    published.resize(before);
  }
  // No memory at packet 5: that frame is dropped, its remaining packets are ignored,
  // the next whole frame comes through
  {
    extern int allocFailAt, allocCount;
    size_t before = published.size(); uint32_t nm0 = stats.dropNoMem, inc0 = stats.dropIncomplete;
    allocCount = 0; allocFailAt = 5;
    feed(inorder);
    allocFailAt = -1;
    feed(inorder);
    feed({0});
    bool ok = published.size() == before + 1 && published[before] == jpeg && stats.dropNoMem == nm0 + 1 &&
              stats.dropIncomplete == inc0;
    printf("%-22s published %zu, identical %d, nomem +%u, incomplete +%u\n", "no memory at packet 5",
           published.size() - before, published.size() > before && published[before] == jpeg,
           stats.dropNoMem - nm0, stats.dropIncomplete - inc0);
    if (!ok) fails++;
  }
  // Messages to port 20000: light button and the reply to INIT2
  {
    extern std::atomic<int> ledLevel;
    to.sin_port = htons(20000);
    auto msg = [&](std::string m) { sendto(tx, m.data(), m.size(), 0, (sockaddr *)&to, sizeof to); s->poll(buf, sizeof buf); };
    msg(std::string("JHCMD\x10\x20\x3c", 8));
    bool ok1 = telemetry.led == 1 && ledLevel == 60;
    msg(std::string("JHCMD\x10\x20\x00", 8));
    bool ok2 = telemetry.led == 0 && ledLevel == 60;
    std::string info("JHCMD\x20\x00\x61", 8); info.resize(24, 0); info += "YPC320"; info.resize(105, 0);
    msg(info);
    // the reply to INIT2 always says 0x61: it must not switch the LED state
    bool ok3 = telemetry.led == 0 && ledLevel == 60 && !strcmp(telemetry.product, "YPC320");
    to.sin_port = htons(20001);
    msg(std::string("FDWN\x20\x00\x0e\x00\x01\x00\x1e", 11));
    bool ok4 = telemetry.led == 1 && ledLevel == 30;
    // FDWN status answer (48 bytes): byte 32 is taken as the raw battery value
    std::string st("FDWN\x00\x00\x01\x00\x1a\x00\x00\x05", 12); st.resize(48, 0); st[32] = (char)0x8a;
    msg(st);
    bool ok5 = telemetry.batteryRaw == 0x8a && telemetry.battery == 40;  // 138 - 91 = 47 -> 40 %
    st[32] = (char)0x89; msg(st);
    bool ok6 = telemetry.batteryRaw == 0x89 && telemetry.battery == 40;
    // the pairs read off the app: raw -> displayed %
    struct { int raw, pct; } pairs[] = {{110, 10}, {115, 20}, {137, 40}, {148, 50}, {152, 60}, {160, 60}, {162, 70},
                                         {91, 0}, {60, 0}, {191, 100}, {255, 100}};
    for (auto &p : pairs) {
      st[32] = (char)p.raw; msg(st);
      if (telemetry.battery != p.pct) { printf("FAIL: raw %d -> %d %%, expected %d %%\n", p.raw, (int)telemetry.battery, p.pct); ok6 = false; }
    }
    printf("%-22s button 60 %d, button off %d, info reply %d (product '%s'), FDWN 30 %d, status raw %d/%d\n", "light button",
           ok1, ok2, ok3, telemetry.product, ok4, ok5, ok6);
    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5 || !ok6) fails++;
  }
  delete s;
  printf(fails ? "FAIL\n" : "OK\n");
  return fails;
}
