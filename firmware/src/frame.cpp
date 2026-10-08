#include "frame.h"

#include <esp_heap_caps.h>
#include <esp_memory_utils.h>

#include <mutex>

#include "camera.h"

// --- Memory ---------------------------------------------------------------------
// ESP.getFreeHeap()/getMaxAllocHeap() include the IRAM remainder (MALLOC_CAP_INTERNAL).
// It is word-addressable only, so usable neither for malloc() nor for task stacks.
// Decisions and display only count byte-addressable memory (8BIT).
static const uint32_t HEAP_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
unsigned heapFree() { return heap_caps_get_free_size(HEAP_CAPS); }
unsigned heapMin() { return heap_caps_get_minimum_free_size(HEAP_CAPS); }
unsigned heapBlock() { return heap_caps_get_largest_free_block(HEAP_CAPS); }
unsigned iramFree() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - heap_caps_get_free_size(HEAP_CAPS);
}

// Frame chunks preferably live in the IRAM remainder (~44 KB), which would otherwise
// stay unused, and relieve the regular heap. IRAM only supports 32-bit accesses: hence
// copy word by word (volatile, so the compiler does not turn it into a byte memcpy).
static bool releaseIdleFrame();

uint8_t *allocChunk(size_t len, size_t frameSoFar) {
  size_t bytes = (len + 3) & ~(size_t)3;
  if (FRAME_CHUNKS_IN_PSRAM)
    if (void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM)) return (uint8_t *)p;
  for (int attempt = 0; attempt < 2; attempt++) {
    void *p = USE_IRAM_CHUNKS ? heap_caps_malloc(bytes, MALLOC_CAP_EXEC) : nullptr;  // IRAM
    // EXEC may also return RTC FAST memory: it is word-only as well and not usable from
    // core 1 at all -> keep only real IRAM, otherwise regular heap
    if (p && !esp_ptr_in_iram(p)) {
      free(p);
      p = nullptr;
    }
    // Large frames (720p microscopes) must not drain the heap, otherwise the Wi-Fi
    // driver and HTTP tasks run short. Up to FRAME_RESERVE_FROM only the smaller
    // FRAME_HEAP_FLOOR applies, which the otoscope's frames never reach.
    size_t reserve = frameSoFar >= FRAME_RESERVE_FROM ? FRAME_HEAP_RESERVE : FRAME_HEAP_FLOOR;
    if (!p && heapFree() >= reserve + bytes) p = malloc(bytes);  // IRAM full -> regular heap
    if (p) return (uint8_t *)p;
    // Not enough memory: the last published frame and the one being built do not fit
    // together (720p, 50-85 KB each). Free the stored one if nobody is using it, then
    // try once more.
    if (attempt || !releaseIdleFrame()) return nullptr;
  }
  return nullptr;
}

__attribute__((noinline)) void copyToChunk(uint8_t *dst, const uint8_t *src, size_t len) {
  if (!esp_ptr_in_iram(dst)) {
    memcpy(dst, src, len);
    return;
  }
  volatile uint32_t *d = (volatile uint32_t *)dst;
  size_t i = 0;
  for (; i + 4 <= len; i += 4) {
    uint32_t w;
    memcpy(&w, src + i, 4);
    *d++ = w;
  }
  if (i < len) {
    uint32_t w = 0;
    memcpy(&w, src + i, len - i);
    *d = w;
  }
}

__attribute__((noinline)) void copyFromChunk(uint8_t *dst, const uint8_t *chunk, size_t off, size_t len) {
  if (!esp_ptr_in_iram(chunk)) {
    memcpy(dst, chunk + off, len);
    return;
  }
  // Whole words in a loop, only the unaligned head and the tail byte by byte: collapseFill
  // and the CYD's JPEG reader pass every frame through here (MAX-VIEW: ~2 MB/s)
  const volatile uint32_t *w = (const volatile uint32_t *)chunk + off / 4;
  if (size_t b = off % 4) {
    uint32_t v = *w++;
    size_t k = min(len, 4 - b);
    memcpy(dst, (const uint8_t *)&v + b, k);
    dst += k;
    len -= k;
  }
  if (((uintptr_t)dst & 3) == 0) {
    uint32_t *d = (uint32_t *)dst;
    for (; len >= 4; len -= 4) *d++ = *w++;
    dst = (uint8_t *)d;
  } else {
    for (; len >= 4; len -= 4, dst += 4) {
      uint32_t v = *w++;
      memcpy(dst, &v, 4);
    }
  }
  if (len) {
    uint32_t v = *w;  // chunks are allocated in whole words
    memcpy(dst, &v, len);
  }
}

// --- Current frame --------------------------------------------------------------
static std::mutex frameMutex;
static Frame latestFrame;
static uint32_t frameSeq = 0;

void publishFrame(const Frame &frame) {
  if (frame.size() > stats.maxFrameBytes) stats.maxFrameBytes = frame.size();
  std::lock_guard<std::mutex> lock(frameMutex);
  latestFrame = frame;  // old frame is freed once no client is sending it any more
  frameSeq++;
  stats.framesTotal++;
}

// The last published frame, if only the store holds it: no stream viewer is sending it,
// no snapshot, no CYD decoding it (each of them holds a reference of its own). They
// get the next frame then. Called from the receiver when memory is short.
static bool releaseIdleFrame() {
  std::unique_lock<std::mutex> lock(frameMutex, std::try_to_lock);
  if (!lock.owns_lock() || !latestFrame || latestFrame.useCount() != 1) return false;
  latestFrame.reset();
  stats.framesReleased++;
  return true;
}

void clearFrame() {
  std::lock_guard<std::mutex> lock(frameMutex);
  latestFrame.reset();
}

uint32_t getFrame(Frame &out) {
  std::lock_guard<std::mutex> lock(frameMutex);
  out = latestFrame;
  return frameSeq;
}

uint32_t latestFrameSeq() {
  std::lock_guard<std::mutex> lock(frameMutex);
  return frameSeq;
}
