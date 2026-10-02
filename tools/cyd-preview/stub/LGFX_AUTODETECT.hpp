#pragma once
// Stand-in for the CYD panel: an in-memory canvas of the panel's native size (240x320,
// portrait); setRotation() turns it like the real display.
#include <LovyanGFX.hpp>

namespace lgfx {
struct Bus_SPI : IBus {  // only for the cast in setup(); getBus() returns no bus
  struct config_t {
    uint32_t freq_write;
  };
  config_t config() const { return {}; }
  void config(const config_t &) {}
};
}  // namespace lgfx

class LGFX : public lgfx::LGFX_Sprite {
 public:
  struct PanelStub {
    lgfx::IBus *getBus() { return nullptr; }
  };
  bool init() {
    setColorDepth(16);
    return createSprite(240, 320);
  }
  PanelStub *getPanel() { return &panel_; }
  void setBrightness(uint8_t) {}
  int getTouch(lgfx::touch_point_t *) { return 0; }

 private:
  PanelStub panel_;
};
