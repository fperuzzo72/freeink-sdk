#pragma once

#include <atomic>

#include "PanelDriver.h"

namespace freeink {

// PaperToDo-compatible non-flashing four-gray controls. Defaults match the
// calibrated Paper Mono preset: {clean=1, base2=0, dark=3, light=11}.
struct Ssd1683GrayParams {
  uint8_t cleanupPasses = 1;
  uint8_t revertBoost = 0;  // retained for preset compatibility; base pass supersedes it
  uint8_t baseDouble = 0;
  uint8_t grayStrong = 0;  // scheme A only
  uint8_t scheme = 1;      // 1 = B/W first (recommended), 0 = FreeInk legacy mapping
  uint8_t darkFrames = 3;
  uint8_t lightFrames = 11;
  uint8_t pipeline = 0;  // 0 = validated differential pipeline; other values fall back to 0
  uint8_t slamFrames = 0;
  uint8_t frameRate = 0;
};

class Ssd1683Driver final : public PanelDriver {
 public:
  uint32_t spiHz() const override;
  BusyPolarity busyPolarity() const override { return BusyPolarity::ActiveHigh; }
  PanelGeometry geometry() const override;

  void begin(EpdBus& bus) override;
  void deepSleep(EpdBus& bus) override;
  void display(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  bool supportsAsyncDisplay() const override { return true; }
  bool displayStart(EpdBus& bus, const uint8_t* fb, const uint8_t* prev, RefreshMode mode, bool turnOff) override;
  void displayFinish(EpdBus& bus, const uint8_t* fb) override;
  void seedPreviousFrame(EpdBus& bus, const uint8_t* buf) override;

  bool supportsStripGrayscale() const override { return true; }
  void displayGrayscaleBase(EpdBus& bus, const uint8_t* fb, RefreshMode fallback, bool turnOff) override;
  void copyGrayscaleLsb(EpdBus& bus, const uint8_t* lsb) override;
  void copyGrayscaleMsb(EpdBus& bus, const uint8_t* msb) override;
  // Keep selector encoding after the B/W waveform. The CPU overlap experiment
  // mutated the driver's gray history before that waveform had committed,
  // making cancellation and the physical panel state diverge.
  bool supportsBusyGrayscaleStaging() const override { return false; }
  void writeGrayscalePlaneStrip(EpdBus& bus, GrayPlane plane, const uint8_t* rows, uint16_t yStart,
                                uint16_t numRows) override;
  void prepareGrayscaleTarget(const uint8_t* bw) override;
  void displayGray(EpdBus& bus, const uint8_t* fb, bool turnOff, const unsigned char* lut, bool factoryMode) override;
  void displayGrayCalibration(EpdBus& bus, const uint8_t* fb, uint16_t customX, uint16_t customY, uint16_t customW,
                              uint16_t customH) override;
  void cleanupGrayscaleBuffers(EpdBus& bus, const uint8_t* bw) override;

  void requestResync(uint8_t settlePasses) override;
  // Paper Mono cuts EPD power in deep sleep. Force the first paint through one
  // all-pixel absolute OTP waveform before presenting its target frame.
  void skipInitialResync() override { _needsFull = true; }
  void beginDisplayWork() override;
  void abortPostRefresh() override;
  bool postRefreshAborted() const override;
  void runMaintenance(EpdBus& bus) override;
  bool hasPendingMaintenance() const override;

  void setGrayParams(const Ssd1683GrayParams& params);
  void setFastGrayBase(bool enabled) { _fastGrayBase = enabled; }
  void abortGray() { abortPostRefresh(); }
  void resetGray();

 private:
  bool allocateGrayBuffers();
  void initController(EpdBus& bus);
  void resetRamCounter(EpdBus& bus);
  void writePlane(EpdBus& bus, uint8_t command, const uint8_t* data);
  void activateStart(EpdBus& bus, uint8_t control);
  void activate(EpdBus& bus, uint8_t control);
  void absolutePass(EpdBus& bus, const uint8_t* target);
  void loadCustomLut(EpdBus& bus, const uint8_t lut[111]);
  void customPass(EpdBus& bus, const uint8_t* p24, const uint8_t* p26, const uint8_t lut[111]);
  void prepareWhiteCleanup(const uint8_t* bw, const uint8_t* oldBw, bool oldFrameHadGray,
                           bool preservePending = false);
  void stageWhiteCleanup(bool mergeIntoGray);
  uint32_t encodeGrayTarget(const uint8_t* bwOverride = nullptr);
  void encodeCleanup();
  void makeGrayLut(uint8_t out[111]) const;
  void makeFastBaseLut(uint8_t out[111]) const;
  void makeCleanupLut(uint8_t out[111]) const;
  bool displayWorkCancelled() const;

  bool _initialized = false;
  bool _needsFull = true;
  bool _lastBwValid = false;
  bool _preparingGray = false;
  bool _grayArmed = false;
  bool _panelHasGray = false;
  bool _grayLsbReady = false;
  bool _grayMsbReady = false;
  bool _fastGrayBase = false;
  bool _abortedAtTwoLevelBase = false;
  bool _asyncPending = false;
  bool _asyncHadGray = false;
  bool _asyncPreparingGray = false;
  RefreshMode _asyncMode = RefreshMode::Fast;
  uint32_t _asyncChangedPixels = 0;
  unsigned long _asyncStartedMs = 0;
  std::atomic<uint32_t> _abortGeneration{0};
  uint32_t _displayWorkGeneration = 0;
  std::atomic<bool> _maintenancePending{false};
  std::atomic<bool> _grayMaintenancePending{false};
  // Reader AA is staged during the foreground render and applied only after
  // ActivityManager observes a quiet input window. A new page invalidates it
  // before any waveform starts.
  std::atomic<bool> _grayRefinePending{false};
  uint32_t _grayRefineGeneration = 0;
  uint32_t _grayRefinePixels = 0;
  bool _grayRefineLeavesGray = false;
  bool _grayTargetPrepared = false;
  bool _preparedMergedWhiteCleanup = false;
  uint32_t _preparedGrayGeneration = 0;
  uint32_t _preparedGrayPixels = 0;
  uint32_t _whiteCleanupGeneration = 0;
  uint32_t _whiteCleanupPixels = 0;
  bool _whiteCleanupForGray = false;
  uint8_t _bwUpdatesSinceMaintenance = 0;
  uint8_t _grayPagesSinceMaintenance = 0;
  Ssd1683GrayParams _grayParams{};

  uint8_t* _lastBw = nullptr;
  uint8_t* _grayLsb = nullptr;
  uint8_t* _grayMsb = nullptr;
  uint8_t* _base = nullptr;
  uint8_t* _oldEffective = nullptr;
  uint8_t* _whiteCleanupMask = nullptr;
  uint8_t* _bins = nullptr;  // 2 bpp, four pixels per byte
};

Ssd1683Driver& ssd1683Driver();
void ssd1683SetGrayParams(const Ssd1683GrayParams& params);
void ssd1683SetFastGrayBase(bool enabled);
void ssd1683AbortGray();
void ssd1683ResetGray();

}  // namespace freeink
