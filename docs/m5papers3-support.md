# M5Stack PaperS3 (official)

ESP32-S3R8 (8 MB octal PSRAM, 16 MB flash) e-reader dev kit sold by M5Stack —
[shop.m5stack.com/products/m5papers3-esp32s3-development-kit](https://shop.m5stack.com/products/m5papers3-esp32s3-development-kit).
4.7" 960×540 16-gray e-paper, **GT911** capacitive touch, **BM8563** RTC, gyro,
buzzer, 1800 mAh battery. `BoardConfig::M5PAPERS3` (`Board::M5PaperS3`).

**Not the same board as this SDK's `PAPER_MONO` profile.** Despite the similar
naming ("M5Stack Paper Mono / PaperS3" in `BoardConfig.h`'s comments), that
profile targets different silicon entirely — see the [comparison
table](#not-the-same-board-as-paper-mono) below. If your physical unit is the one
`PAPER_MONO` describes (M5PM1 PMIC + M5IOE1 expander, FT6336 touch, RX8130 RTC,
800×480 SSD1677), use that profile, not this one.

Build: `-DFREEINK_DEVICE_M5PAPERS3=1 -DFREEINK_LGFX_EPD_CONFIG=m5PaperS3LgfxConfig`
(see `platformio.sample.ini` `[env:m5papers3]`). `FREEINK_DRIVER_LGFX_EPD`,
`FREEINK_CAP_TOUCH`, `FREEINK_CAP_RTC`, and `FREEINK_CAP_BUZZER` auto-enable.
Needs octal PSRAM (`board_build.arduino.memory_type = qio_opi` +
`-DBOARD_HAS_PSRAM`) and `-DUSE_BLOCK_DEVICE_INTERFACE=1` is **not** required
(SD is plain SPI here, not SDMMC).

## Source and confidence

No physical unit was available when this port was first written — every pin
below was read directly out of the official, MIT-licensed vendor libraries
that M5Stack ships for this exact product. It has since been bench-tested on a
real M5PaperS3 (see [Confirmed working on real hardware](#confirmed-working-on-real-hardware)
below); a handful of items are still open as testing continues:

- `m5stack/M5Unified` **v0.2.10** (`src/M5Unified.cpp`, `src/utility/Power_Class.cpp`)
- `m5stack/M5GFX` **v0.2.15** (`src/M5GFX.cpp`, board autodetect block for `board_M5PaperS3`)

Each field is marked:

- **CONFIRMED** — read directly from the vendor source above.
- **PENDING** — not found in the source read, or a value inferred by analogy to
  a sibling board (M5Paper v1.1 / LilyGo T5S3) that this SDK already models.
  Needs on-device validation before you trust it.

Independent corroboration: [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher)
— a multi-firmware SD-card launcher — ships its own bench-tested M5PaperS3 port
(`boards/m5stack-paper-s3/`), and its changelog records fixing a real touchscreen
bug on physical M5PaperS3 units. Its board bring-up calls `M5.begin()` and uses
`M5Unified`/`M5GFX` directly rather than hand-rolled pins, which is evidence that
those two libraries are sufficient and accurate for this board — the same
libraries this profile's CONFIRMED pins are sourced from.

## Not the same board as Paper Mono

| | `M5PAPERS3` (this doc) | `PAPER_MONO` (existing SDK profile) |
|---|---|---|
| Display | 960×540, raw-parallel EPD, no on-glass controller | 800×480, SSD1677 (SPI) |
| Touch | GT911 | FT6336 |
| RTC | BM8563 | RX8130 |
| Power/reset | direct ESP32 GPIOs | M5PM1 PMIC + M5IOE1 expander (I²C) |
| Source | official M5Unified/M5GFX | community bring-up (juicecultus/crosspoint-reader-papers3) |

## Display — raw-parallel EPD, 960×540, 16-gray

**CONFIRMED.** Same driver *class* as LilyGo T5S3's ED047TC1
(`FREEINK_DRIVER_LGFX_EPD` — a raw parallel bus with no on-glass controller,
clocked over the ESP32-S3's LCD/i80 peripheral), but a different panel and pin
set. Wiring lives in `m5PaperS3LgfxConfig()` (`M5PaperS3Board.h`), read from
`M5GFX.cpp`'s `board_M5PaperS3` autodetect block (`Bus_EPD`/`Panel_EPD` config):

| Signal | GPIO | Confidence |
|---|---|---|
| D0..D7 | 6, 14, 7, 12, 9, 11, 8, 10 | CONFIRMED |
| SPH (start pulse horizontal) | 13 | CONFIRMED |
| SPV (start pulse vertical) | 17 | CONFIRMED |
| OE (output enable) | 45 | CONFIRMED |
| LE (latch enable) | 15 | CONFIRMED |
| CL (clock) | 16 | CONFIRMED |
| CKV (clock vertical) | 18 | CONFIRMED |
| PWR (EPD rail enable) | 46 | CONFIRMED |

Bus speed 16 MHz, line padding 8, both CONFIRMED from `M5GFX.cpp`'s
`bus_cfg`/`cfg_detail`.

**CONFIRMED on real hardware — panel rotation = 0.** `LgfxEpdDriver` applies
orientation via `g_dev.setRotation(cfg.rotation)`, not via the panel's
`offset_rotation` field that M5GFX's own board-detect code sets to `3` for this
panel. `Panel_HasBuffer.cpp`'s `setRotation()`:

```cpp
_internal_rotation = ((r + offset_rotation) & 3) | ((r & 4) ^ (offset_rotation & 4));
_width = panel_width; _height = panel_height;
if (_internal_rotation & 1) std::swap(_width, _height);
```

Two earlier guesses (`rotation=1`, borrowed from LilyGo T5S3, then `3`, derived
from the additive formula alone) were bench-tested and both came out with the
image filling only about half the panel — because 1 and 3 are both **odd**,
and any odd `_internal_rotation` swaps width/height (960×540 becomes an
effective 540×960 drawn into a 960-wide panel). The fix: the official M5Stack
demo (`m5stack/M5PaperS3-UserDemo`, `main/hal/hal.cpp`) calls
`M5.begin(); M5.Display.setRotation(1);` — `setRotation(1)` **on top of** the
board's baked-in `offset_rotation=3`, giving `((1+3)&3)|((1&4)^(3&4)) = 0`, an
**even** result (no swap). Since `LgfxEpdDriver` fixes `offset_rotation=0`, the
`r` that reproduces that same `_internal_rotation=0` is `r=0`. `rotation=0` is
derived from the official demo's approach above, and bench-confirmed on a
physical M5PaperS3: full screen, right-side-up. (Two prior guesses, 1 and 3,
were each plausible-looking but wrong — see the derivation above for why.)

No PMIC/IO-expander sequencing is needed (unlike LilyGo's PCA9535+TPS65185):
the EPD rail is a plain GPIO (`PWR`, pin 46) that LovyanGFX's `Bus_EPD` drives
itself through `pinPwr`.

**FIXED 2026-08-24 — that delegation was broken, and it kept this panel dark.**
`LgfxEpdDriver`'s `FreeInkBusEPD::powerControl()` overrode
`lgfx::Bus_EPD::powerControl()` and never called the base implementation: it
ran the board's power hook and returned. That is correct for LilyGo T5S3,
whose rails live behind an I²C PMIC (the hook *is* the power-up, and whose
`pin_pwr`/`pin_oe` are parked on a placeholder GPIO the base class must not
drive). It is wrong for this board, whose hooks are all `nullptr` — there was
nothing to replace the base sequence *with*, so:

```cpp
lgfx::gpio_hi(_config.pin_oe);    // GPIO45  } the entire
lgfx::gpio_hi(_config.pin_pwr);   // GPIO46  } power-up,
lgfx::gpio_hi(_config.pin_spv);   // GPIO17  } never ran
```

`Bus_EPD::init()` *does* configure all three as outputs, so they sat as
outputs driving LOW: the EPD rail never came up. The failure is silent and
extremely misleading — the SoC is fine, so the firmware boots, mounts SD,
joins WiFi, pairs BLE, and every draw call returns success with plausible
timings (`displayBuffer()` reporting ~29 ms, `[paint] returned after 461 ms`)
while the glass never changes. Same family as the discarded `init()` return
value fixed just above this in the log: on this panel the drive is timed
open-loop, with no ready/ack line to poll, so nothing downstream can tell
that the electronics never woke up.

It also produced a red herring that cost most of a day: the panel *did* work
if M5Stack's own Launcher had run first, because Launcher's `M5GFX` drove
those same pins on its way past. That looked like "this port needs Launcher's
bootloader", and an early A/B (freshly-compiled bootloader → dark panel;
Launcher's bootloader → working panel) seemed to confirm it. Both halves of
that A/B ran the same buggy `powerControl()`, so the bootloader was never the
variable. Fixed by delegating to the base class whenever no power hooks are
supplied; verified on hardware by booting this firmware as the only app on
the device, with no Launcher present at all.

Watch for the same shape elsewhere: an override that *replaces* a vendor
base-class method rather than wrapping it, on a board that supplied nothing
to replace it with.

## Touch — GT911

**CONFIRMED pins** (`M5GFX.cpp`, `Touch_GT911` config for `board_M5PaperS3`):
SDA=41, SCL=42, INT=48, 400 kHz, no reset pin wired (self-loads on power-up).
Vendor code probes I²C address `0x14` before `0x5D`.

**CONFIRMED raw range**, in the digitizer's native portrait frame: X 0..539,
Y 0..959 — matches this profile's `swapXY=true` mapping onto the 960×540
landscape panel (same geometry LilyGo T5S3 and M5Paper v1.1 already use).

**CONFIRMED on real hardware — `gt911CoordsAtByte0=true`, `flipX=false`,
`flipY=true` are correct as inherited from M5Paper v1.1.** Verified 2026-08-21
with a 4-corner-tap test (MicroWriter-BASIC-PaperS3's bring-up program: four crosshair
targets at the panel's extreme corners, tap position echoed back both as raw
normalized coordinates over serial and as a crosshair redrawn at
`tapToLogical()`'s computed position). All four taps produced distinct, stable,
repeatable logical positions at the correct corresponding corners, and the
crosshair visibly landed under the finger for all four — no mirroring, no swap
needed. No flip change required.

## SD card — SPI

**CONFIRMED** (`M5Unified.cpp` `_pin_table_spi_sd`): CLK=39, MOSI=38, MISO=40,
CS=47. Plain SdFat-over-SPI — `FREEINK_SD_SDMMC` does **not** auto-enable for
this device (unlike de-link/X4 Pro/Paper Mono).

## RTC — BM8563

**CONFIRMED present** (M5Stack's product page lists "internal RTC (BM8563)").
BM8563 is register/address-compatible with the NXP PCF8563 (same command set,
address `0x51`), so the profile uses `RtcType::Pcf8563` — the same mapping the
Sticky and X4 Pro profiles use for their own BM8563 chips. Shares the touch I²C
bus (SDA=41, SCL=42).

## Buzzer

**CONFIRMED** (`M5Unified.cpp`, `spk_cfg` for `board_M5PaperS3`): plain LEDC
tone pin on GPIO21, no output codec.

## Power

**CONFIRMED — charge status**: GPIO4, read LOW while charging
(`Power_Class.cpp`, `M5PaperS3_CHG_STAT_PIN`).

**CONFIRMED — power-off is a pulse train, not a level.** `Power_Class.cpp`'s
`_powerOff()` pulses GPIO44 (`PWROFF_PULSE_PIN`) LOW→HIGH five times, 50 ms per
edge, before the board's power circuit actually lets go — a simple
`digitalWrite(LOW)` does **not** turn the board off. This doesn't fit
`BoardConfig::PowerConfig`'s hold-latch model (which is for boards that need a
pin driven HIGH at boot to survive USB unplug, e.g. Sticky/M5Paper v1.1/LilyGo —
M5PaperS3 shows no evidence of needing that), so it's a board-support function
instead: `freeink::m5papers3::powerOff()` in `M5PaperS3Board.h`. Call it from
the consumer's power-off path instead of releasing a `PowerConfig` latch.

**CONFIRMED — battery ADC.** `Power_Class.cpp`'s `board_M5PaperS3` case sets
`_batAdcCh = ADC1_GPIO3_CHANNEL` (`_batAdcUnit = 1`) with `_pmic = pmic_adc` and
`_adc_ratio = 2.0f` — GPIO3 on ADC1, 2:1 divider (`batteryDividerMultiplier`).
No I²C fuel gauge; `batteryGauge` stays unassigned.

**PENDING — no confirmed navigation buttons.** No button-read GPIO (beyond the
write-only power-off pulse pin above) was found in the source read. The device
is modeled as touch-only (`InputStyle::DigitalButtons` with every `InputPins`
field unassigned) — all navigation is expected to come through the GT911 touch
panel. If your unit has a physical button that responds to input, it needs its
GPIO identified and added to the profile.

**PENDING — IMU.** M5Stack's product page advertises a gyroscope sensor,
but the exact chip and I²C address weren't identified in the source areas
read for this port. `ImuType::None` for now; `FREEINK_CAP_IMU` is off.

## Confirmed working on real hardware

- **Display orientation** (`rotation=0`) — correct, full screen, right-side-up.
- **Touch navigation** — functional (swipe-up-from-bottom opens the menu,
  general navigation works) with the inferred `swapXY`/`flipX`/`flipY` values
  still in place; no corner-tap recalibration has been needed so far.
- **General firmware operation** — reported working end-to-end by the owner on
  their physical unit as of this update. Testing is ongoing over the following
  days, so treat this as "no known-broken items found yet," not an exhaustive
  per-subsystem sign-off — the items below are still specifically unverified.

## Still to verify

1. **RTC** — confirm the BM8563 responds at 0x51 on SDA41/SCL42 as a PCF8563
   and keeps time correctly across reboots.
2. **Battery** — confirm the GPIO3 ADC reading tracks real battery voltage
   sensibly (not just that it compiles).
3. **Power-off** — confirm `freeink::m5papers3::powerOff()` actually powers the
   board down; the pulse count/timing (5× 50 ms) is copied from the vendor
   library.
4. **Buttons / IMU** — still PENDING (see above); expect no physical-button
   input until a nav-button GPIO is identified, and no IMU readings until its
   chip/address is confirmed.
