#include "LedManager.h"

#if FREEINK_CAP_LED

#include <Wire.h>
#include <esp_cpu.h>
#include <soc/gpio_struct.h>

namespace freeink {

namespace {

constexpr uint8_t M5PM1_ADDR = 0x6E;
constexpr uint8_t M5PM1_POWER_CONFIG_REG = 0x06;  // PWR_CFG (official M5PM1 map)
constexpr uint8_t M5PM1_I2C_CFG_REG = 0x09;       // I2C_CFG: 0 = 100 kHz, no idle sleep
constexpr uint8_t M5PM1_NEO_CFG_REG = 0x50;       // PM1's OWN NeoPixel engine: [6] refresh, [5:0] LED count
constexpr uint8_t M5PM1_POWER_LDO_EN = 1 << 2;    // = PY_RGB_PWR_EN, the LED rail
constexpr int M5_INTERNAL_I2C_SDA = 3;
constexpr int M5_INTERNAL_I2C_SCL = 2;
constexpr uint32_t M5_INTERNAL_I2C_FREQ = 100000;

// WS2812/SK6812-compatible 800 kHz timings. The exact high pulse separates 0
// from 1; the full bit cell stays near 1.25 us. Two LEDs means interrupts are
// masked for only about 60 us per show().
// M5Unified's LedBus_RMT timings for these exact LEDs (300/900 + 900/300 ns,
// 1.2 us cell, 280 us reset, GRB).
constexpr uint16_t T0H_NS = 300;
constexpr uint16_t T1H_NS = 900;
constexpr uint16_t BIT_NS = 1200;

uint32_t nsToCycles(uint32_t ns) {
  const uint32_t mhz = ESP.getCpuFreqMHz();
  return (mhz * ns + 999) / 1000;
}

inline void IRAM_ATTR waitUntil(uint32_t target) {
  while ((int32_t)(esp_cpu_get_cycle_count() - target) < 0) {
  }
}

inline void IRAM_ATTR gpioHigh(uint8_t pin) {
  if (pin < 32) {
    GPIO.out_w1ts = 1UL << pin;
  } else {
    GPIO.out1_w1ts.val = 1UL << (pin - 32);
  }
}

inline void IRAM_ATTR gpioLow(uint8_t pin) {
  if (pin < 32) {
    GPIO.out_w1tc = 1UL << pin;
  } else {
    GPIO.out1_w1tc.val = 1UL << (pin - 32);
  }
}

bool m5Pm1WriteReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(M5PM1_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool m5Pm1ReadReg(uint8_t reg, uint8_t* value) {
  Wire.beginTransmission(M5PM1_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(M5PM1_ADDR, static_cast<uint8_t>(1)) != 1) return false;
  *value = Wire.read();
  return true;
}

bool m5Pm1UpdateReg(uint8_t reg, uint8_t clearMask, uint8_t setMask) {
  uint8_t value = 0;
  if (m5Pm1ReadReg(reg, &value)) {
    return m5Pm1WriteReg(reg, static_cast<uint8_t>((value & ~clearMask) | setMask));
  }
  return false;
}

}  // namespace

bool LedManager::present() const {
  return BoardConfig::ACTIVE.leds.data != BoardConfig::PIN_UNASSIGNED && BoardConfig::ACTIVE.leds.count > 0;
}

uint8_t LedManager::count() const {
  if (!present()) return 0;
  return BoardConfig::ACTIVE.leds.count > MAX_LEDS ? MAX_LEDS : BoardConfig::ACTIVE.leds.count;
}

bool LedManager::enablePower() {
  const auto& cfg = BoardConfig::ACTIVE.leds;
  if (!cfg.pmicRgbPower) return true;

  Wire.begin(M5_INTERNAL_I2C_SDA, M5_INTERNAL_I2C_SCL, M5_INTERNAL_I2C_FREQ);
  Wire.setTimeOut(4);
  if (!m5Pm1WriteReg(M5PM1_I2C_CFG_REG, 0x00)) return false;
  if (!m5Pm1UpdateReg(M5PM1_POWER_CONFIG_REG, 0, M5PM1_POWER_LDO_EN)) return false;
  delay(10);  // rail ramp + LED power-on reset before the first frame
  return true;
}

void LedManager::disablePower() {
  const auto& cfg = BoardConfig::ACTIVE.leds;
  if (!cfg.pmicRgbPower) return;
  m5Pm1UpdateReg(M5PM1_POWER_CONFIG_REG, M5PM1_POWER_LDO_EN, 0);
}

bool LedManager::begin() {
  if (begun_) return true;
  if (!present()) return false;

  pinMode(BoardConfig::ACTIVE.leds.data, OUTPUT);
  digitalWrite(BoardConfig::ACTIVE.leds.data, LOW);
  // The LED rail stays OFF outside use: writePixels() powers it up lazily for
  // the first lit frame and drops it again when everything is black.
  // Unpowered LEDs are dark by definition — no boot clears needed — and the
  // rail's green indicator LED only glows while the LEDs are actually in use.
  // Force the rail off NOW: the PM1 retains state across USB reflashes, so an
  // older firmware's LDO bit would otherwise stay latched until the first
  // alarm cycles it.
  Wire.begin(M5_INTERNAL_I2C_SDA, M5_INTERNAL_I2C_SCL, M5_INTERNAL_I2C_FREQ);
  Wire.setTimeOut(4);
  // Evict the PM1 from the LED chain: the PMIC has its own NeoPixel engine
  // (it renders charge/status onto these same LEDs, even while the ESP
  // sleeps) — that is the "stuck green LED". NEO_CFG = 0 is the official
  // M5PM1::disableLeds(): LED count zero, engine off, the ESP owns the chain.
  m5Pm1WriteReg(M5PM1_NEO_CFG_REG, 0x00);
  disablePower();
  begun_ = true;
  return true;
}

void LedManager::setBrightness(uint8_t brightness) {
  brightness_ = brightness;
  if (begun_) show();
}

void LedManager::setColor(uint8_t index, LedColor color) {
  const uint8_t n = count();
  if (index >= n) return;
  pixels_[index] = color;
  if (begun_ && !flashing_) show();
}

void LedManager::setAll(LedColor color) {
  const uint8_t n = count();
  for (uint8_t i = 0; i < n; ++i) {
    pixels_[i] = color;
  }
  if (begun_ && !flashing_) show();
}

LedColor LedManager::color(uint8_t index) const {
  if (index >= count()) return LedColor{};
  return pixels_[index];
}

LedColor LedManager::scaled(LedColor color) const {
  if (brightness_ == 255) return color;
  return LedColor{static_cast<uint8_t>((static_cast<uint16_t>(color.r) * brightness_) / 255),
                  static_cast<uint8_t>((static_cast<uint16_t>(color.g) * brightness_) / 255),
                  static_cast<uint8_t>((static_cast<uint16_t>(color.b) * brightness_) / 255)};
}

// Tight frame sender. Everything it touches lives in IRAM/DRAM/registers: a
// flash fetch inside the interrupt-masked window (under WiFi/web/audio load)
// stalls mid-bit, stretches the pulse, and the LEDs latch a corrupted frame.
static void IRAM_ATTR sendFrame(uint8_t pin, const uint8_t* bytes, uint32_t len, uint32_t t0h,
                                uint32_t t1h, uint32_t bit) {
  for (uint32_t i = 0; i < len; ++i) {
    const uint8_t value = bytes[i];
    for (uint8_t m = 0x80; m != 0; m >>= 1) {
      const uint32_t start = esp_cpu_get_cycle_count();
      gpioHigh(pin);
      waitUntil(start + ((value & m) ? t1h : t0h));
      gpioLow(pin);
      waitUntil(start + bit);
    }
  }
}

void LedManager::writePixels(const LedColor* colors, uint8_t count) {
  if (!begun_ || !colors || count == 0) return;

  bool anyLit = false;
  for (uint8_t i = 0; i < count && i < MAX_LEDS; ++i) {
    const LedColor scaledColor = scaled(colors[i]);
    if (scaledColor.r || scaledColor.g || scaledColor.b) {
      anyLit = true;
      break;
    }
  }
  if (!railOn_) {
    if (!anyLit) return;  // rail off + nothing to light = already dark
    if (!enablePower()) return;
    railOn_ = true;
  }
  // Precompute the byte stream and cycle timings before masking interrupts —
  // the masked window must not touch flash (see sendFrame). nsToCycles calls
  // ESP.getCpuFreqMHz(), a flash-resident function.
  uint8_t bytes[MAX_LEDS * 3];
  uint32_t len = 0;
  const bool grb = BoardConfig::ACTIVE.leds.colorOrder == BoardConfig::LedColorOrder::GRB;
  for (uint8_t i = 0; i < count && i < MAX_LEDS; ++i) {
    const LedColor color = scaled(colors[i]);
    bytes[len++] = grb ? color.g : color.r;
    bytes[len++] = grb ? color.r : color.g;
    bytes[len++] = color.b;
  }
  const uint8_t pin = BoardConfig::ACTIVE.leds.data;
  const uint32_t t0h = nsToCycles(T0H_NS);
  const uint32_t t1h = nsToCycles(T1H_NS);
  const uint32_t bit = nsToCycles(BIT_NS);
  noInterrupts();
  sendFrame(pin, bytes, len, t0h, t1h, bit);
  interrupts();
  // Reset/latch: these parts need >=280 us low; shorter gaps concatenate
  // back-to-back frames instead of latching them.
  delayMicroseconds(280);

  // All black: drop the rail. The LEDs are dark without it, and the rail's
  // indicator LED goes dark too.
  if (!anyLit && railOn_) {
    disablePower();
    railOn_ = false;
  }
}

void LedManager::show() { writePixels(pixels_, count()); }

void LedManager::clear() {
  const uint8_t n = count();
  for (uint8_t i = 0; i < n; ++i) {
    pixels_[i] = LedColor::black();
  }
  if (begun_) show();
}

void LedManager::flash(LedColor color, uint8_t count, uint16_t onMs, uint16_t offMs) {
  if (!begun_ && !begin()) return;
  if (count == 0) return;
  const uint8_t n = this->count();
  for (uint8_t i = 0; i < n; ++i) {
    savedPixels_[i] = pixels_[i];
  }
  flashColor_ = color;
  flashOnMs_ = onMs;
  flashOffMs_ = offMs;
  flashesRemaining_ = count;
  flashing_ = true;
  flashOn_ = false;
  flashNextAt_ = millis();
  update();
}

void LedManager::update() {
  if (!flashing_ || !begun_) return;
  const unsigned long now = millis();
  if ((long)(now - flashNextAt_) < 0) return;

  flashOn_ = !flashOn_;
  if (flashOn_) {
    const uint8_t n = count();
    LedColor temp[MAX_LEDS]{};
    for (uint8_t i = 0; i < n; ++i) {
      temp[i] = flashColor_;
    }
    writePixels(temp, n);
    flashNextAt_ = now + flashOnMs_;
  } else {
    writePixels(savedPixels_, count());
    flashNextAt_ = now + flashOffMs_;
    if (flashesRemaining_ > 0) --flashesRemaining_;
    if (flashesRemaining_ == 0) {
      stopFlash(true);
    }
  }
}

void LedManager::stopFlash(bool restore) {
  if (!flashing_) return;
  flashing_ = false;
  flashOn_ = false;
  flashesRemaining_ = 0;
  if (restore) {
    const uint8_t n = count();
    for (uint8_t i = 0; i < n; ++i) {
      pixels_[i] = savedPixels_[i];
    }
    show();
  }
}

}  // namespace freeink

#else

namespace freeink {

bool LedManager::begin() { return false; }
bool LedManager::present() const { return false; }
uint8_t LedManager::count() const { return 0; }
void LedManager::setBrightness(uint8_t brightness) { (void)brightness; }
void LedManager::setColor(uint8_t index, LedColor color) {
  (void)index;
  (void)color;
}
void LedManager::setAll(LedColor color) { (void)color; }
LedColor LedManager::color(uint8_t index) const {
  (void)index;
  return LedColor{};
}
void LedManager::show() {}
void LedManager::clear() {}
void LedManager::flash(LedColor color, uint8_t count, uint16_t onMs, uint16_t offMs) {
  (void)color;
  (void)count;
  (void)onMs;
  (void)offMs;
}
void LedManager::update() {}
void LedManager::stopFlash(bool restore) { (void)restore; }

}  // namespace freeink

#endif
