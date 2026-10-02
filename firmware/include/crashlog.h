#pragma once

#include <Arduino.h>
#include <stddef.h>

// Letzter Absturz als Text ("" wenn keiner seit dem Einschalten)
void crashlogFormat(char *out, size_t len);
void crashlogInit();  // früh in setup() aufrufen

// Eine Zeile ins Neustart-feste Protokoll schreiben (printf-Format, max. 71 Zeichen)
void crumb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Absturz-Mitschnitt und Protokoll des letzten Laufs als Text (für /log)
String crashlogText();
