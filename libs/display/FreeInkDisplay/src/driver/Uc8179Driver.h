#pragma once

// UC8179 panel driver — Xteink X4 / X4 Pro, newer production run (800x480 B/W).
// UltraChip UC8179 driven in KW mode (PSR KW/R=1): 1-bpp, DTM1 = OLD plane,
// DTM2 = NEW plane, differential refresh — the same KW paradigm and command set
// as the UC8279d X3 driver. It is a *separate* driver because the UC8179 needs
// an explicit PLL / booster / VCOM bring-up that the UC8279d (pure-OTP) omits:
// on OTP defaults alone the UC8179 never develops an image.
//
// Recovered from the X4 Pro OEM firmware (UC8179_800x480 init FUN_4214dff8 /
// full-update FUN_4214e584, via Ghidra). It runs the factory OTP waveforms
// (PSR REG=0) — the MTP holds temperature-compensated LUT sets — so no custom
// LUT upload is needed; only the power rails are programmed here. PENDING
// HARDWARE VALIDATION on a UC8179 (screenType=1 / hw_calib=2) X4 / X4 Pro unit.
//
// BUSY_N: low while busy (PON/DRF/POF all flag), same two-phase shape as the
// UC8279d / UC8253 X3 — reuses BusyPolarity::X3TwoPhase and the async split.

#include "PanelDriver.h"

namespace freeink {

struct Uc8179Config {
  // PSR (0x00) byte 0: 0x1B = 0x3B & 0xDF (REG bit cleared -> OTP LUTs), KW mode,
  // orientation/scan bits at panel default.
  uint8_t psr0;
  // PSR (0x00) byte 1.
  uint8_t psr1;
  // PLL/OSC control (cmd 0x03).
  uint8_t pll;
  // Booster soft-start (cmd 0x06), 4 bytes.
  uint8_t btst[4];
  // cmd 0xE1 (power/analog control tail).
  uint8_t e1;
  // cmd 0xE0.
  uint8_t e0;
  // VCOM_DC (cmd 0xE5).
  uint8_t vcomDc;
  // CDI (0x50) byte0: full refresh vs fast/partial. byte1 is always 0x07.
  uint8_t cdiFull;
  uint8_t cdiFast;
  // TCON (0x60): S2G/G2S non-overlap.
  uint8_t tcon;
};

const Uc8179Config& uc8179DefaultConfig();

class Uc8179Driver : public PanelDriver {
 public:
  explicit Uc8179Driver(const Uc8179Config& cfg = uc8179DefaultConfig());

  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::X3TwoPhase; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;

  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  bool displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  void displayFinish(EpdBus& bus, const uint8_t* fb) override;
  bool supportsAsyncDisplay() const override { return true; }

  void requestResync(uint8_t settlePasses) override;
  void skipInitialResync() override;

 private:
  void initController(EpdBus& bus);

  const Uc8179Config& _cfg;

  uint16_t _w;
  uint16_t _h;
  uint16_t _wb;
  uint32_t _bufferSize;

  bool _isScreenOn = false;
  bool _oldPlaneSynced = false;
  bool _forceFullSyncNext = false;

  // Async split state (see Uc8279Driver for the contract).
  bool _pendingRefresh = false;
  bool _pendingTurnOff = false;
};

PanelDriver& uc8179Driver();

}  // namespace freeink
