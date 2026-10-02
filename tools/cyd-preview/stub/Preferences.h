#pragma once
// No NVS on the host: begin() fails, so the defaults stay
struct Preferences {
  bool begin(const char *, bool) { return false; }
  void end() {}
  uint8_t getUChar(const char *, uint8_t d) { return d; }
  bool getBool(const char *, bool d) { return d; }
  void putUChar(const char *, uint8_t) {}
  void putBool(const char *, bool) {}
};
