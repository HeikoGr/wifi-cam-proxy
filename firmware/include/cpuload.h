#pragma once

// CPU load per core in percent, from the time the idle task of each core ran (FreeRTOS
// run time statistics, on in the Arduino core's ESP-IDF configuration). cpuLoadUpdate()
// measures the interval since its previous call; call it from one place every few
// seconds (loop(), with the [stats] line).

void cpuLoadUpdate();
int cpuLoad(int core);  // 0..100, -1 = not measured yet
