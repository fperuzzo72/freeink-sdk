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
// lib_deps (see platformio.sample.ini's [env:m5papers3]). Defined out-of-line in
// M5PaperS3Board.cpp — LgfxEpdDriver.cpp forward-declares and calls this by name
// via the macro, so it must be a real linkable symbol, not header-inline.
const LgfxEpdConfig& m5PaperS3LgfxConfig();

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
