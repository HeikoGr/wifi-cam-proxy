#pragma once

// Decode geometry of the CYD display (src/main_cyd.cpp): which part of the JPEG is
// decoded where, at which scale and in which MCU groups. Kept apart from the display code
// so the host tests can run it against JPEGDEC (tools/host-tests/jpeg_crop_test.cpp).

#include <JPEGDEC.h>

#include "jpeg_reader.h"

struct DecodePlan {
  bool ok;     // false: the JPEG could not be opened again after skipAbove()
  int dx, dy;  // position for decode(): where image pixel (ix, iy) lands on the display
  int ix, iy;  // the top left of the decoded part (crop start at a block edge, else 0, 0)
  int x, y;    // visible part on the display (clip rectangle)
  int w, h;
  int opt;     // options for decode(): scale and JPEG_USES_DMA
};

// JPEGDEC hands over the decoded MCUs of a row in groups (up to MAX_BUFFERED_PIXELS)
// and only draws a group once it is full: with a crop, the last, partly filled group of
// each row was never drawn (MAX-VIEW 1280x720 at 1:1: 21 MCUs per row in groups of 8,
// the right 64 pixels stayed black). So the group size has to divide the number of MCUs
// per row; if it does not, the crop is widened by a few MCUs (the clip rectangle hides
// them). JPEGDEC decodes MCU columns ax/mw rounded up to (ax+aw)/mw inclusive, and caps a
// group at aw/mw MCUs.
//
// dma: JPEGDEC's ping-pong mode (JPEG_USES_DMA): it splits its pixel buffer in two and
// alternates, so one half can go to the display by DMA while the next group is decoded.
// The group is then fixed at half the MCUs that fit (at most the crop width), and the mode
// is off as soon as setMaxOutputSize() caps the group: so only the crop width can be
// adjusted. Returns the decode options (JPEG_USES_DMA or 0).
inline int fitGroups(JPEGDEC &jpeg, int W, int ax, int ay, int aw, int ah, bool dma) {
  // As wide as the image (e.g. 480x480 on a 480x320 display): JPEGDEC does not crop
  // horizontally and draws the partial group itself
  if (ax == 0 && aw >= W) return dma ? JPEG_USES_DMA : 0;
  int sub = jpeg.getSubSample();
  int mw = (sub >> 4) == 2 ? 16 : 8, mh = (sub & 15) == 2 ? 16 : 8;
  int first = (ax + mw - 1) / mw, cols = (W + mw - 1) / mw;
  int maxGroup = MAX_BUFFERED_PIXELS / (mw * mh);
  if (dma) {
    int half = (maxGroup < cols ? maxGroup : cols) / 2;
    for (int extra = 0; extra < maxGroup && (ax + aw) / mw + extra < cols; extra++) {
      int w = aw + extra * mw;
      int mcus = (ax + w) / mw - first + 1;
      int g = half < w / mw ? half : w / mw;
      if (g >= 1 && mcus % g == 0) {
        if (w != aw) jpeg.setCropArea(ax, ay, w, ah);
        return JPEG_USES_DMA;
      }
    }
  }
  int bestGroup = 1, bestW = aw;
  for (int extra = 0; extra < maxGroup && (ax + aw) / mw + extra < cols; extra++) {
    int w = aw + extra * mw;
    int mcus = (ax + w) / mw - first + 1;
    int g = maxGroup < w / mw ? maxGroup : w / mw;
    while (g > 1 && mcus % g) g--;
    if (g > bestGroup) {
      bestGroup = g;
      bestW = w;
    }
    if (g * 2 > maxGroup) break;  // good enough
  }
  if (bestW != aw) jpeg.setCropArea(ax, ay, bestW, ah);
  jpeg.setMaxOutputSize(bestGroup);
  return 0;
}

// For the JPEG just opened in jpeg (from reader) and a dw x dh display (in the image's
// orientation): full = 1:1 centre crop if the image is larger, else the largest scale
// (1, 1/2, 1/4, 1/8) at which it fits. dma: use JPEGDEC's ping-pong buffers where the
// geometry allows (see fitGroups). reopen() opens reader's JPEG again in jpeg (needed
// after skipAbove()). The overlay strip is the caller's business (main_cyd.cpp).
template <class Reopen>
DecodePlan planDecode(JPEGDEC &jpeg, FrameReader &reader, int dw, int dh, bool full, bool dma, Reopen reopen) {
  DecodePlan p = {true, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  int W = jpeg.getWidth(), H = jpeg.getHeight();
  if (full && (W > dw || H > dh)) {
    // 1:1: centre crop. JPEGDEC moves the crop start down to a block edge (8/16
    // pixels) but keeps the width, so the crop would end that many pixels too early
    // (black bar on one side). Hence a second call that starts at the block edge and
    // reaches the wanted right/bottom end. decode() gets the screen position of that
    // block edge, so the image centre lands exactly in the display centre.
    p.w = W < dw ? W : dw;
    p.h = H < dh ? H : dh;
    int cx = (W - p.w) / 2, cy = (H - p.h) / 2, ax, ay, aw, ah;
    jpeg.setCropArea(cx, cy, p.w, p.h);
    jpeg.getCropArea(&ax, &ay, &aw, &ah);
    jpeg.setCropArea(ax, ay, W - ax < cx + p.w - ax ? W - ax : cx + p.w - ax, H - ay < cy + p.h - ay ? H - ay : cy + p.h - ay);
    jpeg.getCropArea(&ax, &ay, &aw, &ah);
    p.dx = dw / 2 - W / 2 + ax;
    p.dy = dh / 2 - H / 2 + ay;
    p.ix = ax;
    p.iy = ay;
    // Do not decode the rows above the crop if restart markers allow it: open again as
    // the smaller image that starts there, the crop moves up by as many rows
    if (int skipped = reader.skipAbove(ay)) {
      jpeg.close();
      if (!reopen()) {
        p.ok = false;
        return p;
      }
      ay -= skipped;
      jpeg.setCropArea(ax, ay, aw, ah);
    }
    p.opt = fitGroups(jpeg, W, ax, ay, aw, ah, dma);
  } else {
    // Fit: no crop, so JPEGDEC also draws the last partial group of a row
    static const int OPTS[] = {0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH};
    int i = 0;
    while (i < 3 && (W >> i > dw || H >> i > dh)) i++;
    p.w = W >> i;
    p.h = H >> i;
    p.dx = (dw - p.w) / 2;
    p.dy = (dh - p.h) / 2;
    p.opt = OPTS[i] | (dma ? JPEG_USES_DMA : 0);
  }
  p.x = (dw - p.w) / 2;
  p.y = (dh - p.h) / 2;
  return p;
}
