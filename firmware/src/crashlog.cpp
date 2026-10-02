/*
 * Absturz-Mitschnitt ohne serielle Konsole: Der Arduino-Core ruft bei einem Panic
 * unseren Handler mit fertigem Backtrace auf. Grund, Task und Backtrace landen im
 * RTC-Speicher, der einen Neustart übersteht. Auflösen der Adressen:
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

static void crumbsInit();

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
  // Nach Einschalten ist der RTC-Inhalt Zufall; nach sauberem Neustart (z.B. Update)
  // ist ein alter Absturz nicht mehr relevant
  if (r != ESP_RST_PANIC && r != ESP_RST_INT_WDT && r != ESP_RST_TASK_WDT && r != ESP_RST_WDT)
    crash.magic = 0;
  set_arduino_panic_handler(onPanic, nullptr);
  crumbsInit();
}

void crashlogFormat(char *out, size_t len) {
  out[0] = 0;
  if (crash.magic != CRASH_MAGIC || crash.depth < 0 || crash.depth > MAX_DEPTH) return;
  crash.reason[sizeof(crash.reason) - 1] = 0;
  crash.task[sizeof(crash.task) - 1] = 0;
  int n = snprintf(out, len, "%s | Task %s, Core %d | Backtrace%s:", crash.reason, crash.task,
                      crash.core, crash.corrupt ? " (unvollständig)" : "");
  for (int i = 0; i < crash.depth && n < (int)len; i++)
    n += snprintf(out + n, len - n, " 0x%08lx", (unsigned long)crash.pcs[i]);
  // JSON-sicher machen
  for (char *p = out; *p; p++)
    if (*p == '"' || *p == '\\' || (uint8_t)*p < 0x20) *p = ' ';
}

// --- Protokoll der letzten Ereignisse (überlebt Neustarts) ------------------------
static const int CRUMB_COUNT = 40;
static const int CRUMB_LEN = 72;
struct CrumbRing {
  uint32_t magic;
  uint32_t next;
  uint32_t ms[CRUMB_COUNT];
  char msg[CRUMB_COUNT][CRUMB_LEN];
};
RTC_NOINIT_ATTR static CrumbRing ring;
static CrumbRing *prevRing = nullptr;  // Kopie aus dem letzten Lauf
static portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;

void crumb(const char *fmt, ...) {
  char buf[CRUMB_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  portENTER_CRITICAL(&ringMux);
  uint32_t i = ring.next % CRUMB_COUNT;
  ring.ms[i] = millis();
  memcpy(ring.msg[i], buf, CRUMB_LEN);
  ring.next++;
  portEXIT_CRITICAL(&ringMux);
}

static void crumbsInit() {
  if (esp_reset_reason() != ESP_RST_POWERON && ring.magic == CRASH_MAGIC) {
    prevRing = new (std::nothrow) CrumbRing(ring);
  }
  memset(&ring, 0, sizeof(ring));
  ring.magic = CRASH_MAGIC;
}

static void appendRing(String &out, CrumbRing *r) {
  char line[CRUMB_LEN + 16];
  uint32_t n = min(r->next, (uint32_t)CRUMB_COUNT);
  for (uint32_t k = 0; k < n; k++) {
    uint32_t i = (r->next - n + k) % CRUMB_COUNT;
    r->msg[i][CRUMB_LEN - 1] = 0;
    snprintf(line, sizeof(line), "%8.3f s  %s\n", r->ms[i] / 1000.0, r->msg[i]);
    out += line;
  }
}

String crashlogText() {
  String out;
  char line[320];
  crashlogFormat(line, sizeof(line));
  out += "Reset-Grund: ";
  out += (int)esp_reset_reason();
  out += "\nAbsturz-Mitschnitt: ";
  out += line[0] ? line : "(keiner)";
  out += "\n\nAktueller Lauf (letzte Ereignisse):\n";
  CrumbRing *now = new (std::nothrow) CrumbRing;
  if (now) {
    portENTER_CRITICAL(&ringMux);
    memcpy(now, &ring, sizeof(ring));
    portEXIT_CRITICAL(&ringMux);
    appendRing(out, now);
    delete now;
  }
  out += "\nVor dem letzten Neustart:\n";
  if (prevRing) appendRing(out, prevRing);
  else out += "(nichts, z.B. nach Einschalten)\n";
  return out;
}
