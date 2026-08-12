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
      // PENDING — not a direct copy of anything CONFIRMED: M5GFX's own board-detect code sets
      // this panel's offset_rotation=3 (a different, lower-level LovyanGFX knob that this SDK
      // driver does not expose per-board), not a setRotation() value. Starting guess of 1
      // matches LilyGo T5S3 (same 540x960-native-portrait-panel-as-960x540-landscape geometry,
      // so likely the same corrective rotation) — verify on first boot and try 0..3 if the
      // image is mirrored/rotated. See docs/m5papers3-support.md.
      1,
      // No PMIC/IO-expander power sequence needed (unlike LilyGo's PCA9535+TPS65185): the EPD
      // rail is a plain GPIO (pinPwr above) that LovyanGFX's Bus_EPD drives itself.
      {nullptr, nullptr, nullptr},
  };
  return cfg;
}

}  // namespace freeink
