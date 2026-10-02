// allocChunk with a short heap: a frame beyond FRAME_RESERVE_FROM gets the memory of the
// stored frame if nobody else holds it, otherwise it is refused (as before).
#include "camera.h"
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
  printf(fails ? "FAIL\n" : "OK\n");
  return fails;
}
