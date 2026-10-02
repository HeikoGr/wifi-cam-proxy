#pragma once
// Stand-in: no IRAM on the host; the free heap is a budget minus what the test has
// allocated (glibc mallinfo2), so allocChunk's memory rules can be checked.
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_EXEC (1 << 0)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT (1 << 2)
extern size_t heapBudget, heapBaseline;
inline void *heap_caps_malloc(size_t, uint32_t) { return nullptr; }
inline size_t heap_caps_get_free_size(uint32_t) {
  size_t used = mallinfo2().uordblks - heapBaseline;
  return used < heapBudget ? heapBudget - used : 0;
}
inline size_t heap_caps_get_minimum_free_size(uint32_t c) { return heap_caps_get_free_size(c); }
inline size_t heap_caps_get_largest_free_block(uint32_t c) { return heap_caps_get_free_size(c); }
