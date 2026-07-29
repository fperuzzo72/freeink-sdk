#include "Ssd1683Driver.h"

#include <BoardConfig.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

namespace freeink {
namespace {
constexpr uint8_t CMD_SOFT_RESET = 0x12;
constexpr uint8_t CMD_WRITE_NEW = 0x24;
constexpr uint8_t CMD_WRITE_OLD = 0x26;
constexpr uint16_t WIDTH = 800;
constexpr uint16_t HEIGHT = 480;
constexpr uint16_t WIDTH_BYTES = WIDTH / 8;
constexpr uint32_t BUFFER_SIZE = static_cast<uint32_t>(WIDTH_BYTES) * HEIGHT;
constexpr uint32_t BIN_BUFFER_SIZE = static_cast<uint32_t>(WIDTH) * HEIGHT / 4;
constexpr uint16_t ROTATE_CHUNK_BYTES = 512;
constexpr uint8_t BW_MAINTENANCE_INTERVAL = 6;
constexpr uint8_t GRAY_MAINTENANCE_INTERVAL = 6;
// Large UI transitions (home/settings/file lists/dialog returns) should clean
// immediately rather than waiting for the periodic counter. Small cursor,
// clock, progress, and slider updates stay below this threshold.
constexpr uint32_t SIGNIFICANT_CHANGE_PIXELS = static_cast<uint32_t>(WIDTH) * HEIGHT / 64;
constexpr uint8_t GRAY_BASE_SCHEME_A[4] = {0, 1, 0, 1};
constexpr uint8_t GRAY_PLANE_SCHEME_A[4] = {0, 1, 2, 0};
constexpr uint8_t GRAY_BASE_SCHEME_B[4] = {0, 0, 1, 1};
constexpr uint8_t GRAY_PLANE_SCHEME_B[4] = {0, 2, 1, 0};

// SSD1683 selects LUT entry = (RAM 0x26 bit << 1) | RAM 0x24 bit. The plane
// map uses that same bit layout, so scheme B routes dark to entry 10 and light
// to entry 01. This order was verified directly on Paper Mono hardware.
static_assert(GRAY_PLANE_SCHEME_B[1] == 2, "dark gray must select LUT entry 10");
static_assert(GRAY_PLANE_SCHEME_B[2] == 1, "light gray must select LUT entry 01");

uint8_t reverseBits(uint8_t value) {
  value = static_cast<uint8_t>(((value & 0x55u) << 1) | ((value >> 1) & 0x55u));
  value = static_cast<uint8_t>(((value & 0x33u) << 2) | ((value >> 2) & 0x33u));
  return static_cast<uint8_t>((value << 4) | (value >> 4));
}

void setVsFrames(uint8_t* entry, uint8_t voltage, uint8_t frames) {
  memset(entry, 0, 10);
  if (frames > 12) frames = 12;
  for (uint8_t i = 0; i < frames; ++i) {
    entry[i >> 2] |= static_cast<uint8_t>((voltage & 0x03) << ((3 - (i & 3)) * 2));
  }
}

void setCommonGrayTail(uint8_t lut[111], uint8_t frameRate) {
  const uint8_t rate = frameRate != 0 ? frameRate : 0x8F;
  for (uint8_t i = 0; i < 5; ++i) lut[100 + i] = rate;
  lut[105] = 0x17;  // VGH
  lut[106] = 0x41;  // VSH1
  lut[107] = 0xA8;  // VSH2
  lut[108] = 0x32;  // VSL
  lut[109] = 0x30;  // VCOM
}
}  // namespace

Ssd1683Driver& ssd1683Driver() {
  static Ssd1683Driver driver;
  return driver;
}

void ssd1683SetGrayParams(const Ssd1683GrayParams& params) { ssd1683Driver().setGrayParams(params); }
void ssd1683SetFastGrayBase(bool enabled) { ssd1683Driver().setFastGrayBase(enabled); }
void ssd1683AbortGray() { ssd1683Driver().abortGray(); }
void ssd1683ResetGray() { ssd1683Driver().resetGray(); }

uint32_t Ssd1683Driver::spiHz() const {
  return BoardConfig::ACTIVE.displaySpiHz != 0 ? BoardConfig::ACTIVE.displaySpiHz : 20000000;
}

PanelGeometry Ssd1683Driver::geometry() const { return {WIDTH, HEIGHT, WIDTH_BYTES, BUFFER_SIZE}; }

bool Ssd1683Driver::allocateGrayBuffers() {
  const auto alloc = [](uint8_t*& ptr, size_t size) {
    if (!ptr) ptr = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ptr != nullptr;
  };
  const bool ok = alloc(_lastBw, BUFFER_SIZE) && alloc(_grayLsb, BUFFER_SIZE) && alloc(_grayMsb, BUFFER_SIZE) &&
                  alloc(_base, BUFFER_SIZE) && alloc(_oldEffective, BUFFER_SIZE) && alloc(_bins, BIN_BUFFER_SIZE);
  if (ok && !_grayArmed) memset(_bins, 0, BIN_BUFFER_SIZE);
  return ok;
}

void Ssd1683Driver::begin(EpdBus& bus) {
  allocateGrayBuffers();
  bus.reset();
  initController(bus);
  _needsFull = true;
  _lastBwValid = false;
  _workAbort.store(false);
  _grayAbort.store(false);
  _maintenanceAbort.store(false);
  _maintenancePending.store(false);
  _grayMaintenancePending.store(false);
  _abortedAtTwoLevelBase = false;
  _bwUpdatesSinceMaintenance = 0;
  _grayPagesSinceMaintenance = 0;
  resetGray();
}

void Ssd1683Driver::initController(EpdBus& bus) {
  bus.cmd(CMD_SOFT_RESET);
  bus.waitBusy("SSD1683 reset");

  bus.cmd(0x18);
  bus.data(0x80);
  const uint8_t softStart[] = {0xAE, 0xC7, 0xC3, 0xC0, 0x80};
  bus.cmdData(0x0C, softStart, sizeof(softStart));

  bus.cmd(0x01);
  bus.data(static_cast<uint8_t>((HEIGHT - 1) & 0xFF));
  bus.data(static_cast<uint8_t>((HEIGHT - 1) >> 8));
  bus.data(0x02);
  bus.cmd(0x3C);
  bus.data(0x01);
  bus.cmd(0x11);
  // Keep the controller in Paper Mono's validated X-/Y+ scan layout. The panel
  // mount needs this controller-side X direction in addition to writePlane()'s
  // raster transform; X+/Y+ adds a visible left/right mirror on this hardware.
  bus.data(0x02);

  bus.cmd(0x44);
  const uint16_t xStart = WIDTH - 1;
  const uint16_t xEnd = 0;
  bus.data(static_cast<uint8_t>(xStart & 0xFF));
  bus.data(static_cast<uint8_t>(xStart >> 8));
  bus.data(static_cast<uint8_t>(xEnd & 0xFF));
  bus.data(static_cast<uint8_t>(xEnd >> 8));
  bus.cmd(0x45);
  bus.data(0x00);
  bus.data(0x00);
  bus.data(static_cast<uint8_t>((HEIGHT - 1) & 0xFF));
  bus.data(static_cast<uint8_t>((HEIGHT - 1) >> 8));

  resetRamCounter(bus);
  _initialized = true;
}

void Ssd1683Driver::resetRamCounter(EpdBus& bus) {
  const uint16_t xStart = WIDTH - 1;
  bus.cmd(0x4E);
  bus.data(static_cast<uint8_t>(xStart & 0xFF));
  bus.data(static_cast<uint8_t>(xStart >> 8));
  bus.cmd(0x4F);
  bus.data(0x00);
  bus.data(0x00);
}

void Ssd1683Driver::writePlane(EpdBus& bus, uint8_t command, const uint8_t* data) {
  if (!data) return;
  resetRamCounter(bus);
  bus.cmd(command);
  const bool rotate180 = BoardConfig::ACTIVE.orientation.mirrorX && BoardConfig::ACTIVE.orientation.mirrorY;
  if (!rotate180) {
    bus.data(data, static_cast<uint16_t>(BUFFER_SIZE));
    return;
  }

  // A 180-degree 1-bpp raster rotation is a byte-order reversal plus a bit
  // reversal inside every byte. Stream it in bounded chunks so no second
  // framebuffer is required and keep CS asserted for the whole RAM write.
  uint8_t chunk[ROTATE_CHUNK_BYTES];
  bus.beginTxn();
  for (uint32_t sent = 0; sent < BUFFER_SIZE; sent += ROTATE_CHUNK_BYTES) {
    const uint16_t count = static_cast<uint16_t>(
        (BUFFER_SIZE - sent) < ROTATE_CHUNK_BYTES ? (BUFFER_SIZE - sent) : ROTATE_CHUNK_BYTES);
    for (uint16_t i = 0; i < count; ++i) {
      chunk[i] = reverseBits(data[BUFFER_SIZE - 1 - sent - i]);
    }
    bus.rawWriteBytes(chunk, count);
  }
  bus.endTxn();
}

void Ssd1683Driver::activate(EpdBus& bus, uint8_t control) {
  bus.cmd(0x22);
  bus.data(control);
  bus.cmd(0x20);
  bus.waitRefreshComplete("SSD1683 refresh");
}

void Ssd1683Driver::loadCustomLut(EpdBus& bus, const uint8_t lut[111]) {
  bus.cmd(0x32);
  bus.data(lut, 105);
  bus.cmd(0x03);
  bus.data(lut[105]);
  bus.cmd(0x04);
  bus.data(lut[106]);
  bus.data(lut[107]);
  bus.data(lut[108]);
  bus.cmd(0x2C);
  bus.data(lut[109]);
}

void Ssd1683Driver::customPass(EpdBus& bus, const uint8_t* p24, const uint8_t* p26,
                               const uint8_t lut[111]) {
  bus.cmd(0x3C);
  bus.data(0x80);
  writePlane(bus, CMD_WRITE_NEW, p24);
  writePlane(bus, CMD_WRITE_OLD, p26);
  loadCustomLut(bus, lut);
  bus.cmd(0x21);
  bus.data(0x00);
  bus.data(0x00);
  // 0xCF intentionally omits OTP LUT/temp reload bits 0x10/0x20. Including
  // either silently replaces the custom waveform and brings the flash back.
  activate(bus, 0xCF);
}

void Ssd1683Driver::cleanupChangedWhite(EpdBus& bus, const uint8_t* bw, const uint8_t* oldBw) {
  if (!bw || !oldBw || !allocateGrayBuffers()) return;
  uint32_t whitePixels = 0;
  memset(_base, 0x00, BUFFER_SIZE);
  for (uint32_t i = 0; i < BUFFER_SIZE; ++i) {
    // Cleanup entry 01 is a one-way whitening drive. Select it only where
    // old ink was black and the new frame is white; all other pixels use the
    // no-drive entry 00.
    _oldEffective[i] = static_cast<uint8_t>((~oldBw[i]) & bw[i]);
    whitePixels += static_cast<uint32_t>(__builtin_popcount(_oldEffective[i]));
  }
  if (whitePixels < 64) return;

  uint8_t lut[111];
  makeCleanupLut(lut);
  customPass(bus, _base, _oldEffective, lut);
  writePlane(bus, CMD_WRITE_NEW, bw);
  writePlane(bus, CMD_WRITE_OLD, bw);
}

void Ssd1683Driver::reinforceBw(EpdBus& bus, const uint8_t* bw) {
  if (!bw || !allocateGrayBuffers()) return;
  // Cleanup LUT entry 01 whitens and entry 10 blackens. Encoding ~bw into
  // 0x24 and bw into 0x26 selects the matching one-way drive for every pixel.
  for (uint32_t i = 0; i < BUFFER_SIZE; ++i) _base[i] = static_cast<uint8_t>(~bw[i]);
  uint8_t lut[111];
  makeCleanupLut(lut);
  customPass(bus, _base, bw, lut);

  // The cleanup planes deliberately encode transitions, not a differential
  // baseline. Re-seed both RAM planes without activating the panel.
  writePlane(bus, CMD_WRITE_NEW, bw);
  writePlane(bus, CMD_WRITE_OLD, bw);
}

void Ssd1683Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode,
                            bool turnOff) {
  (void)turnOff;
  if (!fb) return;
  if (!_initialized) {
    bus.reset();
    initController(bus);
    _needsFull = true;
  }
  allocateGrayBuffers();
  // This is the primary refresh for a newly requested frame. It supersedes
  // maintenance scheduled for the previous frame and opens a fresh cancel
  // window for any input which arrives while this refresh is running.
  _workAbort.store(false);
  _grayAbort.store(false);
  _maintenanceAbort.store(false);
  _maintenancePending.store(false);
  _grayMaintenancePending.store(false);

  if (_needsFull || mode == RefreshMode::Full) {
    // The controller RAM is unknown after EPD power-on. Encode every target
    // pixel as a definite one-way transition by using the inverse target as
    // OLD, then run the normal differential waveform. This establishes a
    // deterministic first frame without the black/white OTP full-refresh flash.
    bus.cmd(0x3C);
    bus.data(0x80);
    writePlane(bus, CMD_WRITE_NEW, fb);
    if (allocateGrayBuffers()) {
      for (uint32_t i = 0; i < BUFFER_SIZE; ++i) _base[i] = static_cast<uint8_t>(~fb[i]);
      writePlane(bus, CMD_WRITE_OLD, _base);
    }
    activate(bus, 0xFF);
    writePlane(bus, CMD_WRITE_NEW, fb);
    writePlane(bus, CMD_WRITE_OLD, fb);
    _needsFull = false;
    _panelHasGray = false;
    _bwUpdatesSinceMaintenance = 0;
  } else {
    const bool hadGray = _panelHasGray;
    const uint8_t* const oldBw = _lastBwValid ? _lastBw : prev;
    uint32_t changedPixels = 0;
    if (oldBw) {
      for (uint32_t i = 0; i < BUFFER_SIZE; ++i) {
        changedPixels += static_cast<uint32_t>(__builtin_popcount(static_cast<unsigned int>(oldBw[i] ^ fb[i])));
      }
    }
    bus.cmd(0x3C);
    bus.data(0x80);
    writePlane(bus, CMD_WRITE_NEW, fb);
    // The first pass of every page turn must be a real old->new differential
    // update. In particular, do not synthesize this plane from the new page
    // when the previous page contains gray pixels: that encodes erased black
    // text as white->white, leaving both pages visible until later cleanup
    // passes slowly bleach the old glyphs. The four-gray pass below owns gray
    // reversion after this crisp B/W page is already on screen.
    if (oldBw) {
      const uint8_t* differentialOld = oldBw;
      if (hadGray && allocateGrayBuffers()) {
        // The host baseline records the prior page's two-level precursor, but
        // its gray pixels are physically between endpoints. Force only those
        // pixels to the new page's exact precursor while preserving a real
        // old->new differential everywhere that was already pure B/W.
        memcpy(_oldEffective, oldBw, BUFFER_SIZE);
        for (uint32_t pixel = 0; pixel < static_cast<uint32_t>(WIDTH) * HEIGHT; ++pixel) {
          const uint8_t oldBin = static_cast<uint8_t>((_bins[pixel >> 2] >> (6 - 2 * (pixel & 3))) & 0x03);
          if (oldBin != 1 && oldBin != 2) continue;
          const uint8_t mask = static_cast<uint8_t>(1u << (7 - (pixel & 7)));
          if (fb[pixel >> 3] & mask) {
            _oldEffective[pixel >> 3] &= static_cast<uint8_t>(~mask);
          } else {
            _oldEffective[pixel >> 3] |= mask;
          }
        }
        differentialOld = _oldEffective;
      }
      writePlane(bus, CMD_WRITE_OLD, differentialOld);
    }
    activate(bus, 0xFF);
    writePlane(bus, CMD_WRITE_OLD, fb);
    _panelHasGray = false;

    // Ghost cleanup is intentionally not part of the foreground refresh. The
    // activity render returns as soon as the new frame is visible; the render
    // task performs one non-flashing reinforce pass in the background and can
    // drop it immediately when new input arrives.
    if (!_preparingGray) {
      const bool largeTransition = changedPixels >= SIGNIFICANT_CHANGE_PIXELS;
      const bool due = mode == RefreshMode::Half || hadGray || largeTransition ||
                       ++_bwUpdatesSinceMaintenance >= BW_MAINTENANCE_INTERVAL;
      if (due) {
        _bwUpdatesSinceMaintenance = 0;
        _maintenancePending.store(true);
      }
    }
  }

  if (_lastBw) {
    memcpy(_lastBw, fb, BUFFER_SIZE);
    _lastBwValid = true;
  }
  _grayLsbReady = false;
  _grayMsbReady = false;
}

void Ssd1683Driver::seedPreviousFrame(EpdBus& bus, const uint8_t* buf) {
  writePlane(bus, CMD_WRITE_OLD, buf);
  if (_lastBw && buf) {
    memcpy(_lastBw, buf, BUFFER_SIZE);
    _lastBwValid = true;
  }
}

void Ssd1683Driver::displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback,
                                         bool turnOff) {
  _preparingGray = true;
  display(bus, fb, nullptr, fallback, turnOff);
  _preparingGray = false;
}

void Ssd1683Driver::abortPostRefresh() {
  _workAbort.store(true);
  _grayAbort.store(true);
  _maintenanceAbort.store(true);
}

void Ssd1683Driver::runMaintenance(EpdBus& bus) {
  const bool grayCleanup = _grayMaintenancePending.exchange(false);
  const bool bwCleanup = _maintenancePending.exchange(false);
  if (!grayCleanup && !bwCleanup) return;
  if (_maintenanceAbort.exchange(false) || _workAbort.load() || !_lastBwValid) return;
  if (grayCleanup && _panelHasGray) {
    encodeCleanup();
    uint8_t customLut[111];
    makeCleanupLut(customLut);
    for (uint8_t pass = 0; pass < _grayParams.cleanupPasses && !_maintenanceAbort.load(); ++pass) {
      customPass(bus, _base, _oldEffective, customLut);
    }
    // Cleanup planes encode transition selectors, not a retained baseline.
    writePlane(bus, CMD_WRITE_NEW, _lastBw);
    writePlane(bus, CMD_WRITE_OLD, _lastBw);
  } else if (bwCleanup) {
    reinforceBw(bus, _lastBw);
    _grayPagesSinceMaintenance = 0;
  }
}

void Ssd1683Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
  if (!lsb || !allocateGrayBuffers()) return;
  // This B/W frame is becoming a grayscale page, whose own cleanup stage owns
  // the panel endpoint. A deferred pure-B/W reinforce would erase the gray.
  _maintenancePending.store(false);
  memcpy(_grayLsb, lsb, BUFFER_SIZE);
  _grayLsbReady = true;
}

void Ssd1683Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  (void)bus;
  if (!msb || !allocateGrayBuffers()) return;
  _maintenancePending.store(false);
  memcpy(_grayMsb, msb, BUFFER_SIZE);
  _grayMsbReady = true;
}

void Ssd1683Driver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows,
                                             uint16_t yStart, uint16_t numRows) {
  (void)bus;
  if (!rows || numRows == 0 || yStart + numRows > HEIGHT || !allocateGrayBuffers()) return;
  _maintenancePending.store(false);
  uint8_t* target = plane == GrayPlane::Lsb ? _grayLsb : _grayMsb;
  memcpy(target + static_cast<uint32_t>(yStart) * WIDTH_BYTES, rows,
         static_cast<uint32_t>(numRows) * WIDTH_BYTES);
  if (plane == GrayPlane::Lsb) {
    _grayLsbReady = true;
  } else {
    _grayMsbReady = true;
  }
}

uint32_t Ssd1683Driver::encodeGrayTarget() {
  const uint8_t* baseMap = _grayParams.scheme == 1 ? GRAY_BASE_SCHEME_B : GRAY_BASE_SCHEME_A;
  const uint8_t* planeMap = _grayParams.scheme == 1 ? GRAY_PLANE_SCHEME_B : GRAY_PLANE_SCHEME_A;
  uint32_t grayPixels = 0;

  for (uint32_t i = 0; i < BUFFER_SIZE; ++i) {
    const uint8_t inputLsb = _grayLsb[i];
    const uint8_t inputMsb = _grayMsb[i];
    const uint8_t bw = _lastBw[i];
    uint8_t baseByte = 0;
    uint8_t oldByte = 0;
    uint8_t gray24 = 0;
    uint8_t gray26 = 0;

    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint8_t mask = static_cast<uint8_t>(1u << (7 - bit));
      const bool lsb = (inputLsb & mask) != 0;
      const bool msb = (inputMsb & mask) != 0;
      uint8_t bin;
      if (lsb && msb) {
        bin = 1;  // dark gray
      } else if (msb) {
        bin = 2;  // light gray
      } else if (lsb) {
        bin = 1;  // tolerate a lone LSB from custom content
      } else {
        bin = (bw & mask) ? 3 : 0;
      }

      const uint32_t pixel = i * 8 + bit;
      const uint32_t binIndex = pixel >> 2;
      const uint8_t binShift = static_cast<uint8_t>(6 - 2 * (pixel & 3));
      const uint8_t oldBin = static_cast<uint8_t>((_bins[binIndex] >> binShift) & 0x03);
      const bool changed = !_grayArmed || bin != oldBin || planeMap[bin] != 0;
      const uint8_t baseBit = baseMap[bin];

      if (baseBit) baseByte |= mask;
      if (changed ? !baseBit : baseBit) oldByte |= mask;
      if (changed && planeMap[bin] != 0) {
        if (planeMap[bin] & 1) gray24 |= mask;
        if (planeMap[bin] & 2) gray26 |= mask;
        ++grayPixels;
      }
      _bins[binIndex] = static_cast<uint8_t>((_bins[binIndex] & ~(0x03u << binShift)) | (bin << binShift));
    }

    _base[i] = baseByte;
    _oldEffective[i] = oldByte;
    _grayLsb[i] = gray24;
    _grayMsb[i] = gray26;
  }
  return grayPixels;
}

void Ssd1683Driver::encodeCleanup() {
  const uint8_t* baseMap = _grayParams.scheme == 1 ? GRAY_BASE_SCHEME_B : GRAY_BASE_SCHEME_A;
  const uint8_t* planeMap = _grayParams.scheme == 1 ? GRAY_PLANE_SCHEME_B : GRAY_PLANE_SCHEME_A;
  memset(_base, 0, BUFFER_SIZE);
  memset(_oldEffective, 0, BUFFER_SIZE);

  for (uint32_t pixel = 0; pixel < static_cast<uint32_t>(WIDTH) * HEIGHT; ++pixel) {
    const uint8_t bin = static_cast<uint8_t>((_bins[pixel >> 2] >> (6 - 2 * (pixel & 3))) & 0x03);
    if (planeMap[bin] != 0) continue;
    const uint8_t mask = static_cast<uint8_t>(1u << (7 - (pixel & 7)));
    if (baseMap[bin]) {
      _oldEffective[pixel >> 3] |= mask;  // entry 01 reinforces white
    } else {
      _base[pixel >> 3] |= mask;  // entry 10 reinforces black
    }
  }
}

void Ssd1683Driver::makeGrayLut(uint8_t out[111]) const {
  memset(out, 0, 111);
  if (_grayParams.scheme == 1) {
    setVsFrames(out + 10, 0x01, _grayParams.lightFrames);  // entry 01: physical light gray
    setVsFrames(out + 20, 0x02, _grayParams.darkFrames);   // entry 10: physical dark gray
  } else {
    const uint8_t light = _grayParams.grayStrong == 0 ? 7 : _grayParams.grayStrong == 1 ? 9 : 11;
    const uint8_t dark = _grayParams.grayStrong == 0 ? 9 : _grayParams.grayStrong == 1 ? 11 : 12;
    setVsFrames(out + 10, 0x01, light);
    setVsFrames(out + 20, 0x02, dark);
    setVsFrames(out + 30, 0x02, 6);
  }
  for (uint8_t group = 0; group < 3; ++group) {
    for (uint8_t phase = 0; phase < 4; ++phase) out[50 + group * 5 + phase] = 1;
  }
  setCommonGrayTail(out, _grayParams.frameRate);
}

void Ssd1683Driver::makeFastBaseLut(uint8_t out[111]) const {
  memset(out, 0, 111);
  out[10] = 0x80;  // entry 01: VSL -> black
  out[20] = 0x40;  // entry 10: VSH1 -> white
  out[50] = 0x30;
  out[51] = 0x01;
  for (uint8_t i = 0; i < 5; ++i) out[100 + i] = 0x22;
  out[105] = 0x25;
  out[106] = 0x30;
  out[107] = 0x32;
  out[108] = 0x58;
  out[109] = 0x3A;
}

void Ssd1683Driver::makeCleanupLut(uint8_t out[111]) const {
  memset(out, 0, 111);
  setVsFrames(out + 10, 0x01, 7);
  setVsFrames(out + 20, 0x02, 7);
  for (uint8_t group = 0; group < 2; ++group) {
    for (uint8_t phase = 0; phase < 4; ++phase) out[50 + group * 5 + phase] = 1;
  }
  setCommonGrayTail(out, _grayParams.frameRate);
}

void Ssd1683Driver::displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut,
                                bool factoryMode) {
  (void)fb;
  (void)turnOff;
  (void)lut;
  (void)factoryMode;
  _maintenancePending.store(false);
  if (!_grayLsbReady || !_grayMsbReady || !allocateGrayBuffers() || !_lastBw) return;

  // displayGrayscaleBase()/displayBuffer() opened this cancellation window.
  // Do not clear it here: input arriving after the B/W base became visible
  // must be able to skip every remaining grayscale stage.
  if (_grayAbort.load() || _workAbort.load()) {
    _grayArmed = false;
    _panelHasGray = false;
    _grayLsbReady = false;
    _grayMsbReady = false;
    return;
  }
  const uint32_t grayPixels = encodeGrayTarget();
  uint8_t customLut[111];
  bool grayApplied = false;
  const bool baseAlreadyVisible = memcmp(_base, _lastBw, BUFFER_SIZE) == 0;

  if (_grayAbort.load() || _workAbort.load()) {
    _grayArmed = false;
    _panelHasGray = false;
    _grayLsbReady = false;
    _grayMsbReady = false;
    return;
  }

  // Reader paths now paint scheme B's exact black/dark vs light/white base as
  // their first visible frame. Only legacy/custom callers need this conversion
  // pass; skipping it removes one 543 ms waveform and the thick-then-thin edge.
  if (!baseAlreadyVisible) {
    if (_fastGrayBase) {
      makeFastBaseLut(customLut);
      customPass(bus, _base, _oldEffective, customLut);
    } else {
      bus.cmd(0x3C);
      bus.data(0x80);
      writePlane(bus, CMD_WRITE_NEW, _base);
      writePlane(bus, CMD_WRITE_OLD, _oldEffective);
      activate(bus, 0xFF);
    }
  }
  if (_grayParams.baseDouble && !_grayAbort.load()) {
    writePlane(bus, CMD_WRITE_NEW, _base);
    writePlane(bus, CMD_WRITE_OLD, _oldEffective);
    activate(bus, 0xFF);
  }

  if (grayPixels > 0 && !_grayAbort.load()) {
    makeGrayLut(customLut);
    customPass(bus, _grayLsb, _grayMsb, customLut);
    grayApplied = true;
  }

  if (_grayAbort.load()) {
    if (!grayApplied) {
      // The two-level base pass is already the visible endpoint. Re-seed the
      // exact physical base so the next rapid page turn differentially replaces
      // it instead of diffing against the pre-gray approximation.
      writePlane(bus, CMD_WRITE_NEW, _base);
      writePlane(bus, CMD_WRITE_OLD, _base);
      memcpy(_lastBw, _base, BUFFER_SIZE);
      _lastBwValid = true;
      _abortedAtTwoLevelBase = true;
    }
    _grayArmed = false;
    _panelHasGray = grayApplied;
  } else {
    _grayArmed = true;
    _panelHasGray = grayPixels > 0;
    // Pure B/W endpoint reinforcement is no longer a third foreground stage
    // on every page. Run it occasionally after render returns; input can drop
    // it before another waveform starts.
    if (grayPixels > 0 && _grayParams.cleanupPasses > 0 &&
        ++_grayPagesSinceMaintenance >= GRAY_MAINTENANCE_INTERVAL) {
      _grayPagesSinceMaintenance = 0;
      _grayMaintenancePending.store(true);
    }
  }
  _grayLsbReady = false;
  _grayMsbReady = false;
}

void Ssd1683Driver::displayGrayCalibration(EpdBus& bus, const uint8_t* fb, uint16_t customX, uint16_t customY,
                                           uint16_t customW, uint16_t customH) {
  if (!fb || !_grayLsbReady || !_grayMsbReady || !allocateGrayBuffers() || !_lastBw) return;

  const uint32_t xEnd = std::min<uint32_t>(static_cast<uint32_t>(customX) + customW, WIDTH);
  const uint32_t yEnd = std::min<uint32_t>(static_cast<uint32_t>(customY) + customH, HEIGHT);
  if (customX >= xEnd || customY >= yEnd) return;

  // Freeze the four-level target before encodeGrayTarget() reuses both input
  // plane buffers for its runtime transition planes.
  memcpy(_lastBw, fb, BUFFER_SIZE);
  _lastBwValid = true;
  _grayArmed = false;
  encodeGrayTarget();

  const auto binAt = [this](const uint32_t pixel) {
    return static_cast<uint8_t>((_bins[pixel >> 2] >> (6 - 2 * (pixel & 3))) & 0x03);
  };
  const auto inCustomRect = [=](const uint32_t pixel) {
    const uint32_t x = pixel % WIDTH;
    const uint32_t y = pixel / WIDTH;
    return x >= customX && x < xEnd && y >= customY && y < yEnd;
  };

  // SSD1683 OTP 4-gray format: RAM 0x24 is inverted bin LSB and RAM 0x26 is
  // inverted bin MSB. This is the vendor's flashing, accurate reference.
  memset(_base, 0xFF, BUFFER_SIZE);
  memset(_oldEffective, 0xFF, BUFFER_SIZE);
  for (uint32_t pixel = 0; pixel < static_cast<uint32_t>(WIDTH) * HEIGHT; ++pixel) {
    const uint8_t bin = binAt(pixel);
    const uint8_t mask = static_cast<uint8_t>(1u << (7 - (pixel & 7)));
    if (bin & 0x01) _base[pixel >> 3] &= static_cast<uint8_t>(~mask);
    if (bin & 0x02) _oldEffective[pixel >> 3] &= static_cast<uint8_t>(~mask);
  }

  bus.reset();
  initController(bus);
  bus.cmd(0x3C);
  bus.data(0x01);
  bus.cmd(0x1A);
  bus.data(0x5A);
  writePlane(bus, CMD_WRITE_NEW, _base);
  writePlane(bus, CMD_WRITE_OLD, _oldEffective);
  activate(bus, 0xD7);

  // Reset back to the normal controller setup without touching the ink. The
  // following passes are no-drive outside the requested comparison rectangle,
  // so the reference column remains exactly as the OTP waveform left it.
  bus.reset();
  initController(bus);

  memset(_base, 0xFF, BUFFER_SIZE);
  memset(_oldEffective, 0xFF, BUFFER_SIZE);
  for (uint32_t pixel = 0; pixel < static_cast<uint32_t>(WIDTH) * HEIGHT; ++pixel) {
    if (!inCustomRect(pixel)) continue;
    const uint8_t bin = binAt(pixel);
    const uint8_t mask = static_cast<uint8_t>(1u << (7 - (pixel & 7)));
    if (bin == 0 || bin == 1) _base[pixel >> 3] &= static_cast<uint8_t>(~mask);
    if (bin == 0 || bin == 2) _oldEffective[pixel >> 3] &= static_cast<uint8_t>(~mask);
  }

  uint8_t customLut[111];
  if (_fastGrayBase) {
    makeFastBaseLut(customLut);
    customPass(bus, _base, _oldEffective, customLut);
  } else {
    bus.cmd(0x3C);
    bus.data(0x80);
    writePlane(bus, CMD_WRITE_NEW, _base);
    writePlane(bus, CMD_WRITE_OLD, _oldEffective);
    activate(bus, 0xFF);
  }
  if (_grayParams.baseDouble) {
    writePlane(bus, CMD_WRITE_NEW, _base);
    writePlane(bus, CMD_WRITE_OLD, _oldEffective);
    activate(bus, 0xFF);
  }

  // Use the exact reading-path mapping and LUT, not a calibration-only carrier:
  //   dark  (bin 1): p24=0, p26=1 -> entry 10 -> darkFrames
  //   light (bin 2): p24=1, p26=0 -> entry 01 -> lightFrames
  // Both tones are refreshed together so the Current column always represents
  // the configured pair rather than one custom tone plus one OTP reference.
  memset(_base, 0x00, BUFFER_SIZE);
  memset(_oldEffective, 0x00, BUFFER_SIZE);
  uint32_t grayPixels = 0;
  for (uint32_t pixel = 0; pixel < static_cast<uint32_t>(WIDTH) * HEIGHT; ++pixel) {
    if (!inCustomRect(pixel)) continue;
    const uint8_t bin = binAt(pixel);
    const uint8_t mask = static_cast<uint8_t>(1u << (7 - (pixel & 7)));
    if (bin == 1) {
      _oldEffective[pixel >> 3] |= mask;
      ++grayPixels;
    } else if (bin == 2) {
      _base[pixel >> 3] |= mask;
      ++grayPixels;
    }
  }
  if (grayPixels != 0) {
    makeGrayLut(customLut);
    customPass(bus, _base, _oldEffective, customLut);
  }

  // Match the configured reading cleanup passes. They reinforce only pure
  // black/white pixels, so the two gray swatches remain a waveform comparison.
  if (_grayParams.cleanupPasses > 0) {
    memset(_base, 0x00, BUFFER_SIZE);
    memset(_oldEffective, 0x00, BUFFER_SIZE);
    uint32_t bwPixels = 0;
    for (uint32_t pixel = 0; pixel < static_cast<uint32_t>(WIDTH) * HEIGHT; ++pixel) {
      if (!inCustomRect(pixel)) continue;
      const uint8_t bin = binAt(pixel);
      const uint8_t mask = static_cast<uint8_t>(1u << (7 - (pixel & 7)));
      if (bin == 0) {
        _base[pixel >> 3] |= mask;
        ++bwPixels;
      }
      if (bin == 3) {
        _oldEffective[pixel >> 3] |= mask;
        ++bwPixels;
      }
    }
    if (bwPixels != 0) {
      makeCleanupLut(customLut);
      for (uint8_t pass = 0; pass < _grayParams.cleanupPasses; ++pass) {
        customPass(bus, _base, _oldEffective, customLut);
      }
    }
  }

  // The panel now intentionally contains two different waveform histories.
  // Force the next ordinary screen to establish a clean baseline.
  _needsFull = true;
  _lastBwValid = false;
  resetGray();
}

void Ssd1683Driver::cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) {
  if (!bw) return;
  if (_abortedAtTwoLevelBase) {
    // displayGray() already seeded the exact visible two-level base. The
    // caller is restoring its logical BW framebuffer here; replacing the
    // controller baseline with that approximation would break the next diff.
    _abortedAtTwoLevelBase = false;
    return;
  }
  // This is a controller-RAM seed only. The physical panel intentionally keeps
  // its gray levels; display() uses _bins to force those pixels back to a B/W
  // extreme before any later non-gray page.
  writePlane(bus, CMD_WRITE_OLD, bw);
  if (_lastBw) {
    memcpy(_lastBw, bw, BUFFER_SIZE);
    _lastBwValid = true;
  }
}

void Ssd1683Driver::setGrayParams(const Ssd1683GrayParams& params) {
  _grayParams = params;
  if (_grayParams.scheme > 1) _grayParams.scheme = 1;
  if (_grayParams.darkFrames == 0) _grayParams.darkFrames = 1;
  if (_grayParams.lightFrames == 0) _grayParams.lightFrames = 1;
  if (_grayParams.darkFrames > 12) _grayParams.darkFrames = 12;
  if (_grayParams.lightFrames > 12) _grayParams.lightFrames = 12;
  if (_grayParams.cleanupPasses > 3) _grayParams.cleanupPasses = 3;
  _grayParams.pipeline = 0;
}

void Ssd1683Driver::requestResync(uint8_t settlePasses) {
  (void)settlePasses;
  _needsFull = true;
  _lastBwValid = false;
  resetGray();
}

void Ssd1683Driver::resetGray() {
  _grayArmed = false;
  _panelHasGray = false;
  _grayLsbReady = false;
  _grayMsbReady = false;
  _grayAbort = false;
  _grayMaintenancePending.store(false);
  _grayPagesSinceMaintenance = 0;
}

void Ssd1683Driver::deepSleep(EpdBus& bus) {
  if (!_initialized) return;
  bus.waitBusy("SSD1683 idle");
  bus.cmd(0x10);
  bus.data(0x01);
  delay(100);
  _initialized = false;
  _needsFull = true;
  _lastBwValid = false;
  resetGray();
}

}  // namespace freeink
