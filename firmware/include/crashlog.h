#pragma once

#include <Arduino.h>
#include <stddef.h>

// Last crash as text ("" if none since power-on)
void crashlogFormat(char *out, size_t len);
void crashlogInit();  // call early in setup()

// Write an event to the serial console (printf format)
void crumb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
