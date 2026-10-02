// Stand-ins for the parts of the firmware that cam_jhcmd.cpp calls (frame store, diagnostics)
#include "camera.h"
#include "crashlog.h"
#include <vector>
VideoStats stats;
CamTelemetry telemetry;
std::vector<std::string> published;
int allocFailAt = -1, allocCount = 0;  // test: let the n-th chunk allocation fail
uint8_t *allocChunk(size_t len, size_t) {
  if (allocCount++ == allocFailAt) return nullptr;
  return (uint8_t *)malloc((len + 3) & ~3);
}
void copyToChunk(uint8_t *d, const uint8_t *s, size_t n) { memcpy(d, s, n); }
void copyFromChunk(uint8_t *d, const uint8_t *c, size_t off, size_t n) { memcpy(d, c + off, n); }
void publishFrame(const Frame &f) {
  std::string s;
  for (int i = 0; i < f.chunks(); i++) s.append((const char *)f.chunk(i), f.chunkLen(i));
  published.push_back(s);
  stats.framesTotal++;
}
bool cameraLinkUp() { return false; }
void crumb(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); printf("\n"); }
void diagLog(const char *fmt, ...) { va_list a; va_start(a, fmt); printf("  diag: "); vprintf(fmt, a); va_end(a); printf("\n"); }
void diagReset() {}
bool diagRawWanted() { return false; }
void diagRawPut(const Frame &) {}
std::atomic<int> ledRequest{-1};
std::atomic<int> ledLevel{100};
portMUX_TYPE infoMux = 0;
bool diagSendTake(uint16_t &, uint8_t *, size_t &) { return false; }
