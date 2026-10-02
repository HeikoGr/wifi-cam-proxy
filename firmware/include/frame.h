#pragma once

// Shared frame store for all camera protocols.
//
// A frame is stored as the list of its UDP payloads (~1.3 KB each) instead of in one
// piece: no copy, no large buffer and never a large contiguous heap block, which
// fragments quickly while streaming. The receiver replaces the frame, the HTTP
// clients keep a reference and send without a lock.
// Deliberately malloc instead of new: when memory is short the frame is dropped
// instead of crashing the device via bad_alloc.

#include <Arduino.h>

#include <atomic>
#include <new>

#include "config.h"

static const int MAX_CHUNKS = MAX_FRAME_BYTES / 1024 + 1;

// Memory for a frame chunk: preferably in the IRAM remainder, else regular heap.
// Chunks beyond FRAME_RESERVE_FROM only while enough heap remains.
uint8_t *allocChunk(size_t len, size_t frameSoFar);
void copyToChunk(uint8_t *dst, const uint8_t *src, size_t len);
// Read bytes starting at off from a frame chunk (IRAM chunks word by word)
void copyFromChunk(uint8_t *dst, const uint8_t *chunk, size_t off, size_t len);

// Byte-addressable heap (without the word-only IRAM remainder)
unsigned heapFree();
unsigned heapMin();
unsigned heapBlock();
unsigned iramFree();

struct FrameBuf {
  std::atomic<int> refs;
  size_t len;
  int n;
  uint8_t *chunk[MAX_CHUNKS];
  uint16_t clen[MAX_CHUNKS];
};

class Frame {
 public:
  Frame() = default;
  Frame(const Frame &o) : p_(o.p_) { if (p_) p_->refs++; }
  Frame &operator=(const Frame &o) {
    if (o.p_) o.p_->refs++;
    reset();
    p_ = o.p_;
    return *this;
  }
  ~Frame() { reset(); }

  static Frame create() {
    Frame f;
    f.p_ = (FrameBuf *)malloc(sizeof(FrameBuf));
    if (f.p_) {
      new (&f.p_->refs) std::atomic<int>(1);
      f.p_->len = 0;
      f.p_->n = 0;
    }
    return f;
  }
  // Receiver only, while the frame is not yet published
  bool append(const uint8_t *data, size_t len) {
    if (p_->n >= MAX_CHUNKS) return false;
    uint8_t *c = allocChunk(len, p_->len);
    if (!c) return false;
    copyToChunk(c, data, len);
    p_->chunk[p_->n] = c;
    p_->clen[p_->n++] = len;
    p_->len += len;
    return true;
  }
  // Receiver only: insert as chunk number pos (0..chunks()), for packets that arrive
  // out of order
  bool insert(int pos, const uint8_t *data, size_t len) {
    if (p_->n >= MAX_CHUNKS || pos < 0 || pos > p_->n) return false;
    uint8_t *c = allocChunk(len, p_->len);
    if (!c) return false;
    copyToChunk(c, data, len);
    memmove(&p_->chunk[pos + 1], &p_->chunk[pos], (p_->n - pos) * sizeof(p_->chunk[0]));
    memmove(&p_->clen[pos + 1], &p_->clen[pos], (p_->n - pos) * sizeof(p_->clen[0]));
    p_->chunk[pos] = c;
    p_->clen[pos] = len;
    p_->n++;
    p_->len += len;
    return true;
  }
  void trimLast(size_t bytes) {  // cut off padding zeros after FF D9
    p_->clen[p_->n - 1] -= bytes;
    p_->len -= bytes;
  }
  void reset() {
    if (p_ && --p_->refs == 0) {
      for (int i = 0; i < p_->n; i++) free(p_->chunk[i]);
      free(p_);
    }
    p_ = nullptr;
  }
  explicit operator bool() const { return p_ != nullptr; }
  size_t size() const { return p_->len; }
  int chunks() const { return p_->n; }
  const uint8_t *chunk(int i) const { return p_->chunk[i]; }
  size_t chunkLen(int i) const { return p_->clen[i]; }

 private:
  FrameBuf *p_ = nullptr;
};

void publishFrame(const Frame &frame);
void clearFrame();  // on camera change: stop showing the old frame
uint32_t getFrame(Frame &out);
