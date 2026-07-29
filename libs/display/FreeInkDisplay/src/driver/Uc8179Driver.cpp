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
constexpr uint8_t CMD_DTM1 = 0x10;                // OLD plane in KW mode
constexpr uint8_t CMD_DTM2 = 0x13;                // NEW plane in KW mode
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;     // DRF
constexpr uint8_t CMD_PARTIAL_IN = 0x91;          // PTIN (partial refresh in)
constexpr uint8_t CMD_PARTIAL_OUT = 0x92;         // PTOUT (partial refresh out)
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
      0x3F,                      // psr0 (init): 0x3B + SHL bit2 set (mirror-X in hardware);
                                 // refresh re-asserts psr0 & 0xDF = 0x1F (OTP + SHL)
      0x0A,                      // psr1
      0x20,                      // pll (0x03)
      {0x25, 0x25, 0x3C, 0x25},  // btst (0x06 booster soft-start)
      0x02,                      // e1 (0xE1)
      0x02,                      // e0 (0xE0)
      0x1E,                      // vcomDc (0xE5) full refresh (frame-rate/temp value)
      0x5A,                      // vcomDcFast (0xE5) fast refresh — REQUIRED: this is the
                                 // frame-rate lever that makes the partial shorter (per RE)
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

// Stream a framebuffer into RAM plane `ramCmd`, mirrored vertically via row
// reversal (the same sendPlaneFlipped the UC8279 sibling uses — mirror-Y).
// Mirror-X is handled in hardware by the PSR SHL bit, so no per-byte work here.
// White padding then fills the off-screen gates (_h.._tresH); 0xFF = white.
void Uc8179Driver::streamPlane(EpdBus& bus, uint8_t ramCmd, const uint8_t* fb) {
  bus.sendPlaneFlipped(ramCmd, fb, _h, _wb);  // cmd + rows bottom-to-top, one CS burst
  uint8_t whiteRow[128];
  const uint16_t wb = _wb <= sizeof(whiteRow) ? _wb : sizeof(whiteRow);
  memset(whiteRow, 0xFF, wb);
  for (uint16_t y = _h; y < _tresH; y++) bus.data(whiteRow, wb);
}

bool Uc8179Driver::displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)prev;
  // Full OTP flash on an explicit Full request or the forced first-clear;
  // otherwise a DIFFERENTIAL partial refresh (PTIN/PTOUT). Fast additionally uses
  // the frame-rate lever (E5=0x5A + 0x03/0xE1) that shortens the waveform.
  //
  // GHOSTING FIX: the OLD plane (0x10) MUST hold the PREVIOUS displayed frame for
  // a partial, not a flat 0xFF. In KW mode the (old,new) pair selects the per-
  // pixel LUT; with old=0xFF only WW/WK fire (white-stays and white->black), so
  // KW (black->white) NEVER runs and last page's text is never erased = heavy
  // ghosting. Feeding the previous frame lets KW clear it. (0x10 is synced to the
  // just-displayed frame in displayFinish; a full refresh reseeds it to white.)
  const bool fast = (mode != RefreshMode::Full) && !_needFullClear && _oldPlaneValid;

  // NEW plane (0x13) = new frame.
  streamPlane(bus, CMD_DTM2, fb);
  if (!fast) {
    // Full flash: seed OLD plane white for the absolute GC-from-white waveform.
    uint8_t whiteRow[128];
    const uint16_t wb = _wb <= sizeof(whiteRow) ? _wb : sizeof(whiteRow);
    memset(whiteRow, 0xFF, wb);
    bus.cmd(CMD_DTM1);
    for (uint16_t y = 0; y < _tresH; y++) bus.data(whiteRow, wb);
  }
  // (Fast: OLD plane already holds the previous frame from the last displayFinish.)

  // --- Refresh setup (exact OEM order) -----------------------------------------
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiActive);  // 0x29
  bus.data(CDI_INTERVAL);
  bus.cmd(CMD_E0);
  bus.data(_cfg.e0);  // 0x02
  bus.cmd(CMD_VCOM_DC);
  bus.data(fast ? _cfg.vcomDcFast : _cfg.vcomDc);  // fast 0x5A (frame lever) / full 0x1E
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(static_cast<uint8_t>(_cfg.psr0 & 0xDF));  // REG bit cleared -> OTP
  bus.data(_cfg.psr1);
  if (fast) {
    // Fast-only: PFS/gate + cascade/active-temp. Full omits these; without them
    // the OTP waveform runs at the full frame count (same duration + garbled).
    bus.cmd(CMD_PLL);
    bus.data(_cfg.pll);  // 0x03 <- 0x20
    bus.cmd(CMD_E1);
    bus.data(_cfg.e1);  // 0xE1 <- 0x02
  }

  if (!_isScreenOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" 8179_PON");
    _isScreenOn = true;
  }

  if (fast) bus.cmd(CMD_PARTIAL_IN);  // PTIN — whole-panel partial (no 0x90 window)
  bus.cmd(CMD_DISPLAY_REFRESH);
  // Confirm the waveform started (BUSY dropped) before returning, so
  // displayFinish() only rides out the completion edge.
  {
    const int8_t busyPin = bus.pins().busy;
    const unsigned long t0 = millis();
    while (digitalRead(busyPin) == HIGH && millis() - t0 < 50) delay(1);
  }
  _pendingPartial = fast;
  _pendingTurnOff = turnOff;
  _pendingRefresh = true;
  return true;
}

void Uc8179Driver::displayFinish(EpdBus& bus, const uint8_t* fb) {
  (void)fb;
  if (!_pendingRefresh) return;
  _pendingRefresh = false;

  bus.waitRefreshComplete(" 8179_DRF");
  if (_pendingPartial) bus.cmd(CMD_PARTIAL_OUT);  // PTOUT closes the partial window
  // Restore the idle CDI (border) after the refresh, as the OEM does.
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiIdle);  // 0xA9
  bus.data(CDI_INTERVAL);

  // Sync the OLD plane (0x10) with the just-displayed frame so the NEXT partial
  // diffs against it (KW clears erased pixels -> no ghosting). This is the piece
  // that makes fast page turns clean.
  streamPlane(bus, CMD_DTM1, fb);
  _oldPlaneValid = true;
  _needFullClear = false;

  if (_pendingTurnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8179_POF");
    _isScreenOn = false;
  }
}

void Uc8179Driver::requestResync(uint8_t settlePasses) {
  (void)settlePasses;
  _needFullClear = true;  // next refresh does a full flash to clear ghosting
}

void Uc8179Driver::skipInitialResync() { _needFullClear = false; }

void Uc8179Driver::deepSleep(EpdBus& bus) {
  if (_isScreenOn) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8179 power-down");
    _isScreenOn = false;
  }
  bus.cmd(CMD_DEEP_SLEEP);
  bus.data(0xA5);
}

// --- 4-level grayscale (anti-aliasing) --------------------------------------
// Load the two bitplanes (oriented + padded like the B/W path) into controller
// RAM; displayGray() then triggers the OTP gray waveform. LSB -> 0x10 ("old"),
// MSB -> 0x13 ("new"). If the two mid grays come out swapped on hardware, swap
// the LSB/MSB plane assignment here.
void Uc8179Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  if (lsb) streamPlane(bus, CMD_DTM1, lsb);  // 0x10 = LSB / "old" plane
}

void Uc8179Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  if (msb) streamPlane(bus, CMD_DTM2, msb);  // 0x13 = MSB / "new" plane
}

void Uc8179Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                               bool factoryMode) {
  (void)fb;           // the two planes are already loaded (copyGrayscaleLsb/Msb)
  (void)lut;          // OTP gray waveform — no custom LUT (matches stock gray_full)
  (void)factoryMode;  // 4-level is absolute (defined by the planes)

  // OEM gray_full stream: E0/E5 (frame value 0x5A) + CDI 0x29 + PSR (OTP) then
  // PON/DRF over the loaded planes. TRES/PLL/BTST were set at begin().
  bus.cmd(CMD_E0);
  bus.data(_cfg.e0);  // 0x02
  bus.cmd(CMD_VCOM_DC);
  bus.data(_cfg.vcomDcFast);  // 0x5A gray frame value
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiActive);  // 0x29
  bus.data(CDI_INTERVAL);
  bus.cmd(CMD_PANEL_SETTING);
  bus.data(static_cast<uint8_t>(_cfg.psr0 & 0xDF));  // OTP (bit5=0) + SHL mirror-X
  bus.data(_cfg.psr1);

  if (!_isScreenOn) {
    bus.cmd(CMD_POWER_ON);
    bus.waitBusy(" 8179_gray_PON");
    _isScreenOn = true;
  }
  bus.cmd(CMD_DISPLAY_REFRESH);
  bus.waitBusy(" 8179_gray");
  bus.cmd(CMD_VCOM_DATA_INTERVAL);
  bus.data(_cfg.cdiIdle);  // 0xA9 restore
  bus.data(CDI_INTERVAL);

  if (turnOff) {
    bus.cmd(CMD_POWER_OFF);
    bus.waitBusy(" 8179_gray_POF");
    _isScreenOn = false;
  }
  // The panel now holds grayscale planes; the next B/W refresh must be a full
  // flash (a partial diff can't run against gray plane content).
  _needFullClear = true;
  _oldPlaneValid = false;
}

void Uc8179Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  (void)bus;
  (void)bw;
  // Gray overwrote both planes, so the differential baseline is gone — force the
  // next B/W refresh to a full flash (which reseeds the old plane).
  _needFullClear = true;
  _oldPlaneValid = false;
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
