#pragma once

// Runtime settings in NVS (keys: table in firmware/README.md). They survive restarts and
// firmware updates.
//
//   nvsRead([&](Preferences &p) { x = p.getInt("key", x); });
//   nvsWrite([&](Preferences &p) { p.putInt("key", x); });
//
// Both return false if the namespace could not be opened; f then does not run and the
// caller keeps its defaults.

#include <Preferences.h>

#define NVS_NAMESPACE "wifi-cam"

template <class F>
bool nvsOpen(bool write, F f) {
  Preferences p;
  if (!p.begin(NVS_NAMESPACE, !write)) return false;
  f(p);
  p.end();
  return true;
}
template <class F>
bool nvsRead(F f) { return nvsOpen(false, f); }
template <class F>
bool nvsWrite(F f) { return nvsOpen(true, f); }
