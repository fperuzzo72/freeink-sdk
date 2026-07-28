#include "Uc8179Driver.h"

#include <Arduino.h>

#include <string.h>

#include <BoardConfig.h>

namespace freeink {
namespace {
// UC8179 command set (UC8179 datasheet + OEM UC8179_800x480 stream, via Ghidra).
constexpr uint8_t CMD_PANEL_SETTING = 0x00;       // PSR
constexpr uint8_t CMD_POWER_OFF = 0x02;           // POF
constexpr uint8_t CMD_PLL = 0x03;                 // PLL/OSC control
constexpr uint8_t CMD_POWER_ON = 0x04;            // PON
constexpr uint8_t CMD_BOOSTER_SOFT_START = 0x06;  // BTST
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;          // DSLP (check code 0xA5)
constexpr uint8_t CMD_DTM2 = 0x13;                // NEW plane in KW mode
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;     // DRF
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;  // CDI
constexpr uint8_t CMD_RESOLUTION = 0x61;          // TRES
constexpr uint8_t CMD_GATE_SOURCE_START = 0x65;   // GSST
constexpr uint8_t CMD_E0 = 0xE0;                  // power/analog control
constexpr uint8_t CMD_E1 = 0xE1;                  // power/analog control
constexpr uint8_t CMD_VCOM_DC = 0xE5;             // VDCS (VCOM_DC)

constexpr uint8_t CDI_INTERVAL = 0x07;  // CDI byte1, constant
}  // namespace

const Uc8179Config& uc8179DefaultConfig() {
  static const Uc8179Config cfg = {
      0x3B,                      // psr0 (init); refresh re-asserts psr0 & 0xDF = 0x1B (OTP)
      0x0A,                      // psr1
      0x20,                      // pll (0x03)
      {0x25, 0x25, 0x3C, 0x25},  // btst (0x06 booster soft-start)
      0x02,                      // e1 (0xE1)
      0x02,                      // e0 (0xE0)
      0x1E,                      // vcomDc (0xE5)
      0x29,                      // cdiActive (0x50, during refresh)
      0xA9,                      // cdiIdle (0x50, restored after)
      600,                       // tresHeight — panel addressed 800x600 (480 visible)
  };
  return cfg;
}

// Visible geometry comes from the active BoardProfile (X4 / X4 Pro, 800x480).
Uc8179Driver::Uc8179Driver(const Uc8179Config& cfg)
    : _cfg(cfg),
      _w(BoardConfig::ACTIVE.displayWidth),
      _h(BoardConfig::ACTIVE.displayHeight),
      _wb(BoardConfig::ACTIVE.displayWidth / 8),
      _tresH(cfg.tresHeight),
      _bufferSize(static_cast<uint32_t>(BoardConfig::ACTIVE.displayWidth / 8) * BoardConfig::ACTIVE.displayHeight) {}

uint32_t Uc8179Driver::spiHz() const {
  // UC8179 serial write timing is rated to 20 MHz, same as the rest of the family.
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 16000000;
}

PanelGeometry Uc8179Driver::geometry() const { return {_w, _h, _wb, _bufferSize}; }

// The OEM init (FUN_4214dff8): PSR, TRES (800x600), GSST, PLL, BTST, E1. No plane
// fill, no CDI/VCOM here — those are (re)asserted per refresh. OTP waveforms
// (PSR REG bit cleared at refresh), so no LUT upload.
void Uc8179Driver::initController(EpdBus& bus) {
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);
  bus.data(_cfg.psr1);

  // TRES: HRES (16-bit BE) then VRES (16-bit BE). Width from the visible geometry
  // (800 -> 0x03,0x20), height is the addressed gate count (600 -> 0x02,0x58).
  bus.cmd(CMD_RESOLUTION);
  bus.data(static_cast<uint8_t>((_w >> 8) & 0xFF));
  bus.data(static_cast<uint8_t>(_w & 0xFF));
  bus.data(static_cast<uint8_t>((_tresH >> 8) & 0xFF));
  bus.data(static_cast<uint8_t>(_tresH & 0xFF));

  bus.cmd(CMD_GATE_SOURCE_START);
  bus.data(0x00);
  bus.data(0x00);

  bus.cmd(CMD_PLL);
  bus.data(_cfg.pll);

  bus.cmd(CMD_BOOSTER_SOFT_START);
  bus.data(_cfg.btst[0]);
  bus.data(_cfg.btst[1]);
  bus.data(_cfg.btst[2]);
  bus.data(_cfg.btst[3]);

  bus.cmd(CMD_E1);
  bus.data(_cfg.e1);

  _isScreenOn = false;
}

void Uc8179Driver::begin(EpdBus& bus) {
  bus.reset(50);
  initController(bus);
}

void Uc8179Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  displayStart(bus, fb, prev, mode, turnOff);
  displayFinish(bus, fb);
}

bool Uc8179Driver::displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)prev;
  (void)mode;  // every refresh is a full OTP flash for now (no partial LUT path yet)

  // --- DTM2 (0x13): framebuffer + white padding to the 800x600 gate count ------
  static uint8_t whiteRow[128];
  static bool whiteInit = false;
  if (!whiteInit) {
    memset(whiteRow, 0xFF, sizeof(whiteRow));
    whiteInit = true;
  }
  bus.cmd(CMD_DTM2);
  // Visible rows straight through (facade framebuffer is already panel-oriented,
  // 0xFF = white — same convention fillPlane/sendPlane use).
  bus.data(fb, static_cast<uint16_t>(_h) * _wb);
  // Off-screen padding rows (tresHeight - visibleHeight), white.
  for (uint16_t y = _h; y < _tresH; y++) bus.data(whiteRow, _wb);

  // --- refresh voltages, matching the OEM full path ----------------------------
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiActive);
  bus.data(CDI_INTERVAL);
  bus.cmd(CMD_E0);
  bus.data(_cfg.e0);
  bus.cmd(CMD_VCOM_DC);
  bus.data(_cfg.vcomDc);
  // Re-assert PSR with the REG bit cleared (OTP waveform) right before power-on.
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(static_cast<uint8_t>(_cfg.psr0 & 0xDF));
  bus.data(_cfg.psr1);

  bus.cmd(CMD_POWER_ON);
  bus.waitBusy(" 8179_PON");
  _isScreenOn = true;

  bus.cmd(CMD_DISPLAY_REFRESH);
  // Confirm the waveform started (BUSY dropped) before returning, so
  // displayFinish() only rides out the completion edge.
  {
    const int8_t busyPin = bus.pins().busy;
    const unsigned long t0 = millis();
    while (digitalRead(busyPin) == HIGH && millis() - t0 < 50) delay(1);
  }
  _pendingTurnOff = turnOff;
  _pendingRefresh = true;
  return true;
}

void Uc8179Driver::displayFinish(EpdBus& bus, const uint8_t* fb) {
  (void)fb;
  if (!_pendingRefresh) return;
  _pendingRefresh = false;

  bus.waitRefreshComplete(" 8179_DRF");
  // Restore the idle CDI (border) after the refresh, as the OEM does.
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiIdle);
  bus.data(CDI_INTERVAL);

  if (_pendingTurnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8179_POF");
    _isScreenOn = false;
  }
}

void Uc8179Driver::requestResync(uint8_t settlePasses) {
  (void)settlePasses;  // absolute OTP waveform; every frame is a full write
}

void Uc8179Driver::skipInitialResync() {}

void Uc8179Driver::deepSleep(EpdBus& bus) {
  if (_isScreenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8179 power-down");
    _isScreenOn = false;
  }
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0xA5);
}

// Per-board config injection, same idiom as the other drivers: define
// `const Uc8179Config& yourConfig();` in namespace freeink and build with
// -DFREEINK_UC8179_CONFIG=yourConfig.
#ifdef FREEINK_UC8179_CONFIG
const Uc8179Config& FREEINK_UC8179_CONFIG();
static const Uc8179Config& uc8179ActiveConfig() { return FREEINK_UC8179_CONFIG(); }
#else
static const Uc8179Config& uc8179ActiveConfig() { return uc8179DefaultConfig(); }
#endif

PanelDriver& uc8179Driver() {
  static Uc8179Driver instance(uc8179ActiveConfig());
  return instance;
}

}  // namespace freeink
