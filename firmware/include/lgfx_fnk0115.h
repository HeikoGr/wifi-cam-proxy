#pragma once

// LovyanGFX setup of the Freenove ESP32-S3 Display FNK0115 with the 800x480 IPS panel
// (FNK0115Q 5.0", FNK0115L 4.3"): 16-bit RGB panel with its frame buffers in PSRAM, GT911
// touch, backlight through the boost converter's EN/PWM input. Pins and timings from
// Freenove's sketches and schematic (github.com/Freenove/Freenove_ESP32_S3_Display_FNK0115).

#include <LovyanGFX.hpp>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

#include "config.h"

// LovyanGFX's frame buffer drawing on ESP-IDF's RGB driver with three frame buffers
// (LovyanGFX's own RGB driver has one, so the panel showed half-decoded frames: tearing).
// A live frame is drawn into a buffer the panel does not show (beginFrame) and shown from
// the end of the current refresh on (endFrame). Everything else draws into the shown buffer.
class Panel_RGBFlip : public lgfx::Panel_FrameBufferBase {
 public:
  static const int FBS = 3;  // one shown, one waiting for its refresh, one to draw into
  esp_lcd_rgb_panel_config_t rgb = {};

  Panel_RGBFlip() {
    _write_depth = lgfx::color_depth_t::rgb565_2Byte;
    _read_depth = lgfx::color_depth_t::rgb565_2Byte;
  }

  bool init(bool use_reset) override {
    setBus(&nullBus);  // Panel_Device::init() expects one
    if (!Panel_FrameBufferBase::init(use_reset)) return false;
    rgb.timings.h_res = _cfg.panel_width;
    rgb.timings.v_res = _cfg.panel_height;
    rgb.num_fbs = FBS;
    if (esp_lcd_new_rgb_panel(&rgb, &handle) != ESP_OK) return false;
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_vsync = onVsync;
    esp_lcd_rgb_panel_register_event_callbacks(handle, &cbs, this);
    esp_lcd_panel_reset(handle);
    esp_lcd_panel_init(handle);
    void *fb[FBS];
    esp_lcd_rgb_panel_get_frame_buffer(handle, FBS, &fb[0], &fb[1], &fb[2]);
    int h = _cfg.panel_height;
    for (int i = 0; i < FBS; i++) {
      lines[i] = (uint8_t **)heap_caps_malloc(h * sizeof(uint8_t *), MALLOC_CAP_INTERNAL);
      if (!lines[i]) return false;
      for (int y = 0; y < h; y++) lines[i][y] = (uint8_t *)fb[i] + y * _cfg.panel_width * 2;
      retiredAt[i] = -2u;
    }
    _lines_buffer = lines[front];
    return true;
  }

  void beginFrame() {
    display(0, 0, 0, 0);  // write back what went into the shown buffer
    back = (front + 1) % FBS;
    // DMA prefetch can repeat a buffer for one refresh after the switch (esp_lcd_panel_rgb.c)
    while (vsyncs - retiredAt[back] < 2) vTaskDelay(1);
    _lines_buffer = lines[back];
  }

  void endFrame() {
    display(0, 0, 0, 0);
    // A pointer into one of the driver's buffers switches to it at the end of the refresh
    esp_lcd_panel_draw_bitmap(handle, 0, 0, 1, 1, lines[back][0]);
    retiredAt[front] = vsyncs;
    front = back;
  }

 private:
  lgfx::Bus_NULL nullBus;
  esp_lcd_panel_handle_t handle = nullptr;
  uint8_t **lines[FBS] = {};
  uint32_t retiredAt[FBS];  // VSYNC count when the buffer stopped being the shown one
  volatile uint32_t vsyncs = 0;
  int front = 0, back = 0;

  static IRAM_ATTR bool onVsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *self) {
    static_cast<Panel_RGBFlip *>(self)->vsyncs++;
    return false;
  }
};

class LGFX : public lgfx::LGFX_Device {
  Panel_RGBFlip panel;
  lgfx::Light_PWM light;
  lgfx::Touch_GT911 touch;

 public:
  static const int FBS = Panel_RGBFlip::FBS;
  void beginFrame() { panel.beginFrame(); }
  void endFrame() { panel.endFrame(); }

  LGFX() {
    {
      auto cfg = panel.config();
      cfg.memory_width = cfg.panel_width = 800;
      cfg.memory_height = cfg.panel_height = 480;
      panel.config(cfg);
    }
    {
      auto &cfg = panel.rgb;
      cfg.clk_src = LCD_CLK_SRC_DEFAULT;
      cfg.data_width = 16;
      cfg.bits_per_pixel = 16;
      cfg.dma_burst_size = 64;
      cfg.flags.fb_in_psram = 1;
      // The DMA read the frame buffer straight from the PSRAM: with both cores decoding
      // into it, it fell behind every few seconds, the panel went black and came back
      // shifted. Bounce buffers in internal RAM, refilled by the CPU from the PSRAM (through
      // the cache: no write-back needed), absorb that; the driver also picks the frame
      // buffer for each refresh itself and resyncs when a refill is missing.
      cfg.bounce_buffer_size_px = 800 * 10;
      cfg.disp_gpio_num = -1;
      // Data lines d0..d4 blue, d5..d10 green, d11..d15 red. LovyanGFX keeps the pixels
      // byte-swapped (RGB565 big-endian): swap the two bytes' lines instead
      static const int8_t data[16] = {8, 3, 46, 9, 1, 5, 6, 7, 15, 16, 4, 45, 48, 47, 21, 14};
      for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data[i ^ 8];
      cfg.de_gpio_num = 40;
      cfg.vsync_gpio_num = 41;
      cfg.hsync_gpio_num = 39;
      cfg.pclk_gpio_num = 42;
      // Every refresh reads a whole frame buffer from the PSRAM (~26 MB/s at 13 MHz)
      cfg.timings.pclk_hz = FNK_PCLK_HZ;
      cfg.timings.hsync_front_porch = 4;
      cfg.timings.hsync_pulse_width = 4;
      cfg.timings.hsync_back_porch = 8;
      cfg.timings.vsync_front_porch = 4;
      cfg.timings.vsync_pulse_width = 4;
      cfg.timings.vsync_back_porch = 8;
      cfg.timings.flags.hsync_idle_low = 1;
      cfg.timings.flags.vsync_idle_low = 1;
      cfg.timings.flags.pclk_active_neg = 1;
    }
    {
      auto cfg = light.config();
      cfg.pin_bl = 2;
      cfg.freq = 20000;  // above hearing: the boost converter's coil would whistle
      cfg.pwm_channel = 7;
      light.config(cfg);
    }
    panel.setLight(&light);
    {
      auto cfg = touch.config();
      cfg.i2c_port = 1;
      cfg.pin_sda = 19;
      cfg.pin_scl = 20;
      cfg.pin_int = 18;
      cfg.pin_rst = 38;
      cfg.freq = 400000;
      cfg.x_min = 0;
      cfg.x_max = 799;
      cfg.y_min = 0;
      cfg.y_max = 479;
      touch.config(cfg);
    }
    panel.setTouch(&touch);
    setPanel(&panel);
  }
};
