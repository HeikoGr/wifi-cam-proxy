#pragma once

// Gemeinsamer Bildspeicher für alle Kamera-Protokolle.
//
// Ein Bild wird als Liste seiner UDP-Nutzdaten (je ~1,3 KB) gespeichert statt am
// Stück: keine Kopie, kein großer Puffer und nie ein großer zusammenhängender
// Heap-Block, der beim Streamen schnell zerstückelt ist. Der Empfänger ersetzt das
// Bild, die HTTP-Clients halten sich eine Referenz und senden ohne Sperre.
// Bewusst malloc statt new: bei Speichermangel wird das Bild verworfen, statt per
// bad_alloc das Gerät abstürzen zu lassen.

#include <Arduino.h>

#include <atomic>
#include <new>

#include "config.h"

static const int MAX_CHUNKS = MAX_FRAME_BYTES / 1024 + 1;

// Speicher für ein Bildstück: bevorzugt im IRAM-Rest, sonst normaler Heap.
// Stücke jenseits von FRAME_RESERVE_FROM nur, solange genug Heap übrig bleibt.
uint8_t *allocChunk(size_t len, size_t frameSoFar);
void copyToChunk(uint8_t *dst, const uint8_t *src, size_t len);
// Bytes ab off aus einem Bildstück lesen (IRAM-Stücke wortweise)
void copyFromChunk(uint8_t *dst, const uint8_t *chunk, size_t off, size_t len);

// Byteweise nutzbarer Heap (ohne den nur wortweise nutzbaren IRAM-Rest)
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
  // Nur vom Empfänger, solange das Bild noch nicht veröffentlicht ist
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
  void trimLast(size_t bytes) {  // Füll-Nullen nach FF D9 abschneiden
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
void clearFrame();  // beim Kamerawechsel: altes Bild nicht weiter zeigen
uint32_t getFrame(Frame &out);
