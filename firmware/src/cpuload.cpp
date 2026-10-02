#include "cpuload.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

static uint32_t lastAt = 0, lastIdle[2];
static std::atomic<int> load[2] = {{-1}, {-1}};

void cpuLoadUpdate() {
  // Both counters in microseconds (run time stats use esp_timer). 32 bits wrap after
  // ~71 min; the differences stay right as long as the interval is shorter.
  uint32_t now = (uint32_t)esp_timer_get_time();
  for (int core = 0; core < 2; core++) {
    uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(core);
    if (lastAt && now != lastAt)
      load[core] = constrain(100 - (int)((uint64_t)(idle - lastIdle[core]) * 100 / (now - lastAt)), 0, 100);
    lastIdle[core] = idle;
  }
  lastAt = now;
}

int cpuLoad(int core) { return core >= 0 && core < 2 ? load[core].load() : -1; }
