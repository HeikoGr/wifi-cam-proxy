#pragma once

#include <Arduino.h>
#include <stddef.h>

// Letzter Absturz als Text ("" wenn keiner seit dem Einschalten)
void crashlogFormat(char *out, size_t len);
void crashlogInit();  // früh in setup() aufrufen

// Ereignis auf die serielle Konsole schreiben (printf-Format)
void crumb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

