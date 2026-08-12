#include "M5PaperS3Board.h"

namespace freeink {

const LgfxEpdConfig& m5PaperS3LgfxConfig() {
  static const LgfxEpdConfig cfg = {
      // 8-bit parallel data bus D0..D7 — CONFIRMED (M5GFX.cpp board_M5PaperS3 bus_cfg.pin_data[0..7]).
      {6, 14, 7, 12, 9, 11, 8, 10},
      13,  // pinSph (start pulse horizontal) — CONFIRMED
      17,  // pinSpv (start pulse vertical)   — CONFIRMED
      45,  // pinOe  (output enable)          — CONFIRMED
      15,  // pinLe  (latch enable)           — CONFIRMED
      16,  // pinCl  (clock)                  — CONFIRMED
      18,  // pinCkv (clock vertical)         — CONFIRMED
      46,  // pinPwr (EPD rail enable, driven by LovyanGFX's own Bus_EPD::powerControl) — CONFIRMED
      16'000'000,  // busHz — CONFIRMED (M5GFX bus_cfg.bus_speed)
      8,           // linePadding — CONFIRMED (M5GFX cfg_detail.line_padding)
      // rotation: LovyanGFX setRotation() value LgfxEpdDriver applies via g_dev.setRotation().
      // CONFIRMED — verified on real M5PaperS3 hardware. Two earlier attempts (rotation=1,
      // then 3) both came out with the framebuffer's width/height effectively swapped (image
      // filled only ~half the physical landscape panel). Root cause: LovyanGFX's rotation math
      // (Panel_HasBuffer.cpp / Panel_FrameBufferBase.cpp):
      //   _internal_rotation = ((r + offset_rotation) & 3) | ((r & 4) ^ (offset_rotation & 4))
      //   if (_internal_rotation & 1) std::swap(_width, _height);
      // — any ODD r swaps width/height when LgfxEpdDriver's fixed offset_rotation=0 is used, and
      // 1 and 3 are both odd. The official M5Stack demo (m5stack/M5PaperS3-UserDemo,
      // main/hal/hal.cpp: `M5.begin(); M5.Display.setRotation(1);`) calls setRotation(1) ON TOP
      // OF the board's baked-in offset_rotation=3 — combined: ((1+3)&3)|((1&4)^(3&4)) = 0, an
      // EVEN internal_rotation (no swap). With our driver's offset_rotation=0, the r that
      // reproduces that same internal_rotation=0 is simply r=0.
      0,
      // No PMIC/IO-expander power sequence needed (unlike LilyGo's PCA9535+TPS65185): the EPD
      // rail is a plain GPIO (pinPwr above) that LovyanGFX's Bus_EPD drives itself.
      {nullptr, nullptr, nullptr},
  };
  return cfg;
}

}  // namespace freeink
