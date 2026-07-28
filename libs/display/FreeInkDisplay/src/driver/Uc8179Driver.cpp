#include "Uc8179Driver.h"

#include <BoardConfig.h>

namespace freeink {
namespace {
// UC8179 command set (UC8179 datasheet + OEM UC8179_800x480 init, via Ghidra).
constexpr uint8_t CMD_PANEL_SETTING = 0x00;       // PSR
constexpr uint8_t CMD_POWER_OFF = 0x02;           // POF
constexpr uint8_t CMD_PLL = 0x03;                 // PLL/OSC control
constexpr uint8_t CMD_POWER_ON = 0x04;            // PON
constexpr uint8_t CMD_BOOSTER_SOFT_START = 0x06;  // BTST
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;          // DSLP (check code 0xA5)
constexpr uint8_t CMD_DTM1 = 0x10;                // OLD plane in KW mode
constexpr uint8_t CMD_DATA_STOP = 0x11;           // DSP
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;     // DRF
constexpr uint8_t CMD_DTM2 = 0x13;                // NEW plane in KW mode
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;  // CDI
constexpr uint8_t CMD_TCON = 0x60;                // TCON
constexpr uint8_t CMD_RESOLUTION = 0x61;          // TRES
constexpr uint8_t CMD_GATE_SOURCE_START = 0x65;   // GSST
constexpr uint8_t CMD_E0 = 0xE0;                  // power/analog control
constexpr uint8_t CMD_E1 = 0xE1;                  // power/analog control
constexpr uint8_t CMD_VCOM_DC = 0xE5;             // VDCS (VCOM_DC)
}  // namespace

const Uc8179Config& uc8179DefaultConfig() {
  static const Uc8179Config cfg = {
      0x1B,                      // psr0: 0x3B & 0xDF (REG=0 -> OTP LUTs), KW mode
      0x0A,                      // psr1
      0x20,                      // pll (0x03)
      {0x25, 0x25, 0x3C, 0x25},  // btst (0x06 booster soft-start)
      0x02,                      // e1 (0xE1)
      0x02,                      // e0 (0xE0)
      0x1E,                      // vcomDc (0xE5)
      0xA9,                      // cdiFull (0x50 byte0, full refresh)
      0x29,                      // cdiFast (0x50 byte0, fast/partial refresh)
      0x22,                      // tcon (0x60)
  };
  return cfg;
}

// Resolution comes from the active BoardProfile (X4 / X4 Pro, 800x480).
Uc8179Driver::Uc8179Driver(const Uc8179Config& cfg)
    : _cfg(cfg),
      _w(BoardConfig::ACTIVE.displayWidth),
      _h(BoardConfig::ACTIVE.displayHeight),
      _wb(BoardConfig::ACTIVE.displayWidth / 8),
      _bufferSize(static_cast<uint32_t>(BoardConfig::ACTIVE.displayWidth / 8) * BoardConfig::ACTIVE.displayHeight) {}

uint32_t Uc8179Driver::spiHz() const {
  // UC8179 serial write timing is rated to 20 MHz, same as the rest of the family.
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 16000000;
}

PanelGeometry Uc8179Driver::geometry() const { return {_w, _h, _wb, _bufferSize}; }

void Uc8179Driver::initController(EpdBus& bus) {
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(_cfg.psr0);
  bus.data(_cfg.psr1);

  // TRES: HRES as 16-bit big-endian, then VRES as 16-bit big-endian (low 3 HRES
  // bits ignored — 8-pixel granular). 800x480 -> 0x03,0x20 / 0x01,0xE0.
  // NOTE (hardware validation): the OEM programs VRES=600 (0x02,0x58) and treats
  // the visible 480 as a window; if the image is shifted or compressed, try
  // pushing the panel as 800x600 there instead.
  bus.cmd(CMD_RESOLUTION);
  bus.data(static_cast<uint8_t>((_w >> 8) & 0xFF));
  bus.data(static_cast<uint8_t>(_w & 0xFF));
  bus.data(static_cast<uint8_t>((_h >> 8) & 0xFF));
  bus.data(static_cast<uint8_t>(_h & 0xFF));

  bus.cmd(CMD_GATE_SOURCE_START);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(0x00);

  // Explicit power-rail bring-up (the UC8179 needs this; the UC8279d does not):
  // PLL, booster soft-start, then the E1/CDI/E0/VCOM_DC block so the first
  // refresh has valid VCOM. LUTs stay OTP (PSR REG=0).
  bus.cmd(CMD_PLL);
  bus.data(_cfg.pll);
  bus.cmd(CMD_BOOSTER_SOFT_START);
  bus.data(_cfg.btst[0]);
  bus.data(_cfg.btst[1]);
  bus.data(_cfg.btst[2]);
  bus.data(_cfg.btst[3]);
  bus.cmd(CMD_E1);
  bus.data(_cfg.e1);
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiFull);
  bus.data(0x07);
  bus.cmd(CMD_E0);
  bus.data(_cfg.e0);
  bus.cmd(CMD_VCOM_DC);
  bus.data(_cfg.vcomDc);
  bus.cmd(CMD_TCON);
  bus.data(_cfg.tcon);

  // Fill both planes white so the first differential diffs against white, not
  // stale SRAM (same rationale as the UC8279d / UC8253 X3 driver).
  bus.fillPlane(CMD_DTM1, 0xFF, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  bus.fillPlane(CMD_DTM2, 0xFF, _h, _wb);
  bus.cmd(CMD_DATA_STOP);

  _isScreenOn = false;
}

void Uc8179Driver::begin(EpdBus& bus) {
  bus.reset(50);
  _oldPlaneSynced = false;
  _forceFullSyncNext = false;
  initController(bus);
}

void Uc8179Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  displayStart(bus, fb, prev, mode, turnOff);
  displayFinish(bus, fb);
}

bool Uc8179Driver::displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)prev;
  const bool doFullSync = (mode == RefreshMode::Full) || !_oldPlaneSynced || _forceFullSyncNext;

  if (doFullSync) {
    // Absolute write from a white OLD baseline; the OTP waveform drives every
    // pixel to target regardless of history.
    bus.fillPlane(CMD_DTM1, 0xFF, _h, _wb);
    bus.cmd(CMD_DATA_STOP);
  }
  // Differential path: DTM1 already holds the displayed frame from the last
  // displayFinish() sync, so only writing NEW is needed either way.
  bus.sendPlaneFlipped(CMD_DTM2, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);

  // Reassert CDI per refresh (full vs fast) — the OEM full/partial paths do.
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(mode == RefreshMode::Full ? _cfg.cdiFull : _cfg.cdiFast);
  bus.data(0x07);

  if (!_isScreenOn || doFullSync) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" 8179_PON");
    _isScreenOn = true;
  }
  bus.cmd(CMD_DISPLAY_REFRESH);
  // Confirm the waveform started (BUSY_N dropped LOW) before returning, so
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
  if (!_pendingRefresh) return;
  _pendingRefresh = false;

  bus.waitRefreshComplete(" 8179_DRF");
  if (_pendingTurnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8179_POF");
    _isScreenOn = false;
  }

  // Sync the OLD plane with the just-displayed frame for the next differential.
  bus.sendPlaneFlipped(CMD_DTM1, fb, _h, _wb);
  bus.cmd(CMD_DATA_STOP);
  _oldPlaneSynced = true;
  _forceFullSyncNext = false;
}

void Uc8179Driver::requestResync(uint8_t settlePasses) {
  (void)settlePasses;  // no conditioning passes on the OTP waveform path
  _forceFullSyncNext = true;
}

void Uc8179Driver::skipInitialResync() { _oldPlaneSynced = true; }

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
