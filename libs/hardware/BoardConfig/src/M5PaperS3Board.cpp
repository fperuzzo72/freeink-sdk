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
      // CONFIRMED — verified on real M5PaperS3 hardware (first attempt at rotation=1 came out
      // 90 degrees off with part of the screen clipped). LovyanGFX's rotation math
      // (Panel_FrameBufferBase.cpp / Panel_HasBuffer.cpp):
      //   _internal_rotation = ((r + offset_rotation) & 3) | ((r & 4) ^ (offset_rotation & 4))
      // is additive in r and offset_rotation, so with LgfxEpdDriver's fixed offset_rotation=0,
      // setRotation(3) produces the identical _internal_rotation as M5GFX's own board-detect
      // code, which sets this exact panel's offset_rotation=3 with setRotation(0) — i.e.
      // rotation=3 here is the exact equivalent, not a guess.
      3,
      // No PMIC/IO-expander power sequence needed (unlike LilyGo's PCA9535+TPS65185): the EPD
      // rail is a plain GPIO (pinPwr above) that LovyanGFX's Bus_EPD drives itself.
      {nullptr, nullptr, nullptr},
  };
  return cfg;
}

}  // namespace freeink
