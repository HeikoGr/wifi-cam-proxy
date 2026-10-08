#pragma once
// Stand-in for the CYD panel: an in-memory canvas of the panel's native size (240x320,
// portrait); setRotation() turns it like the real display.
#include <LovyanGFX.hpp>

#ifndef PREVIEW_PANEL_W
#define PREVIEW_PANEL_W 240
#define PREVIEW_PANEL_H 320
#endif

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
  static const int FBS = 1;
  void beginFrame() {}
  void endFrame() {}
  uint8_t **frameRows() {  // the canvas in the panel's orientation, as the frame buffer
    static uint8_t *rows[PREVIEW_PANEL_H];
    for (int y = 0; y < PREVIEW_PANEL_H; y++) rows[y] = (uint8_t *)getBuffer() + y * PREVIEW_PANEL_W * 2;
    return rows;
  }
  struct PanelStub {
    lgfx::IBus *getBus() { return nullptr; }
  };
  bool init() {
    setColorDepth(16);
    return createSprite(PREVIEW_PANEL_W, PREVIEW_PANEL_H);
  }
  PanelStub *getPanel() { return &panel_; }
  void setBrightness(uint8_t) {}
  int getTouch(lgfx::touch_point_t *) { return 0; }

 private:
  PanelStub panel_;
};
