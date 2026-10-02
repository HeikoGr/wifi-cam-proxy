/*
 * Crash capture without a serial console: on a panic the Arduino core calls our
 * handler with a ready-made backtrace. Reason, task and backtrace are stored in RTC
 * memory, which survives a restart. Resolving the addresses:
 *   xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/zb-gw03/firmware.elf <PCs>
 */

#include <Arduino.h>

#include <new>

#include "crashlog.h"

static const uint32_t CRASH_MAGIC = 0xC0A5C0A5;
static const int MAX_DEPTH = 16;

struct CrashInfo {
  uint32_t magic;
  char reason[64];
  char task[16];
  int core;
  int depth;
  bool corrupt;
  uint32_t pcs[MAX_DEPTH];
};
RTC_NOINIT_ATTR static CrashInfo crash;

static void onPanic(arduino_panic_info_t *info, void *) {
  crash.magic = CRASH_MAGIC;
  strlcpy(crash.reason, info->reason ? info->reason : "?", sizeof(crash.reason));
  const char *task = pcTaskGetName(nullptr);
  strlcpy(crash.task, task ? task : "?", sizeof(crash.task));
  crash.core = info->core;
  crash.corrupt = info->backtrace_corrupt;
  crash.depth = min((int)info->backtrace_len, MAX_DEPTH);
  for (int i = 0; i < crash.depth; i++) crash.pcs[i] = info->backtrace[i];
}

void crashlogInit() {
  esp_reset_reason_t r = esp_reset_reason();
  // After power-on the RTC content is random; after a clean restart (e.g. update)
  // an old crash is no longer relevant
  if (r != ESP_RST_PANIC && r != ESP_RST_INT_WDT && r != ESP_RST_TASK_WDT && r != ESP_RST_WDT)
    crash.magic = 0;
  set_arduino_panic_handler(onPanic, nullptr);
}

void crashlogFormat(char *out, size_t len) {
  out[0] = 0;
  if (crash.magic != CRASH_MAGIC || crash.depth < 0 || crash.depth > MAX_DEPTH) return;
  crash.reason[sizeof(crash.reason) - 1] = 0;
  crash.task[sizeof(crash.task) - 1] = 0;
  int n = snprintf(out, len, "%s | Task %s, Core %d | Backtrace%s:", crash.reason, crash.task,
                      crash.core, crash.corrupt ? " (incomplete)" : "");
  for (int i = 0; i < crash.depth && n < (int)len; i++)
    n += snprintf(out + n, len - n, " 0x%08lx", (unsigned long)crash.pcs[i]);
  // Make it JSON-safe
  for (char *p = out; *p; p++)
    if (*p == '"' || *p == '\\' || (uint8_t)*p < 0x20) *p = ' ';
}

// --- Events ----------------------------------------------------------------------
// Serial console only: a ring buffer on the heap cost ~3 KB permanently (copy of the
// previous run) and /log up to ~9 KB while being fetched.
void crumb(const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.printf("[%lu.%03lu] %s\r\n", millis() / 1000, millis() % 1000, buf);
}
