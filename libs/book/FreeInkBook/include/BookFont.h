#pragma once

// FreeInk SDK — font metrics interface for FreeInkBook layout.
//
// Layout needs only metrics: how wide is a codepoint at a size, how tall is a
// line. Rasterization stays behind the application's renderer (FreeInkUI
// DrawTarget, FreeType, or a test fake), so layout runs host-side with no
// font files at all. Sizes are pixels; style flags travel with each text run
// so bold/italic variants can report different advances.

#include <stdint.h>

namespace freeink {
namespace book {

// Per-run style bits, kept small enough to live in every page text run.
enum StyleFlags : uint8_t {
  StyleNone = 0,
  StyleBold = 1u << 0,
  StyleItalic = 1u << 1,
};

class BookFont {
 public:
  virtual ~BookFont() = default;

  // Horizontal advance of one codepoint, in pixels.
  virtual int16_t advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) = 0;

  // Baseline-to-baseline line height for a size, in pixels.
  virtual int16_t lineHeight(uint16_t sizePx) = 0;

  // Distance from line top to baseline, in pixels.
  virtual int16_t ascent(uint16_t sizePx) = 0;

  // Kerning adjustment applied between `left` and `right`, in pixels
  // (usually zero or negative). Layout adds this during measurement so line
  // breaks account for it; fonts without kerning data keep the default.
  virtual int16_t kerning(uint32_t left, uint32_t right, uint16_t sizePx, uint8_t styleFlags) {
    (void)left;
    (void)right;
    (void)sizePx;
    (void)styleFlags;
    return 0;
  }
};

}  // namespace book
}  // namespace freeink
