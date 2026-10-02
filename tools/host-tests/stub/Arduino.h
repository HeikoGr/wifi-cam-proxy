#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string>
#include <algorithm>
#include <chrono>
using std::min; using std::max;
inline unsigned long millis() {
  using namespace std::chrono;
  static auto t0 = steady_clock::now();
  return duration_cast<milliseconds>(steady_clock::now() - t0).count();
}
struct SerialStub { int printf(const char *f, ...) { va_list a; va_start(a, f); int n = vprintf(f, a); va_end(a); return n; } };
static SerialStub Serial;
typedef int portMUX_TYPE;
struct IPAddress {
  uint32_t a; IPAddress(uint32_t x) : a(x) {}
  IPAddress(int b0,int b1,int b2,int b3) : a(b0|(b1<<8)|(b2<<16)|((uint32_t)b3<<24)) {}
  operator uint32_t() const { return a; }
  std::string toString() const { char b[20]; snprintf(b,20,"%u.%u.%u.%u",a&255,(a>>8)&255,(a>>16)&255,a>>24); return b; }
};
#define portENTER_CRITICAL(x) (void)0
#define portEXIT_CRITICAL(x) (void)0
#define portMUX_INITIALIZER_UNLOCKED 0
template <class T> T constrain(T v, T lo, T hi) { return v < lo ? lo : v > hi ? hi : v; }
