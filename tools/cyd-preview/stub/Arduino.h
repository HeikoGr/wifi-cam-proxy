#pragma once
// The host tests' Arduino stand-in plus what main_cyd.cpp uses besides
#include_next <Arduino.h>
#include <atomic>
#include <new>
#define OUTPUT 1
#define HIGH 1
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline uint32_t micros() { return millis() * 1000; }
struct SerialCyd : SerialStub {
  void begin(int) {}
  void println(const char *s) { ::printf("%s\n", s); }
};
static SerialCyd SerialCydStub;
#define Serial SerialCydStub
// FreeRTOS
#define pdMS_TO_TICKS(ms) (ms)
inline void vTaskDelay(int ms) { delay(ms); }
inline int xTaskCreatePinnedToCore(void (*)(void *), const char *, int, void *, int, void *, int) { return 1; }
inline void heap_caps_monitor_local_minimum_free_size_start() {}
inline void heap_caps_monitor_local_minimum_free_size_stop() {}
