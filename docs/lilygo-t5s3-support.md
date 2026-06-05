# LilyGo T5 S3 (4.7" ED047TC1) — support status & bring-up

The LilyGo T5S3-4.7-ePaper (PRO/Lite) is an ESP32-S3 board whose panel is an
**ED047TC1: a raw 960×540 16-gray parallel EPD with no on-glass controller**. The
MCU clocks every row over the S3 LCD (i80) peripheral and an external PMIC
generates the waveform rails — a different display class from FreeInk's SPI
single-chip drivers (SSD1677/UC8253/ED2208).

Reference port: [ShallowGreen123/t5s3-reader](https://github.com/ShallowGreen123/t5s3-reader).

## What the SDK provides

- **`LgfxEpdDriver`** — wraps **LovyanGFX's `Panel_EPD`/`Bus_EPD`** (bundled in
  `m5stack/M5GFX`), `usesExternalBus() == true`. Gated by `FREEINK_DRIVER_LGFX_EPD`
  (derived from `FREEINK_DEVICE_LILYGO`), so M5GFX links only on this device.
- **`BoardConfig::LILYGO_T5S3`** profile — geometry, GT911 touch
  (`LILYGO_T5_PRO_GT911`), `DisplayController::LgfxEpd`. `CAP_TOUCH` auto-on.
- **GT911 touch** — already implemented in `InputManager`.

> Status: the display driver and profile compile and follow the validated
> reference port's LovyanGFX usage, but have **not been run on T5 S3 hardware** by
> the SDK author. Treat as "implemented, pending on-device validation."

## Board bring-up (what the consumer supplies)

The panel can't power up without the board's PMIC (TPS65185) + PCA9535 I²C
IO-expander sequence — that glue is board-specific, so `LgfxEpdDriver` has **no
default config**. The board injects everything LovyanGFX needs through an
`LgfxEpdConfig`:

```cpp
namespace freeink {
const LgfxEpdConfig& lilygoT5S3LgfxConfig() {
  static const LgfxEpdConfig cfg = {
      {EP_D0,EP_D1,EP_D2,EP_D3,EP_D4,EP_D5,EP_D6,EP_D7},  // 8-bit data bus
      EP_STH, EP_STV, /*OE dummy*/ T5S3_LORA_CS, EP_LEH, EP_CKH, EP_CKV,
      /*pwr dummy*/ T5S3_LORA_CS, /*busHz*/ 20'000'000, /*linePadding*/ 8,
      /*rotation*/ 1,
      { &prepareEpdPowerPins, &epdPowerOn, &epdPowerOff },  // PCA9535 + TPS65185 glue
  };
  return cfg;
}
}
```
Build with `-DFREEINK_DEVICE_LILYGO=1 -DFREEINK_LGFX_EPD_CONFIG=lilygoT5S3LgfxConfig`
and add `m5stack/M5GFX` to that env's `lib_deps` (see `platformio.sample.ini`). The
power-hook bodies (PCA9535 expander + TPS65185 PMIC register writes) live in the
board-support layer, not the SDK.

## Remaining gaps (board-support, not core SDK)

- **Battery** — the board uses a **BQ27220 fuel gauge + BQ25896 charger over I²C**,
  not an ADC divider. `BatteryMonitor` is ADC-only; an I²C-gauge backend (same
  pluggable shape as `SDCardManager`'s SPI-vs-SDMMC seam) would be the SDK-side fix.
- **Input** — the user button is behind the **PCA9535 expander**; `InputManager`
  reads direct GPIOs, and `PowerManager`'s deep-sleep wake assumes a direct GPIO
  pin. Expander-based buttons are handled by the board for now.
- **PCA9535 / TPS65185 / PCF85063 RTC / backlight / LoRa / GPS** — board-support
  peripherals; the SDK intentionally doesn't absorb them.
