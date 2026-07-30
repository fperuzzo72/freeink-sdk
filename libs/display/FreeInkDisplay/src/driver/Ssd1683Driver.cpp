#include "Ssd1683Driver.h"

#include <BoardConfig.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <array>
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
constexpr uint16_t ROTATE_CHUNK_BYTES = 16384;
// SSD1683 Display Update Control 2 bits. 0xFC performs the Paper Mono OTP
// B/W update but intentionally omits ANALOG_OFF/CLOCK_OFF. Subsequent custom
// passes can then use 0x0C while the rails remain ready; 0x03 is issued only
// after ActivityManager proves the controller queue is empty.
constexpr uint8_t CTRL_OTP_BW_HOLD = 0xFC;
constexpr uint8_t CTRL_CUSTOM_HOLD_COLD = 0xCC;
constexpr uint8_t CTRL_DISPLAY_HOLD_WARM = 0x0C;
constexpr uint8_t CTRL_POWER_OFF = 0x03;
constexpr uint8_t GRAY_DEGHOST_INTERVAL = 6;
// Paper Mono's 180-degree mount requires a reversed staging buffer. Keep one
// controller-worker-owned block in internal RAM: larger bursts cut the Arduino
// SPI transaction setup count from 94 to 3 per plane without consuming the
// render task's 8 KB stack.
DRAM_ATTR uint8_t ROTATE_CHUNK[ROTATE_CHUNK_BYTES];
constexpr uint8_t GRAY_BASE_SCHEME_A[4] = {0, 1, 0, 1};
constexpr uint8_t GRAY_PLANE_SCHEME_A[4] = {0, 1, 2, 0};
constexpr uint8_t GRAY_BASE_SCHEME_B[4] = {0, 0, 1, 1};
constexpr uint8_t GRAY_PLANE_SCHEME_B[4] = {0, 2, 1, 0};

struct PackedBinMasks {
  uint8_t high = 0;
  uint8_t low = 0;
  uint8_t gray = 0;
};

constexpr std::array<PackedBinMasks, 256> makePackedBinMaskLut() {
  std::array<PackedBinMasks, 256> lut{};
  for (uint16_t packed = 0; packed < 256; ++packed) {
    for (uint8_t pixel = 0; pixel < 4; ++pixel) {
      const uint8_t bin = static_cast<uint8_t>((packed >> (6 - 2 * pixel)) & 0x03);
      const uint8_t mask = static_cast<uint8_t>(1u << (3 - pixel));
      if (bin & 0x02) lut[packed].high |= mask;
      if (bin & 0x01) lut[packed].low |= mask;
      if (bin == 1 || bin == 2) lut[packed].gray |= mask;
    }
  }
  return lut;
}

constexpr std::array<uint8_t, 16> makeSpreadNibbleLut() {
  std::array<uint8_t, 16> lut{};
  for (uint8_t nibble = 0; nibble < 16; ++nibble) {
    for (uint8_t pixel = 0; pixel < 4; ++pixel) {
      if (nibble & (1u << (3 - pixel))) lut[nibble] |= static_cast<uint8_t>(1u << (6 - 2 * pixel));
    }
  }
  return lut;
}

constexpr std::array<uint8_t, 256> makeReverseBitsLut() {
  std::array<uint8_t, 256> lut{};
  for (uint16_t input = 0; input < 256; ++input) {
    uint8_t value = static_cast<uint8_t>(input);
    value = static_cast<uint8_t>(((value & 0x55u) << 1) | ((value >> 1) & 0x55u));
    value = static_cast<uint8_t>(((value & 0x33u) << 2) | ((value >> 2) & 0x33u));
    lut[input] = static_cast<uint8_t>((value << 4) | (value >> 4));
  }
  return lut;
}

constexpr auto PACKED_BIN_MASK_LUT = makePackedBinMaskLut();
constexpr auto SPREAD_NIBBLE_LUT = makeSpreadNibbleLut();
constexpr auto REVERSE_BITS_LUT = makeReverseBitsLut();
static_assert(PACKED_BIN_MASK_LUT[0x00].gray == 0x00 && PACKED_BIN_MASK_LUT[0x55].gray == 0x0F &&
                  PACKED_BIN_MASK_LUT[0xAA].gray == 0x0F && PACKED_BIN_MASK_LUT[0xFF].gray == 0x00,
              "packed four-gray mask mapping must preserve endpoint pixels");
static_assert(SPREAD_NIBBLE_LUT[0x0F] == 0x55, "four-level bin packing must preserve pixel order");

constexpr uint8_t packFourBins(uint8_t highNibble, uint8_t lowNibble) {
  return static_cast<uint8_t>((SPREAD_NIBBLE_LUT[highNibble & 0x0F] << 1) | SPREAD_NIBBLE_LUT[lowNibble & 0x0F]);
}

// SSD1683 selects LUT entry = (RAM 0x26 bit << 1) | RAM 0x24 bit. The plane
// map uses that same bit layout, so scheme B routes dark to entry 10 and light
// to entry 01. This order was verified directly on Paper Mono hardware.
static_assert(GRAY_PLANE_SCHEME_B[1] == 2, "dark gray must select LUT entry 10");
static_assert(GRAY_PLANE_SCHEME_B[2] == 1, "light gray must select LUT entry 01");

void rotatePlane180InPlace(uint8_t* data) {
  for (uint32_t front = 0, back = BUFFER_SIZE - 1; front < back; ++front, --back) {
    const uint8_t frontValue = data[front];
    const uint8_t backValue = data[back];
    data[front] = REVERSE_BITS_LUT[backValue];
    data[back] = REVERSE_BITS_LUT[frontValue];
  }
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
  const bool binsWereMissing = _bins == nullptr;
  const bool preparedBinsWereMissing = _preparedBins == nullptr;
  const bool ok = alloc(_lastBw, BUFFER_SIZE) && alloc(_grayLsb, BUFFER_SIZE) && alloc(_grayMsb, BUFFER_SIZE) &&
                  alloc(_base, BUFFER_SIZE) && alloc(_oldEffective, BUFFER_SIZE) &&
                  alloc(_whiteCleanupMask, BUFFER_SIZE) && alloc(_bins, BIN_BUFFER_SIZE) &&
                  alloc(_preparedBins, BIN_BUFFER_SIZE);
  // Allocation must not mutate committed panel history. In particular, the
  // balanced gray maintenance path temporarily clears _grayArmed before it
  // reconstructs selectors from _bins. Clearing here turned that entire pass
  // into an all-00 custom update while the software still reported restored=1.
  if (binsWereMissing && _bins) memset(_bins, 0, BIN_BUFFER_SIZE);
  if (preparedBinsWereMissing && _preparedBins) memset(_preparedBins, 0, BIN_BUFFER_SIZE);
  return ok;
}

void Ssd1683Driver::begin(EpdBus& bus) {
  allocateGrayBuffers();
  bus.reset();
  initController(bus);
  _needsFull = true;
  _lastBwValid = false;
  _abortGeneration.store(0);
  _displayWorkGeneration = 0;
  _maintenancePending.store(false);
  _grayMaintenancePending.store(false);
  _grayRefinePending.store(false);
  _abortedAtTwoLevelBase = false;
  _asyncPending = false;
  _controllerPowered = false;
  _lutState = LutState::Unknown;
  _bwUpdatesSinceMaintenance = 0;
  _grayPagesSinceMaintenance = 0;
  resetGray();
}

void Ssd1683Driver::initController(EpdBus& bus) {
  bus.cmd(CMD_SOFT_RESET);
  bus.waitBusy("SSD1683 reset");
  _controllerPowered = false;
  _lutState = LutState::Unknown;

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
  uint32_t transformUs = 0;
  uint32_t spiUs = 0;
  bus.beginTxn();
  for (uint32_t sent = 0; sent < BUFFER_SIZE; sent += ROTATE_CHUNK_BYTES) {
    const uint16_t count =
        static_cast<uint16_t>((BUFFER_SIZE - sent) < ROTATE_CHUNK_BYTES ? (BUFFER_SIZE - sent) : ROTATE_CHUNK_BYTES);
    const uint32_t transformStarted = micros();
    for (uint16_t i = 0; i < count; ++i)
      ROTATE_CHUNK[i] = REVERSE_BITS_LUT[data[BUFFER_SIZE - 1 - sent - i]];
    transformUs += micros() - transformStarted;
    const uint32_t spiStarted = micros();
    bus.rawWriteBytes(ROTATE_CHUNK, count);
    spiUs += micros() - spiStarted;
  }
  bus.endTxn();
#ifdef ENABLE_SERIAL_LOG
  Serial.printf("[%lu] SSD1683 plane 0x%02X: transform=%luus spi=%luus chunks=%u\n", millis(), command,
                static_cast<unsigned long>(transformUs), static_cast<unsigned long>(spiUs),
                static_cast<unsigned>((BUFFER_SIZE + ROTATE_CHUNK_BYTES - 1) / ROTATE_CHUNK_BYTES));
#endif
}

void Ssd1683Driver::writePhysicalPlane(EpdBus& bus, uint8_t command, const uint8_t* data) {
  if (!data) return;
  resetRamCounter(bus);
  bus.cmd(command);
  const uint32_t startedUs = micros();
  bus.data(data, static_cast<uint16_t>(BUFFER_SIZE));
#ifdef ENABLE_SERIAL_LOG
  Serial.printf("[%lu] SSD1683 plane 0x%02X: pretransformed spi=%luus\n", millis(), command,
                static_cast<unsigned long>(micros() - startedUs));
#endif
}

void Ssd1683Driver::activateStart(EpdBus& bus, uint8_t control) {
  bus.cmd(0x22);
  bus.data(control);
  bus.cmd(0x20);
}

void Ssd1683Driver::activate(EpdBus& bus, uint8_t control) {
  activateStart(bus, control);
  bus.waitRefreshComplete("SSD1683 refresh");
}

void Ssd1683Driver::activateOtpStart(EpdBus& bus) {
  // When the preceding task was another OTP B/W update, the loaded Mono LUT
  // and powered rails are still valid. Otherwise 0xFC reloads the temperature
  // selected Paper Mono OTP waveform and powers the analog domain in the same
  // master activation, without shutting it down at the tail.
  const bool warmOtp = _controllerPowered && _lutState == LutState::OtpBw;
  activateStart(bus, warmOtp ? CTRL_DISPLAY_HOLD_WARM : CTRL_OTP_BW_HOLD);
  _controllerPowered = true;
  _lutState = LutState::OtpBw;
#ifdef ENABLE_SERIAL_LOG
  Serial.printf("[%lu] SSD1683 OTP activation: powered=hold source=%s\n", millis(), warmOtp ? "resident" : "mono-otp");
#endif
}

void Ssd1683Driver::activateOtp(EpdBus& bus) {
  activateOtpStart(bus);
  bus.waitRefreshComplete("SSD1683 OTP refresh");
}

void Ssd1683Driver::powerOffController(EpdBus& bus) {
  if (!_controllerPowered) return;
  const unsigned long started = millis();
  activate(bus, CTRL_POWER_OFF);
  _controllerPowered = false;
#ifdef ENABLE_SERIAL_LOG
  Serial.printf("[%lu] SSD1683 controller idle: analog+clock off in %lums\n", millis(), millis() - started);
#endif
}

void Ssd1683Driver::absolutePass(EpdBus& bus, const uint8_t* target) {
  if (!target || !allocateGrayBuffers()) return;
  for (uint32_t i = 0; i < BUFFER_SIZE; ++i) _oldEffective[i] = static_cast<uint8_t>(~target[i]);
  writePlane(bus, CMD_WRITE_NEW, target);
  writePlane(bus, CMD_WRITE_OLD, _oldEffective);
  activateOtp(bus);
  writePlane(bus, CMD_WRITE_NEW, target);
  writePlane(bus, CMD_WRITE_OLD, target);
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
  _lutState = LutState::Custom;
}

void Ssd1683Driver::customPass(EpdBus& bus, const uint8_t* p24, const uint8_t* p26, const uint8_t lut[111],
                               bool planesPhysical) {
  bus.cmd(0x3C);
  bus.data(0x80);
  if (planesPhysical) {
    writePhysicalPlane(bus, CMD_WRITE_NEW, p24);
    writePhysicalPlane(bus, CMD_WRITE_OLD, p26);
  } else {
    writePlane(bus, CMD_WRITE_NEW, p24);
    writePlane(bus, CMD_WRITE_OLD, p26);
  }
  loadCustomLut(bus, lut);
  bus.cmd(0x21);
  bus.data(0x00);
  bus.data(0x00);
  // The custom trigger intentionally omits OTP LUT/temp reload bits 0x10/0x20.
  // Including either silently replaces the injected waveform.
  const uint8_t control = _controllerPowered ? CTRL_DISPLAY_HOLD_WARM : CTRL_CUSTOM_HOLD_COLD;
  activate(bus, control);
  _controllerPowered = true;
  _lutState = LutState::Custom;
}

void Ssd1683Driver::prepareWhiteCleanup(const uint8_t* bw, const uint8_t* oldBw, bool oldFrameHadGray,
                                        bool preservePending) {
  _whiteCleanupPixels = 0;
  _whiteCleanupForGray = false;
  if (!bw || !oldBw || !allocateGrayBuffers()) return;
  uint32_t carriedPixels = 0;

  for (uint32_t i = 0; i < BUFFER_SIZE; ++i) {
    // UI screens such as Home intentionally schedule a second render after
    // their first paint. Carry an unfinished cleanup through that same-target
    // render; otherwise displayStart() clears the task after _panelHasGray was
    // already reset, permanently losing the gray residue it was meant to erase.
    const uint8_t carried = preservePending ? static_cast<uint8_t>(_whiteCleanupMask[i] & bw[i]) : 0;
    carriedPixels += static_cast<uint32_t>(__builtin_popcount(static_cast<unsigned int>(carried)));
    uint8_t oldNonWhite = static_cast<uint8_t>(~oldBw[i]);
    if (oldFrameHadGray) {
      const uint8_t oldGray = static_cast<uint8_t>((PACKED_BIN_MASK_LUT[_bins[i * 2]].gray << 4) |
                                                   PACKED_BIN_MASK_LUT[_bins[i * 2 + 1]].gray);
      oldNonWhite = static_cast<uint8_t>(oldNonWhite | oldGray);
    }
    _whiteCleanupMask[i] = static_cast<uint8_t>((oldNonWhite & bw[i]) | carried);
    _whiteCleanupPixels += static_cast<uint32_t>(__builtin_popcount(static_cast<unsigned int>(_whiteCleanupMask[i])));
  }
#ifdef ENABLE_SERIAL_LOG
  if (carriedPixels > 0) {
    Serial.printf("[%lu] SSD1683 white cleanup carried: pixels=%lu merged=%lu\n", millis(),
                  static_cast<unsigned long>(carriedPixels), static_cast<unsigned long>(_whiteCleanupPixels));
  }
#endif
}

void Ssd1683Driver::stageWhiteCleanup(bool mergeIntoGray) {
  (void)mergeIntoGray;
  // Differential foreground updates and the periodic balanced OTP refresh own
  // ghost removal. The selective custom cleanup experiment added a second
  // one-way waveform whose charge history was not represented by _bins.
  _maintenancePending.store(false);
  _whiteCleanupPixels = 0;
  _whiteCleanupForGray = false;
}

void Ssd1683Driver::display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  (void)turnOff;
  if (!fb) return;
  if (!_initialized) {
    bus.reset();
    initController(bus);
    _needsFull = true;
  }
  allocateGrayBuffers();
  const uint8_t* const settledBw = _lastBwValid ? _lastBw : prev;
  if (!_needsFull && mode != RefreshMode::Full && !_panelHasGray && settledBw &&
      memcmp(settledBw, fb, BUFFER_SIZE) == 0) {
    // A UI may request a follow-up render after asynchronous data/covers are
    // ready even when the resulting pixels are identical. Keep the cleanup
    // queued by the first frame, but submit no redundant panel waveform.
    _grayMaintenancePending.store(false);
    _grayRefinePending.store(false);
    _grayTargetPrepared = false;
    _graySelectorsPhysical = false;
    discardPreparedBins();
    _grayLsbReady = false;
    _grayMsbReady = false;
    if (_preparingGray) _maintenancePending.store(false);
#ifdef ENABLE_SERIAL_LOG
    Serial.printf("[%lu] SSD1683 unchanged target: waveform skipped cleanup_pending=%u\n", millis(),
                  static_cast<unsigned>(_maintenancePending.load()));
#endif
    return;
  }
  // This primary refresh supersedes maintenance for the previous frame. The
  // cancellation generation was captured before CPU composition began; never
  // reset it here or an input which raced layout would be lost.
  _maintenancePending.store(false);
  _grayMaintenancePending.store(false);
  _grayRefinePending.store(false);
  _grayTargetPrepared = false;
  _graySelectorsPhysical = false;
  discardPreparedBins();
  bool foregroundChanged = false;

  if (_needsFull || mode == RefreshMode::Full) {
    _whiteCleanupPixels = 0;
    _whiteCleanupForGray = false;
    // Encode every target pixel as a definite transition and let the panel's
    // balanced OTP waveform clean and present it in one activation. The former
    // three-pass endpoint sweep took ~2.1 s; the attempted joined custom LUT
    // took ~2.4 s and left heavy ghosts on W21 hardware.
    bus.cmd(0x3C);
    bus.data(0x80);
    if (allocateGrayBuffers()) {
      const unsigned long started = millis();
      absolutePass(bus, fb);
#ifdef ENABLE_SERIAL_LOG
      Serial.printf("[%lu] SSD1683 absolute full: %lums\n", millis(), millis() - started);
#endif
    } else {
      // Allocation failure still gets a deterministic absolute target frame.
      writePlane(bus, CMD_WRITE_NEW, fb);
      activateOtp(bus);
      writePlane(bus, CMD_WRITE_NEW, fb);
      writePlane(bus, CMD_WRITE_OLD, fb);
    }
    _needsFull = false;
    _grayArmed = false;
    _panelHasGray = false;
    _bwUpdatesSinceMaintenance = 0;
  } else {
    const bool hadGray = _panelHasGray;
    const uint8_t* const oldBw = _lastBwValid ? _lastBw : prev;
    foregroundChanged = !oldBw || memcmp(oldBw, fb, BUFFER_SIZE) != 0;
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
        for (uint32_t byte = 0; byte < BUFFER_SIZE; ++byte) {
          const uint8_t grayMask = static_cast<uint8_t>((PACKED_BIN_MASK_LUT[_bins[byte * 2]].gray << 4) |
                                                        PACKED_BIN_MASK_LUT[_bins[byte * 2 + 1]].gray);
          _oldEffective[byte] = static_cast<uint8_t>((oldBw[byte] & ~grayMask) | (~fb[byte] & grayMask));
        }
        differentialOld = _oldEffective;
      }
      writePlane(bus, CMD_WRITE_OLD, differentialOld);
    }
    // Keep the binary precursor on Paper Mono's own OTP partial waveform.
    // Custom fast-base LUTs borrowed from other panels under-drive this W21
    // panel and leave severe, persistent endpoint ghosts.
    activateOtp(bus);
    writePlane(bus, CMD_WRITE_OLD, fb);
    _grayArmed = false;
    _panelHasGray = false;

    // Do not run a one-way full-screen endpoint reinforcement after this
    // differential update. Besides accumulating image-correlated DC, it can
    // land just before a rapid page turn and visibly gray/mottle pure B/W UI.
    // Periodic vendor full refreshes remain the balanced ghost-cleaning path.
    ++_bwUpdatesSinceMaintenance;
  }

  if (_lastBw) {
    memcpy(_lastBw, fb, BUFFER_SIZE);
    _lastBwValid = true;
  }
  // Every settled binary UI transition gets a balanced, all-pixel Mono OTP
  // cleanup. A following foreground render clears/replaces this task before it
  // starts; once started it is indivisible and the new render runs afterward.
  if (!_preparingGray && foregroundChanged) _maintenancePending.store(true);
  _grayLsbReady = false;
  _grayMsbReady = false;
}

bool Ssd1683Driver::displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) {
  if (!fb) return false;
  // Endpoint sweeps contain multiple ordered waveforms and remain blocking.
  // Ordinary differential page turns use the split start/finish path below.
  if (!_initialized || _needsFull || mode == RefreshMode::Full) {
    display(bus, fb, prev, mode, turnOff);
    return false;
  }

  (void)turnOff;
  allocateGrayBuffers();
  const uint8_t* const settledBw = _lastBwValid ? _lastBw : prev;
  if (!_panelHasGray && settledBw && memcmp(settledBw, fb, BUFFER_SIZE) == 0) {
    _grayMaintenancePending.store(false);
    _grayRefinePending.store(false);
    _grayTargetPrepared = false;
    _graySelectorsPhysical = false;
    discardPreparedBins();
    _grayLsbReady = false;
    _grayMsbReady = false;
    if (_preparingGray) _maintenancePending.store(false);
#ifdef ENABLE_SERIAL_LOG
    Serial.printf("[%lu] SSD1683 unchanged async target: waveform skipped cleanup_pending=%u\n", millis(),
                  static_cast<unsigned>(_maintenancePending.load()));
#endif
    return false;
  }
  // beginDisplayWork() captured the generation before CPU composition. Do not
  // clear cancellation here: this may already be an obsolete rendered page.
  _maintenancePending.store(false);
  _grayMaintenancePending.store(false);
  _grayRefinePending.store(false);
  _grayTargetPrepared = false;
  _graySelectorsPhysical = false;
  discardPreparedBins();

  _asyncHadGray = _panelHasGray;
  _asyncPreparingGray = _preparingGray;
  _asyncMode = mode;
  _asyncChangedPixels = 0;
  const uint8_t* const oldBw = _lastBwValid ? _lastBw : prev;
  if (oldBw) {
    for (uint32_t i = 0; i < BUFFER_SIZE; ++i) {
      _asyncChangedPixels += static_cast<uint32_t>(__builtin_popcount(static_cast<unsigned int>(oldBw[i] ^ fb[i])));
    }
  }

  bus.cmd(0x3C);
  bus.data(0x80);
  writePlane(bus, CMD_WRITE_NEW, fb);
  if (oldBw) {
    const uint8_t* differentialOld = oldBw;
    if (_asyncHadGray && allocateGrayBuffers()) {
      for (uint32_t byte = 0; byte < BUFFER_SIZE; ++byte) {
        const uint8_t grayMask = static_cast<uint8_t>((PACKED_BIN_MASK_LUT[_bins[byte * 2]].gray << 4) |
                                                      PACKED_BIN_MASK_LUT[_bins[byte * 2 + 1]].gray);
        _oldEffective[byte] = static_cast<uint8_t>((oldBw[byte] & ~grayMask) | (~fb[byte] & grayMask));
      }
      differentialOld = _oldEffective;
    }
    writePlane(bus, CMD_WRITE_OLD, differentialOld);
  }

  _asyncStartedMs = millis();
  _asyncPending = true;
  // Mode 2 with LUT/temp reload uses the Paper Mono controller's own OTP
  // partial waveform. CPU-side grayscale staging still overlaps this BUSY
  // interval, and the gray pass can reuse its NEW RAM plane afterward.
  activateOtpStart(bus);
#ifdef ENABLE_SERIAL_LOG
  Serial.printf("[%lu] SSD1683 async start: mode=%u changed=%lu gray=%u lut=mono-otp\n", _asyncStartedMs,
                static_cast<unsigned>(_asyncMode), static_cast<unsigned long>(_asyncChangedPixels),
                static_cast<unsigned>(_asyncHadGray));
#endif
  return true;
}

void Ssd1683Driver::displayFinish(EpdBus& bus, const uint8_t* fb) {
  if (!_asyncPending) return;
  const unsigned long waitStarted = millis();
  bus.waitRefreshComplete("SSD1683 async refresh");
  const unsigned long waveformDone = millis();
  _asyncPending = false;
  if (!fb) return;

  // A prepared gray pass will immediately consume the controller planes. Do
  // not spend another full-plane transfer seeding OLD in the narrow gap. If an
  // input cancelled the generation, retain the ordinary seed so the visible
  // two-level frame remains a complete standalone commit.
  const bool preparedGray = _grayTargetPrepared && _preparedBinsReady &&
                            _preparedGrayGeneration == _displayWorkGeneration && !displayWorkCancelled();
  if (!preparedGray) writePlane(bus, CMD_WRITE_OLD, fb);
  _panelHasGray = false;
  if (!_asyncPreparingGray) {
    ++_bwUpdatesSinceMaintenance;
    if (_asyncChangedPixels > 0) _maintenancePending.store(true);
  }
  if (_lastBw) {
    memcpy(_lastBw, fb, BUFFER_SIZE);
    _lastBwValid = true;
  }
  if (!preparedGray) {
    _grayTargetPrepared = false;
    _graySelectorsPhysical = false;
    discardPreparedBins();
    _grayLsbReady = false;
    _grayMsbReady = false;
  }
#ifdef ENABLE_SERIAL_LOG
  const unsigned long finished = millis();
  Serial.printf("[%lu] SSD1683 async finish: flight=%lums wait=%lums post=%lums gray_prepared=%u\n", finished,
                waveformDone - _asyncStartedMs, waveformDone - waitStarted, finished - waveformDone,
                static_cast<unsigned>(preparedGray));
#endif
}

void Ssd1683Driver::seedPreviousFrame(EpdBus& bus, const uint8_t* buf) {
  writePlane(bus, CMD_WRITE_OLD, buf);
  if (_lastBw && buf) {
    memcpy(_lastBw, buf, BUFFER_SIZE);
    _lastBwValid = true;
  }
}

void Ssd1683Driver::displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) {
  _preparingGray = true;
  display(bus, fb, nullptr, fallback, turnOff);
  _preparingGray = false;
}

void Ssd1683Driver::beginDisplayWork() { _displayWorkGeneration = _abortGeneration.load(); }

bool Ssd1683Driver::displayWorkCancelled() const { return _abortGeneration.load() != _displayWorkGeneration; }

bool Ssd1683Driver::postRefreshAborted() const { return displayWorkCancelled(); }

void Ssd1683Driver::abortPostRefresh() { _abortGeneration.fetch_add(1); }

bool Ssd1683Driver::hasPendingMaintenance() const {
  return _lastBwValid &&
         (_grayRefinePending.load() || _grayMaintenancePending.load() || _maintenancePending.load());
}

void Ssd1683Driver::runMaintenance(EpdBus& bus) {
  const bool grayRefine = _grayRefinePending.load();
  const bool grayCleanup = _grayMaintenancePending.load();
  const bool bwCleanup = _maintenancePending.load();
  if (!grayRefine && !grayCleanup && !bwCleanup) return;
  if (!_lastBwValid) return;

  // ActivityManager only calls this after a quiet window. A monotonic snapshot
  // distinguishes the old input which opened that quiet window from a fresh
  // edge racing this pass; no cancellation bit is ever cleared here.
  const uint32_t maintenanceGeneration = _abortGeneration.load();
  if (grayRefine) {
    if (!_grayRefinePending.exchange(false)) return;
    if (maintenanceGeneration != _grayRefineGeneration) {
      _graySelectorsPhysical = false;
      discardPreparedBins();
#ifdef ENABLE_SERIAL_LOG
      Serial.printf("[%lu] SSD1683 gray refine dropped: staged_gen=%lu current_gen=%lu\n", millis(),
                    static_cast<unsigned long>(_grayRefineGeneration),
                    static_cast<unsigned long>(maintenanceGeneration));
#endif
      return;
    }

    uint8_t customLut[111];
    makeGrayLut(customLut);
    const unsigned long started = millis();
    // Explicitly submit both selectors. Reusing NEW saved one SPI transfer but
    // made correctness depend on an implicit controller-RAM state spanning an
    // async OTP pass, cancellation and retained power.
    customPass(bus, _grayLsb, _grayMsb, customLut, _graySelectorsPhysical);
    _graySelectorsPhysical = false;
    commitPreparedBins();

    // The custom selector planes are not a differential baseline. Restore both
    // controller RAM planes before another page can start; this is the last
    // committed stable Paper Mono behavior.
    writePlane(bus, CMD_WRITE_NEW, _lastBw);
    writePlane(bus, CMD_WRITE_OLD, _lastBw);
    _grayArmed = true;
    _panelHasGray = _grayRefineLeavesGray;
    _grayLsbReady = false;
    _grayMsbReady = false;
    const bool cancelledDuring = _abortGeneration.load() != maintenanceGeneration;
    if (_panelHasGray && !cancelledDuring) {
      ++_grayPagesSinceMaintenance;
      if (_grayPagesSinceMaintenance >= GRAY_DEGHOST_INTERVAL) {
        _grayMaintenancePending.store(true);
      }
    }
#ifdef ENABLE_SERIAL_LOG
    Serial.printf("[%lu] SSD1683 gray refine committed: gen=%lu pixels=%lu elapsed=%lums cancelled_during=%u\n",
                  millis(), static_cast<unsigned long>(_grayRefineGeneration),
                  static_cast<unsigned long>(_grayRefinePixels), millis() - started,
                  static_cast<unsigned>(cancelledDuring));
#endif
  } else if (grayCleanup && _panelHasGray) {
    if (!_grayMaintenancePending.exchange(false)) return;
    if (_abortGeneration.load() != maintenanceGeneration) {
      return;
    }
    const unsigned long started = millis();
    // Re-establish every pixel from its inverse through Paper Mono's own OTP
    // waveform. This is balanced and represented by the controller baseline;
    // unlike the removed one-way reinforce LUT it cannot accumulate a hidden
    // image-correlated charge state.
    absolutePass(bus, _lastBw);
    _panelHasGray = false;
    _grayArmed = false;
    _grayPagesSinceMaintenance = 0;

    bool grayRestored = false;
    if (_abortGeneration.load() == maintenanceGeneration) {
      encodeCommittedGraySelectors();
      uint8_t customLut[111];
      makeGrayLut(customLut);
      customPass(bus, _grayLsb, _grayMsb, customLut);
      writePlane(bus, CMD_WRITE_NEW, _lastBw);
      writePlane(bus, CMD_WRITE_OLD, _lastBw);
      _grayArmed = true;
      _panelHasGray = true;
      grayRestored = true;
    }
#ifdef ENABLE_SERIAL_LOG
    Serial.printf("[%lu] SSD1683 balanced gray deghost: gen=%lu elapsed=%lums restored=%u\n", millis(),
                  static_cast<unsigned long>(maintenanceGeneration), millis() - started,
                  static_cast<unsigned>(grayRestored));
#endif
  } else if (grayCleanup) {
    // A newer foreground B/W frame removed the gray page before this low
    // priority task reached the head of the controller queue.
    _grayMaintenancePending.store(false);
  } else if (bwCleanup) {
    if (!_maintenancePending.exchange(false)) return;
    if (_abortGeneration.load() != maintenanceGeneration) return;
    const unsigned long started = millis();
    absolutePass(bus, _lastBw);
    _bwUpdatesSinceMaintenance = 0;
    _grayArmed = false;
    _panelHasGray = false;
#ifdef ENABLE_SERIAL_LOG
    Serial.printf("[%lu] SSD1683 balanced BW deghost: gen=%lu elapsed=%lums cancelled=%u\n", millis(),
                  static_cast<unsigned long>(maintenanceGeneration), millis() - started,
                  static_cast<unsigned>(_abortGeneration.load() != maintenanceGeneration));
#endif
  }
}

void Ssd1683Driver::controllerIdle(EpdBus& bus) { powerOffController(bus); }

void Ssd1683Driver::copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) {
  (void)bus;
  if (!lsb || !allocateGrayBuffers()) return;
  // This B/W frame is becoming a grayscale page, whose own cleanup stage owns
  // the panel endpoint. A deferred pure-B/W reinforce would erase the gray.
  _maintenancePending.store(false);
  memcpy(_grayLsb, lsb, BUFFER_SIZE);
  _grayLsbReady = true;
  _graySelectorsPhysical = false;
}

void Ssd1683Driver::copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) {
  (void)bus;
  if (!msb || !allocateGrayBuffers()) return;
  _maintenancePending.store(false);
  memcpy(_grayMsb, msb, BUFFER_SIZE);
  _grayMsbReady = true;
  _graySelectorsPhysical = false;
}

void Ssd1683Driver::writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                             uint16_t numRows) {
  (void)bus;
  if (!rows || numRows == 0 || yStart + numRows > HEIGHT || !allocateGrayBuffers()) return;
  _maintenancePending.store(false);
  uint8_t* target = plane == GrayPlane::Lsb ? _grayLsb : _grayMsb;
  memcpy(target + static_cast<uint32_t>(yStart) * WIDTH_BYTES, rows, static_cast<uint32_t>(numRows) * WIDTH_BYTES);
  if (plane == GrayPlane::Lsb) {
    _grayLsbReady = true;
  } else {
    _grayMsbReady = true;
  }
  _graySelectorsPhysical = false;
}

void Ssd1683Driver::prepareGrayscaleTarget(const uint8_t* bw) {
  _grayTargetPrepared = false;
  _graySelectorsPhysical = false;
  discardPreparedBins();
  if (!bw || !_grayLsbReady || !_grayMsbReady || !allocateGrayBuffers() || displayWorkCancelled()) return;

  // Perform the 48 KB transition encoding while the controller is still
  // scanning the B/W waveform. Old-page cleanup is deliberately excluded:
  // that base waveform already drove it to the current binary endpoint.
  _whiteCleanupForGray = false;
  const uint32_t startedUs = micros();
  _preparedGrayPixels = encodeGrayTarget(bw);
  if (!displayWorkCancelled()) {
    if (BoardConfig::ACTIVE.orientation.mirrorX && BoardConfig::ACTIVE.orientation.mirrorY) {
      rotatePlane180InPlace(_grayLsb);
      rotatePlane180InPlace(_grayMsb);
    }
    _graySelectorsPhysical = true;
  }
  _preparedMergedWhiteCleanup = false;
  _preparedGrayGeneration = _displayWorkGeneration;
  _grayTargetPrepared = _preparedBinsReady && !displayWorkCancelled();
  if (!_grayTargetPrepared) discardPreparedBins();
#ifdef ENABLE_SERIAL_LOG
  Serial.printf("[%lu] SSD1683 gray target prepared: gen=%lu gray=%lu cleanup=%lu ready=%u encode=%luus\n", millis(),
                static_cast<unsigned long>(_preparedGrayGeneration), static_cast<unsigned long>(_preparedGrayPixels),
                static_cast<unsigned long>(_preparedMergedWhiteCleanup ? _whiteCleanupPixels : 0),
                static_cast<unsigned>(_grayTargetPrepared), static_cast<unsigned long>(micros() - startedUs));
#endif
}

uint32_t Ssd1683Driver::encodeGrayTarget(const uint8_t* bwOverride) {
  if (!_preparedBins) return 0;
  _graySelectorsPhysical = false;
  const bool schemeB = _grayParams.scheme == 1;
  uint32_t grayPixels = 0;

  for (uint32_t i = 0; i < BUFFER_SIZE; ++i) {
    const uint8_t inputLsb = _grayLsb[i];
    const uint8_t inputMsb = _grayMsb[i];
    const uint8_t bw = bwOverride ? bwOverride[i] : _lastBw[i];

    // The input planes encode dark as L=1, light as L=0/M=1, and use BW for
    // endpoint pixels. Derive the packed bin's two bit planes eight pixels at a
    // time, avoiding the former 384,000-iteration per-pixel loop.
    const uint8_t darkMask = inputLsb;
    const uint8_t lightMask = static_cast<uint8_t>(inputMsb & ~inputLsb);
    const uint8_t whiteMask = static_cast<uint8_t>(bw & ~(inputLsb | inputMsb));
    const uint8_t binHigh = static_cast<uint8_t>(lightMask | whiteMask);
    const uint8_t binLow = static_cast<uint8_t>(darkMask | whiteMask);
    const uint8_t grayMask = static_cast<uint8_t>(inputLsb | inputMsb);

    const uint32_t binIndex = i * 2;
    const auto& oldUpper = PACKED_BIN_MASK_LUT[_bins[binIndex]];
    const auto& oldLower = PACKED_BIN_MASK_LUT[_bins[binIndex + 1]];
    const uint8_t oldHigh = static_cast<uint8_t>((oldUpper.high << 4) | oldLower.high);
    const uint8_t oldLow = static_cast<uint8_t>((oldUpper.low << 4) | oldLower.low);
    const uint8_t changedMask = static_cast<uint8_t>((binHigh ^ oldHigh) | (binLow ^ oldLow));

    const uint8_t baseByte = schemeB ? binHigh : binLow;
    const uint8_t forcedMask = _grayArmed ? static_cast<uint8_t>(grayMask | changedMask) : static_cast<uint8_t>(0xFF);
    _base[i] = baseByte;
    _oldEffective[i] = static_cast<uint8_t>(baseByte ^ forcedMask);
    if (schemeB) {
      // Stable scheme-B mapping: black selects 00, dark 10, light 01 and white
      // 11. Entries 00/11 are both no-drive in makeGrayLut().
      _grayLsb[i] = binHigh;
      _grayMsb[i] = binLow;
    } else {
      _grayLsb[i] = darkMask;
      _grayMsb[i] = lightMask;
    }

    _preparedBins[binIndex] = packFourBins(static_cast<uint8_t>(binHigh >> 4), static_cast<uint8_t>(binLow >> 4));
    _preparedBins[binIndex + 1] = packFourBins(binHigh, binLow);
    grayPixels += static_cast<uint32_t>(__builtin_popcount(static_cast<unsigned int>(grayMask)));
  }
  _preparedBinsReady = true;
  return grayPixels;
}

void Ssd1683Driver::commitPreparedBins() {
  if (!_preparedBinsReady) return;
  std::swap(_bins, _preparedBins);
  _preparedBinsReady = false;
}

void Ssd1683Driver::discardPreparedBins() { _preparedBinsReady = false; }

void Ssd1683Driver::encodeCommittedGraySelectors() {
  if (!allocateGrayBuffers()) return;
  for (uint32_t i = 0; i < BUFFER_SIZE; ++i) {
    const auto& upper = PACKED_BIN_MASK_LUT[_bins[i * 2]];
    const auto& lower = PACKED_BIN_MASK_LUT[_bins[i * 2 + 1]];
    const uint8_t high = static_cast<uint8_t>((upper.high << 4) | lower.high);
    const uint8_t low = static_cast<uint8_t>((upper.low << 4) | lower.low);
    if (_grayParams.scheme == 1) {
      _grayLsb[i] = high;
      _grayMsb[i] = low;
    } else {
      _grayLsb[i] = static_cast<uint8_t>(low & ~high);   // dark selector
      _grayMsb[i] = static_cast<uint8_t>(high & ~low);  // light selector
    }
  }
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
      _oldEffective[pixel >> 3] |= mask;  // entry 10 reinforces white
    } else {
      _base[pixel >> 3] |= mask;  // entry 01 reinforces black
    }
  }
}

void Ssd1683Driver::makeGrayLut(uint8_t out[111]) const {
  memset(out, 0, 111);
  if (_grayParams.scheme == 1) {
    // Entry 00/11 remain no-drive. Gray refinement is strictly scoped to the
    // current book glyph/image selectors; old-page and UI cleanup never share
    // this waveform.
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
  if (displayWorkCancelled()) {
    _grayTargetPrepared = false;
    _graySelectorsPhysical = false;
    discardPreparedBins();
    stageWhiteCleanup(false);
    _grayArmed = false;
    _panelHasGray = false;
    _grayLsbReady = false;
    _grayMsbReady = false;
    return;
  }
  const bool usePrepared = _grayTargetPrepared && _preparedGrayGeneration == _displayWorkGeneration;
  const uint32_t grayPixels = usePrepared ? _preparedGrayPixels : encodeGrayTarget();
  const bool mergedWhiteCleanup =
      usePrepared ? _preparedMergedWhiteCleanup
                  : (_whiteCleanupForGray && _grayParams.cleanupPasses > 0 && _whiteCleanupPixels > 0 &&
                     _whiteCleanupGeneration == _displayWorkGeneration);
  _grayTargetPrepared = false;
  uint8_t customLut[111];
  bool grayApplied = false;
  const bool baseAlreadyVisible = memcmp(_base, _lastBw, BUFFER_SIZE) == 0;

  if (displayWorkCancelled()) {
    _graySelectorsPhysical = false;
    discardPreparedBins();
    stageWhiteCleanup(false);
    _grayArmed = false;
    _panelHasGray = false;
    _grayLsbReady = false;
    _grayMsbReady = false;
    return;
  }

  // The reader already displayed scheme B's exact two-level precursor. Stage
  // its gray selectors and return immediately; ActivityManager calls
  // runMaintenance() as soon as foreground composition commits. Input queued
  // while the B/W waveform was BUSY invalidates the generation first, so a
  // burst skips this pass without adding an artificial post-refresh delay.
  if (baseAlreadyVisible) {
    if (grayPixels > 0 || mergedWhiteCleanup) {
      _grayRefineGeneration = _displayWorkGeneration;
      _grayRefinePixels = grayPixels + (mergedWhiteCleanup ? _whiteCleanupPixels : 0);
      _grayRefineLeavesGray = grayPixels > 0;
      _grayRefinePending.store(true);
#ifdef ENABLE_SERIAL_LOG
      Serial.printf("[%lu] SSD1683 gray refine staged: gen=%lu pixels=%lu\n", millis(),
                    static_cast<unsigned long>(_grayRefineGeneration), static_cast<unsigned long>(grayPixels));
#endif
    } else {
      commitPreparedBins();
      _graySelectorsPhysical = false;
      _grayArmed = true;
      _panelHasGray = false;
    }
    _grayLsbReady = false;
    _grayMsbReady = false;
    return;
  }

  // Reader paths now paint scheme B's exact black/dark vs light/white base as
  // their first visible frame. Only legacy/custom callers need this conversion
  // pass; skipping it removes one 543 ms waveform and the thick-then-thin edge.
  if (!baseAlreadyVisible) {
    bus.cmd(0x3C);
    bus.data(0x80);
    writePlane(bus, CMD_WRITE_NEW, _base);
    writePlane(bus, CMD_WRITE_OLD, _oldEffective);
    activateOtp(bus);
  }
  if (_grayParams.baseDouble && !displayWorkCancelled()) {
    writePlane(bus, CMD_WRITE_NEW, _base);
    writePlane(bus, CMD_WRITE_OLD, _oldEffective);
    activateOtp(bus);
  }

  if ((grayPixels > 0 || mergedWhiteCleanup) && !displayWorkCancelled()) {
    makeGrayLut(customLut);
    customPass(bus, _grayLsb, _grayMsb, customLut, _graySelectorsPhysical);
    _graySelectorsPhysical = false;
    commitPreparedBins();
    grayApplied = true;
  }

  if (displayWorkCancelled()) {
    if (!grayApplied) {
      _graySelectorsPhysical = false;
      discardPreparedBins();
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
    _panelHasGray = grayApplied && grayPixels > 0;
    // If input landed in an indivisible custom pass, its exact gray target and
    // bins reached the panel. Do not queue a stale cleanup before the new page.
    if (grayApplied) _grayArmed = true;
  } else {
    if (!grayApplied) discardPreparedBins();
    _grayArmed = true;
    _panelHasGray = grayPixels > 0;
    if (grayPixels > 0) {
      ++_grayPagesSinceMaintenance;
      if (_grayPagesSinceMaintenance >= GRAY_DEGHOST_INTERVAL) _grayMaintenancePending.store(true);
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
  const uint8_t* const calibrationBins = _preparedBinsReady ? _preparedBins : _bins;

  const auto binAt = [calibrationBins](const uint32_t pixel) {
    return static_cast<uint8_t>((calibrationBins[pixel >> 2] >> (6 - 2 * (pixel & 3))) & 0x03);
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
  _controllerPowered = false;
  _lutState = LutState::OtpBw;

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
  bus.cmd(0x3C);
  bus.data(0x80);
  writePlane(bus, CMD_WRITE_NEW, _base);
  writePlane(bus, CMD_WRITE_OLD, _oldEffective);
  activateOtp(bus);
  if (_grayParams.baseDouble) {
    writePlane(bus, CMD_WRITE_NEW, _base);
    writePlane(bus, CMD_WRITE_OLD, _oldEffective);
    activateOtp(bus);
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
  if (_grayTargetPrepared) {
    // The B/W base completed but input cancelled before displayGray() consumed
    // the CPU-prepared selectors. They never reached the panel.
    _grayTargetPrepared = false;
    _graySelectorsPhysical = false;
    discardPreparedBins();
    _grayArmed = false;
    _panelHasGray = false;
    _grayLsbReady = false;
    _grayMsbReady = false;
  }
  if (_abortedAtTwoLevelBase) {
    // displayGray() already seeded the exact visible two-level base. The
    // caller is restoring its logical BW framebuffer here; replacing the
    // controller baseline with that approximation would break the next diff.
    _abortedAtTwoLevelBase = false;
    return;
  }
  // The ordinary reader path has already seeded OLD RAM in displayFinish()
  // (or display() on a blocking refresh), and displayGray() only staged its
  // custom pass for deferred maintenance. Do not transmit the identical 48 KB
  // plane a second time. Once a gray pass has physically run, _panelHasGray is
  // true and the controller must be restored below as before.
  if (!_panelHasGray && _lastBwValid && _lastBw && memcmp(_lastBw, bw, BUFFER_SIZE) == 0) {
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
  _grayMaintenancePending.store(false);
  _maintenancePending.store(false);
  _grayRefinePending.store(false);
  _grayRefineGeneration = 0;
  _grayRefinePixels = 0;
  _grayRefineLeavesGray = false;
  _grayTargetPrepared = false;
  _graySelectorsPhysical = false;
  discardPreparedBins();
  _preparedMergedWhiteCleanup = false;
  _preparedGrayGeneration = 0;
  _preparedGrayPixels = 0;
  if (_bins) memset(_bins, 0, BIN_BUFFER_SIZE);
  if (_preparedBins) memset(_preparedBins, 0, BIN_BUFFER_SIZE);
  _whiteCleanupGeneration = 0;
  _whiteCleanupPixels = 0;
  _whiteCleanupForGray = false;
  _grayPagesSinceMaintenance = 0;
}

void Ssd1683Driver::deepSleep(EpdBus& bus) {
  if (!_initialized) return;
  bus.waitBusy("SSD1683 idle");
  powerOffController(bus);
  bus.cmd(0x10);
  bus.data(0x01);
  delay(100);
  _initialized = false;
  _needsFull = true;
  _lastBwValid = false;
  resetGray();
}

}  // namespace freeink
