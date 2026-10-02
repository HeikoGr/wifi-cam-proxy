// allocChunk with a short heap: a frame beyond FRAME_RESERVE_FROM gets the memory of the
// stored frame if nobody else holds it, otherwise it is refused (as before).
#include "camera.h"
#include <esp_memory_utils.h>
#include <malloc.h>
VideoStats stats;
size_t heapBudget = 0, heapBaseline = 0;

static Frame build(size_t bytes) {  // a frame of 1442-byte chunks, like the MAX-VIEW
  static uint8_t pkt[1442];
  Frame f = Frame::create();
  while (f && f.size() < bytes)
    if (!f.append(pkt, sizeof(pkt))) return Frame();
  return f;
}

int main() {
  int fails = 0;
  auto check = [&](const char *what, bool ok) { printf("%-58s %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; };
  heapBaseline = mallinfo2().uordblks;
  heapBudget = 140 * 1024;  // free heap with nothing stored

  publishFrame(build(70 * 1024));  // stored frame, 70 KB
  Frame b = build(70 * 1024);      // next one: 70 + 70 KB + 40 KB reserve do not fit
  check("second 70 KB frame built (stored one released)", (bool)b && b.size() >= 70 * 1024);
  check("released counter = 1", stats.framesReleased == 1);
  Frame cur;
  getFrame(cur);
  check("store is empty until the new frame is published", !cur);
  if (b) publishFrame(b);
  b.reset();

  Frame viewer;
  getFrame(viewer);  // a stream viewer is sending the stored frame
  Frame c = build(70 * 1024);
  check("frame refused while a viewer holds the stored one", !c);
  check("released counter still 1", stats.framesReleased == 1);
  getFrame(cur);
  check("the viewer's frame is still stored", cur && cur.size() >= 70 * 1024);
  cur.reset();
  viewer.reset();

  Frame small = build(30 * 1024);  // below FRAME_RESERVE_FROM: no rule, plain malloc
  check("30 KB frame (below 48 KB) is built", (bool)small);

  // Below 48 KB the heap floor still applies: a frame held elsewhere (the CYD decoding
  // it) and a small budget must not drain the heap to zero
  small.reset();
  getFrame(viewer);
  heapBudget = mallinfo2().uordblks - heapBaseline + 40 * 1024;  // 40 KB left
  Frame d = build(30 * 1024);
  check("30 KB frame refused when it would cut into the floor", !d);
  check("free heap stays above FRAME_HEAP_FLOOR", heapFree() >= FRAME_HEAP_FLOOR);
  viewer.reset();

  // A waiting stream viewer only looks at the sequence number: that holds no reference,
  // so the stored frame can still be released for the next one
  heapBudget = 140 * 1024;
  publishFrame(build(70 * 1024));
  uint32_t seq = latestFrameSeq();
  uint32_t rel0 = stats.framesReleased;
  Frame e = build(70 * 1024);
  check("latestFrameSeq() holds no reference (stored one released)", e && stats.framesReleased == rel0 + 1);
  e.reset();
  publishFrame(build(1024));
  check("latestFrameSeq() counts up with every published frame", latestFrameSeq() == seq + 1);
  clearFrame();

  // Word-wise IRAM copies: every offset/length against memcpy, aligned and unaligned target
  {
    fakeIram = true;
    uint8_t src[64], chunk[68] __attribute__((aligned(4))), back[68] __attribute__((aligned(4)));
    for (int i = 0; i < 64; i++) src[i] = (uint8_t)(i * 7 + 1);
    bool okTo = true, okFrom = true;
    for (size_t len = 0; len <= 64; len++) {
      memset(chunk, 0xEE, sizeof(chunk));
      copyToChunk(chunk, src, len);
      okTo &= !memcmp(chunk, src, len);
    }
    copyToChunk(chunk, src, 64);
    for (size_t off = 0; off <= 64; off++)
      for (size_t len = 0; off + len <= 64; len++)
        for (size_t d = 0; d < 4; d++) {
          memset(back, 0xEE, sizeof(back));
          copyFromChunk(back + d, chunk, off, len);
          bool same = !memcmp(back + d, src + off, len) && (!d || back[d - 1] == 0xEE) && back[d + len] == 0xEE;
          if (!same) okFrom = false;
        }
    fakeIram = false;
    check("copyToChunk word-wise: every length", okTo);
    check("copyFromChunk word-wise: every offset, length, alignment", okFrom);
  }
  printf(fails ? "FAIL\n" : "OK\n");
  return fails;
}
