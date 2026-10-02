#pragma once

// Reads a JPEG from the packet list of a frame (CYD display, src/main_cyd.cpp) without
// copying it into one piece. Optionally skips the MCU rows above the visible crop:
// the decoder has to read every row before the crop (entropy-coded data has no
// positions), which at 1:1 with the MAX-VIEW's 1280x720 image was a third of the decode
// time. With restart markers (DRI) the data of a restart interval does not depend on
// anything before it, so the reader serves the header (image height cut down) followed
// by the data after the restart marker at which the wanted row begins: for the decoder
// that is a smaller image that starts there.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "frame.h"

class FrameReader {
 public:
  void reset(const Frame &f) {
    frame_ = f;
    chunk_ = 0;
    start_ = 0;
    hdrLen_ = 0;
    skipTo_ = 0;
  }
  void release() { frame_.reset(); }
  // Size of the JPEG as the decoder sees it
  size_t size() const { return hdrLen_ ? hdrLen_ + frame_.size() - skipTo_ : frame_.size(); }

  int32_t read(size_t pos, uint8_t *buf, int32_t len) {
    int32_t done = 0;
    if (hdrLen_ && pos < hdrLen_) {  // the patched header
      done = (int32_t)(hdrLen_ - pos) < len ? (int32_t)(hdrLen_ - pos) : len;
      memcpy(buf, hdr_ + pos, done);
    }
    if (done < len) done += readRaw(hdrLen_ ? pos + done - hdrLen_ + skipTo_ : pos + done, buf + done, len - done);
    return done;
  }

  // Skip the MCU rows above row y (pixels) if the JPEG allows it. Returns the number of
  // pixel rows skipped (a multiple of the MCU height, 0 = nothing skipped); the decoder
  // must then be opened again with size() and its crop moved up by that many rows.
  int skipAbove(int y) {
    uint8_t h[HDR_MAX];
    size_t n = frame_.chunks() ? frame_.chunkLen(0) : 0;
    if (n > HDR_MAX) n = HDR_MAX;
    if (n < 4) return 0;
    copyFromChunk(h, frame_.chunk(0), 0, n);
    // Markers up to SOS: SOF0/1 (size, sampling), DRI (restart interval)
    size_t pos = 2, sof = 0, scan = 0;
    int W = 0, H = 0, hmax = 1, vmax = 1, dri = 0;
    while (pos + 4 <= n && h[pos] == 0xFF) {
      uint8_t m = h[pos + 1];
      size_t seg = h[pos + 2] << 8 | h[pos + 3];
      if (pos + 2 + seg > n) return 0;
      if (m == 0xC0 || m == 0xC1) {
        sof = pos;
        H = h[pos + 5] << 8 | h[pos + 6];
        W = h[pos + 7] << 8 | h[pos + 8];
        int comps = h[pos + 9];
        for (int c = 0; c < comps && pos + 12 + c * 3 <= n; c++) {
          int s = h[pos + 11 + c * 3];
          if ((s >> 4) > hmax) hmax = s >> 4;
          if ((s & 15) > vmax) vmax = s & 15;
        }
      } else if (m == 0xC2) {
        return 0;  // progressive: no
      } else if (m == 0xDD && seg >= 4) {
        dri = h[pos + 4] << 8 | h[pos + 5];
      } else if (m == 0xDA) {
        scan = pos + 2 + seg;
        break;
      }
      pos += 2 + seg;
    }
    if (!sof || !scan || !dri || !W || !H) return 0;
    int mw = 8 * hmax, mh = 8 * vmax;
    int perRow = (W + mw - 1) / mw;
    // Last restart interval that starts at the beginning of an MCU row at least one MCU
    // row above y: JPEGDEC smooths the chroma of the first row with the row above it, so
    // starting right at the crop gave a few wrong colours in its top rows (host test
    // jpeg_crop_test, 4:2:0 with restart every MCU row)
    long target = (long)(y / mh - 1) * perRow;
    long i = target / dri;
    while (i > 0 && (i * dri) % perRow) i--;
    if (i <= 0) return 0;
    int rows = (int)(i * dri / perRow);
    // Find the i-th RSTn marker in the scan data (the data of interval i follows it)
    size_t raw = 0;
    long found = 0;
    bool ff = false;
    for (int c = 0; c < frame_.chunks() && found < i; c++) {
      size_t len = frame_.chunkLen(c), from = c ? 0 : scan;
      uint8_t b[64];
      for (size_t o = from; o < len && found < i;) {
        size_t k = len - o < sizeof(b) ? len - o : sizeof(b);
        copyFromChunk(b, frame_.chunk(c), o, k);
        for (size_t j = 0; j < k; j++) {
          if (ff && b[j] >= 0xD0 && b[j] <= 0xD7 && ++found == i) {
            skipTo_ = raw + o + j + 1;
            break;
          }
          ff = b[j] == 0xFF;
        }
        o += k;
      }
      raw += len;
    }
    if (found < i || scan > HDR_MAX) {
      skipTo_ = 0;
      return 0;
    }
    memcpy(hdr_, h, scan);
    int newH = H - rows * mh;
    hdr_[sof + 5] = newH >> 8;
    hdr_[sof + 6] = newH & 0xFF;
    hdrLen_ = scan;
    chunk_ = 0;
    start_ = 0;
    return rows * mh;
  }

 private:
  static const size_t HDR_MAX = 1024;

  int32_t readRaw(size_t pos, uint8_t *buf, int32_t len) {
    if (pos < start_) {  // jumped back -> search from the start
      chunk_ = 0;
      start_ = 0;
    }
    int32_t done = 0;
    while (done < len) {
      while (chunk_ < frame_.chunks() && pos >= start_ + frame_.chunkLen(chunk_)) {
        start_ += frame_.chunkLen(chunk_);
        chunk_++;
      }
      if (chunk_ >= frame_.chunks()) break;
      size_t off = pos - start_;
      size_t k = frame_.chunkLen(chunk_) - off;
      if (k > (size_t)(len - done)) k = len - done;
      copyFromChunk(buf + done, frame_.chunk(chunk_), off, k);
      done += k;
      pos += k;
    }
    return done;
  }

  Frame frame_;
  int chunk_ = 0;     // chunk containing the last read position
  size_t start_ = 0;  // raw byte position at which this chunk starts
  uint8_t hdr_[HDR_MAX];
  size_t hdrLen_ = 0;  // 0 = no skipping, the JPEG as it is
  size_t skipTo_ = 0;  // raw position the data continues at after the header
};
