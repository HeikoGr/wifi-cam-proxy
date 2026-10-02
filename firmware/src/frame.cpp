#include "frame.h"

#include <esp_heap_caps.h>
#include <esp_memory_utils.h>

#include <mutex>

#include "camera.h"

// --- Speicher -------------------------------------------------------------------
// ESP.getFreeHeap()/getMaxAllocHeap() zählen den IRAM-Rest mit (MALLOC_CAP_INTERNAL).
// Der ist nur wortweise nutzbar, also weder für malloc() noch für Task-Stacks. Für
// Entscheidungen und Anzeige zählt nur der byteweise nutzbare Speicher (8BIT).
static const uint32_t HEAP_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
unsigned heapFree() { return heap_caps_get_free_size(HEAP_CAPS); }
unsigned heapMin() { return heap_caps_get_minimum_free_size(HEAP_CAPS); }
unsigned heapBlock() { return heap_caps_get_largest_free_block(HEAP_CAPS); }
unsigned iramFree() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - heap_caps_get_free_size(HEAP_CAPS);
}

// Bildstücke liegen bevorzugt im IRAM-Rest (~44 KB), der sonst ungenutzt bleibt und
// den normalen Heap entlastet. IRAM verträgt nur 32-Bit-Zugriffe: deshalb wortweise
// kopieren (volatile, damit der Compiler daraus kein byteweises memcpy macht).
uint8_t *allocChunk(size_t len, size_t frameSoFar) {
  size_t bytes = (len + 3) & ~(size_t)3;
  void *p = USE_IRAM_CHUNKS ? heap_caps_malloc(bytes, MALLOC_CAP_EXEC) : nullptr;  // IRAM
  // EXEC liefert u.U. auch RTC-FAST-Speicher: der ist ebenfalls nur wortweise und von
  // Core 1 gar nicht nutzbar -> nur echtes IRAM behalten, sonst normaler Heap
  if (p && !esp_ptr_in_iram(p)) {
    free(p);
    p = nullptr;
  }
  if (!p) {
    // Große Bilder (Mikroskope mit 720p) dürfen den Heap nicht leer laufen lassen,
    // sonst fehlt er WLAN-Treiber und HTTP-Tasks. Bis FRAME_RESERVE_FROM gilt das
    // nicht: so bleibt das am Otoskop erprobte Verhalten unverändert.
    if (frameSoFar >= FRAME_RESERVE_FROM && heapFree() < FRAME_HEAP_RESERVE + bytes) return nullptr;
    p = malloc(bytes);  // IRAM voll -> normaler Heap
  }
  return (uint8_t *)p;
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
  const volatile uint32_t *w = (const volatile uint32_t *)chunk;
  while (len > 0) {
    uint32_t v = w[off / 4];
    size_t b = off % 4, k = min(len, 4 - b);
    memcpy(dst, (const uint8_t *)&v + b, k);
    dst += k;
    off += k;
    len -= k;
  }
}

// --- Aktuelles Bild -------------------------------------------------------------
static std::mutex frameMutex;
static Frame latestFrame;
static uint32_t frameSeq = 0;

void publishFrame(const Frame &frame) {
  if (frame.size() > stats.maxFrameBytes) stats.maxFrameBytes = frame.size();
  std::lock_guard<std::mutex> lock(frameMutex);
  latestFrame = frame;  // altes Bild wird frei, sobald kein Client es mehr sendet
  frameSeq++;
  stats.framesTotal++;
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
