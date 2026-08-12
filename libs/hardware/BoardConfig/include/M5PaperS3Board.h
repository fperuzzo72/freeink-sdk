#pragma once

// M5Stack PaperS3 (official, shop.m5stack.com) — board-specific glue that the
// generic BoardConfig::BoardProfile can't carry (the raw-parallel EPD bus wiring,
// and the board's non-standard power-off sequence).
//
// Every pin below is sourced from the official, MIT-licensed m5stack/M5Unified
// 0.2.10 and m5stack/M5GFX 0.2.15 — see docs/m5papers3-support.md for the exact
// file/line evidence and the CONFIRMED/PENDING breakdown. This board is NOT the
// same hardware as the SDK's "Paper Mono" profile (BoardConfig.h) despite the
// similar name; do not mix pins between the two.

#include <Arduino.h>

#include "LgfxEpdConfig.h"

namespace freeink {

// Raw-parallel EPD bus config for FREEINK_DRIVER_LGFX_EPD (LgfxEpdDriver). Build
// with -DFREEINK_LGFX_EPD_CONFIG=m5PaperS3LgfxConfig and add m5stack/M5GFX to
// lib_deps (see platformio.sample.ini's [env:m5papers3]).
inline const LgfxEpdConfig& m5PaperS3LgfxConfig() {
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

// Board-specific power-off. CONFIRMED from M5Unified's Power_Class.cpp: M5PaperS3
// doesn't turn off on a simple GPIO level like most boards (the generic
// BoardConfig::PowerConfig hold-latch model doesn't fit) — it needs GPIO44
// (PWROFF_PULSE_PIN) pulsed LOW/HIGH five times, 50 ms each edge, before the
// board's own power circuit lets go. Call this from the consumer's deep-sleep /
// power-off path instead of the generic latch release.
namespace m5papers3 {

inline void powerOff() {
  pinMode(44, OUTPUT);
  for (int i = 0; i < 5; ++i) {
    digitalWrite(44, LOW);
    delay(50);
    digitalWrite(44, HIGH);
    delay(50);
  }
}

}  // namespace m5papers3
}  // namespace freeink
