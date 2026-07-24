#include "XteinkDetect.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <Wire.h>

#include <string.h>

namespace freeink {

#if !(FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3)

// Neither Xteink profile is in this build, so there is nothing to fingerprint.
// Probing would also be unsafe here: SDA=20 / SCL=0 are only free pins on the
// Xteink C3 pinout — on an ESP32-S3, GPIO20 is native USB D+ and GPIO0 is the
// boot strap.
XteinkVerdict detectXteinkVerdict(uint8_t* score1, uint8_t* score2) {
  if (score1) *score1 = 0;
  if (score2) *score2 = 0;
  return XteinkVerdict::Inconclusive;
}
bool detectXteinkIsX3() { return false; }
X3DisplayVerdict detectX3DisplayController(uint8_t verBytes[5], uint8_t* flg) {
  if (verBytes) memset(verBytes, 0, 5);
  if (flg) *flg = 0;
  return X3DisplayVerdict::Uc8253Assumed;
}
bool selectXteinkDevice() { return false; }

#else

namespace {

// X3-only peripherals on the secondary I2C bus (SDA=20, SCL=0).
constexpr int X3_I2C_SDA = 20;
constexpr int X3_I2C_SCL = 0;
constexpr uint32_t X3_I2C_FREQ = 400000;

constexpr uint8_t ADDR_BQ27220 = 0x55;  // fuel gauge
constexpr uint8_t ADDR_DS3231 = 0x68;   // RTC
constexpr uint8_t ADDR_QMI8658 = 0x6B;  // IMU
constexpr uint8_t ADDR_QMI8658_ALT = 0x6A;

constexpr uint8_t BQ27220_SOC_REG = 0x2C;
constexpr uint8_t BQ27220_VOLT_REG = 0x08;
constexpr uint8_t DS3231_SEC_REG = 0x00;
constexpr uint8_t QMI8658_WHO_AM_I_REG = 0x00;
constexpr uint8_t QMI8658_WHO_AM_I_VALUE = 0x05;

bool readReg8(uint8_t addr, uint8_t reg, uint8_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) return false;
  *out = Wire.read();
  return true;
}

bool readReg16LE(uint8_t addr, uint8_t reg, uint16_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) Wire.read();
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *out = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

// Each probe checks not just for an ACK but for a plausible value, so a stray
// pull-up or floating bus can't masquerade as a present chip.
bool probeBq27220() {
  uint16_t soc = 0;
  uint16_t mv = 0;
  if (!readReg16LE(ADDR_BQ27220, BQ27220_SOC_REG, &soc) || soc > 100) return false;
  if (!readReg16LE(ADDR_BQ27220, BQ27220_VOLT_REG, &mv)) return false;
  return mv >= 2500 && mv <= 5000;
}

bool probeDs3231() {
  uint8_t sec = 0;
  if (!readReg8(ADDR_DS3231, DS3231_SEC_REG, &sec)) return false;
  const uint8_t tens = (sec >> 4) & 0x07;
  const uint8_t ones = sec & 0x0F;
  return tens <= 5 && ones <= 9;  // valid BCD seconds
}

bool probeQmi8658() {
  uint8_t who = 0;
  if (readReg8(ADDR_QMI8658, QMI8658_WHO_AM_I_REG, &who) && who == QMI8658_WHO_AM_I_VALUE) return true;
  if (readReg8(ADDR_QMI8658_ALT, QMI8658_WHO_AM_I_REG, &who) && who == QMI8658_WHO_AM_I_VALUE) return true;
  return false;
}

uint8_t runProbePass() {
  Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
  Wire.setTimeOut(6);
  const uint8_t score =
      static_cast<uint8_t>(probeBq27220()) + static_cast<uint8_t>(probeDs3231()) + static_cast<uint8_t>(probeQmi8658());
  Wire.end();
  pinMode(X3_I2C_SDA, INPUT);
  pinMode(X3_I2C_SCL, INPUT);
  return score;
}

}  // namespace

XteinkVerdict detectXteinkVerdict(uint8_t* score1, uint8_t* score2) {
  const uint8_t pass1 = runProbePass();
  delay(2);
  const uint8_t pass2 = runProbePass();
  if (score1) *score1 = pass1;
  if (score2) *score2 = pass2;
  // X3 confirmed only when both passes see at least two of the three chips; the
  // X4 sees zero, so a single stray ACK never flips the result. Anything in
  // between is Inconclusive: callers should run as X4 but may re-probe later.
  if (pass1 >= 2 && pass2 >= 2) return XteinkVerdict::X3Confirmed;
  if (pass1 == 0 && pass2 == 0) return XteinkVerdict::X4Confirmed;
  return XteinkVerdict::Inconclusive;
}

bool detectXteinkIsX3() { return detectXteinkVerdict() == XteinkVerdict::X3Confirmed; }

#if FREEINK_DEVICE_X3

namespace {

// X3 display pins (shared by both X3 controller variants; see XTEINK_X3).
constexpr int8_t EPD_SCLK = 8;
constexpr int8_t EPD_MOSI = 10;  // the controller's bidirectional SDA
constexpr int8_t EPD_CS = 21;
constexpr int8_t EPD_DC = 4;
constexpr int8_t EPD_RST = 5;
constexpr int8_t EPD_BUSY = 6;

// UC8279d read-capable registers (UC8279d_B 0.1 datasheet).
constexpr uint8_t UC8279_CMD_VER = 0x70;  // reserved, CHIP_VER, LUT_VER[23:0]
constexpr uint8_t UC8279_CMD_FLG = 0x71;  // status; BUSY_N (D0) = 1 when idle

inline void epdClockDelay() { delayMicroseconds(1); }  // ~500 kHz, timing-safe

void epdWriteByte(uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(EPD_MOSI, (b & 0x80) ? HIGH : LOW);
    epdClockDelay();
    digitalWrite(EPD_SCLK, HIGH);
    epdClockDelay();
    digitalWrite(EPD_SCLK, LOW);
    b <<= 1;
  }
}

uint8_t epdReadByte() {
  uint8_t b = 0;
  for (uint8_t i = 0; i < 8; i++) {
    // The controller shifts the next bit out on the SCL falling edge; sample
    // while the clock is low, then pulse.
    epdClockDelay();
    b = static_cast<uint8_t>((b << 1) | (digitalRead(EPD_MOSI) == HIGH ? 1 : 0));
    digitalWrite(EPD_SCLK, HIGH);
    epdClockDelay();
    digitalWrite(EPD_SCLK, LOW);
  }
  return b;
}

// One command + N-byte half-duplex read: command with DC low, then SDA (our
// MOSI) released to input with DC high while the controller drives the reads.
void epdCmdRead(uint8_t cmd, uint8_t* out, uint8_t len) {
  pinMode(EPD_MOSI, OUTPUT);
  digitalWrite(EPD_DC, LOW);
  digitalWrite(EPD_CS, LOW);
  epdClockDelay();
  epdWriteByte(cmd);
  digitalWrite(EPD_DC, HIGH);
  pinMode(EPD_MOSI, INPUT_PULLUP);
  epdClockDelay();
  for (uint8_t i = 0; i < len; i++) out[i] = epdReadByte();
  digitalWrite(EPD_CS, HIGH);
  pinMode(EPD_MOSI, OUTPUT);
}

// The UC8279 VER signature: a leading reserved 0x00, then a CHIP_VER byte
// (datasheet default 0x03, MTP-programmed so not pinned to that exact value)
// that is neither a floating-low nor a floating-high bus. A released SDA reads
// back all-0x00 or all-0xFF through the pull-up, and the UC8253 places its
// revision in the FIRST byte of its 0x70 read (UC815x-family REV layout), so a
// genuine 0x00 lead followed by a non-trivial byte is discriminating. FLG must
// additionally report idle (BUSY_N=1) without being a floating pattern.
bool matchUc8279(const uint8_t ver[5], uint8_t flg) {
  if (ver[0] != 0x00) return false;
  if (ver[1] == 0x00 || ver[1] == 0xFF) return false;
  if (flg == 0x00 || flg == 0xFF) return false;
  return (flg & 0x01) == 0x01;
}

bool runDisplayProbePass(uint8_t ver[5], uint8_t* flg) {
  pinMode(EPD_CS, OUTPUT);
  digitalWrite(EPD_CS, HIGH);
  pinMode(EPD_SCLK, OUTPUT);
  digitalWrite(EPD_SCLK, LOW);
  pinMode(EPD_DC, OUTPUT);
  digitalWrite(EPD_DC, LOW);
  pinMode(EPD_MOSI, OUTPUT);
  pinMode(EPD_BUSY, INPUT);

  // Hardware reset pulse (RST_N min low width 50 us; give it 1 ms) and wait
  // for the controller to come ready (BUSY_N high). The panel driver's own
  // begin() resets again afterwards, so this leaves no lasting state.
  pinMode(EPD_RST, OUTPUT);
  digitalWrite(EPD_RST, HIGH);
  delay(2);
  digitalWrite(EPD_RST, LOW);
  delay(1);
  digitalWrite(EPD_RST, HIGH);
  {
    const unsigned long t0 = millis();
    while (digitalRead(EPD_BUSY) == LOW && millis() - t0 < 30) delay(1);
  }

  uint8_t flgByte = 0;
  epdCmdRead(UC8279_CMD_FLG, &flgByte, 1);
  epdCmdRead(UC8279_CMD_VER, ver, 5);
  if (flg) *flg = flgByte;
  return matchUc8279(ver, flgByte);
}

void releaseDisplayPins() {
  // Same convention as the I2C probe: leave everything released. RST_N has an
  // internal pull-up, so INPUT keeps the controller out of reset.
  pinMode(EPD_SCLK, INPUT);
  pinMode(EPD_MOSI, INPUT);
  pinMode(EPD_CS, INPUT_PULLUP);  // don't leave the panel selected
  pinMode(EPD_DC, INPUT);
  pinMode(EPD_RST, INPUT);
}

}  // namespace

X3DisplayVerdict detectX3DisplayController(uint8_t verBytes[5], uint8_t* flg) {
  uint8_t ver1[5] = {0};
  uint8_t ver2[5] = {0};
  uint8_t flg1 = 0;
  const bool pass1 = runDisplayProbePass(ver1, &flg1);
  delay(2);
  const bool pass2 = runDisplayProbePass(ver2, nullptr);
  releaseDisplayPins();
  if (verBytes) memcpy(verBytes, ver1, 5);
  if (flg) *flg = flg1;
  // Confirmed only when both passes match the UC8279 signature AND agree on
  // the VER bytes — a floating bus can't produce the same stable non-trivial
  // pattern twice. Disagreement is Inconclusive (resolve as UC8253, the
  // shipping controller, but don't persist so a flaky boot re-probes).
  if (pass1 && pass2 && memcmp(ver1, ver2, 5) == 0) return X3DisplayVerdict::Uc8279Confirmed;
  if (!pass1 && !pass2) return X3DisplayVerdict::Uc8253Assumed;
  return X3DisplayVerdict::Inconclusive;
}

#else  // X4-only build: no X3 profile, nothing to probe.

X3DisplayVerdict detectX3DisplayController(uint8_t verBytes[5], uint8_t* flg) {
  if (verBytes) memset(verBytes, 0, 5);
  if (flg) *flg = 0;
  return X3DisplayVerdict::Uc8253Assumed;
}

#endif  // FREEINK_DEVICE_X3

bool selectXteinkDevice() {
  const bool isX3 = detectXteinkIsX3();
  if (!isX3) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX4);
    return false;
  }
  // X3 confirmed: fingerprint which panel controller this production run
  // carries and select the matching sibling profile.
  const bool isUc8279 = detectX3DisplayController() == X3DisplayVerdict::Uc8279Confirmed;
  BoardConfig::selectDevice(isUc8279 ? BoardConfig::Board::XteinkX3Uc8279 : BoardConfig::Board::XteinkX3);
  return true;
}

#endif  // FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3

}  // namespace freeink
