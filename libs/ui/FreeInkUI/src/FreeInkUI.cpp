#include <FreeInkUI.h>

namespace freeink {
namespace ui {

StyleSet defaultButtonStyles() {
  StyleSet styles;
  styles.normal.background = Paint::solid(Color::White);
  styles.normal.foreground = Paint::solid(Color::Black);
  styles.normal.border = Paint::solid(Color::Black);
  styles.normal.borderWidth = 1;

  styles.selected.background = Paint::solid(Color::Black);
  styles.selected.foreground = Paint::solid(Color::White);
  styles.selected.border = Paint::solid(Color::Black);
  styles.selected.borderWidth = 1;

  styles.focused.background = Paint::dither(Color::LightGray);
  styles.focused.foreground = Paint::solid(Color::Black);
  styles.focused.border = Paint::solid(Color::Black);
  styles.focused.borderWidth = 2;

  styles.active.background = Paint::solid(Color::Black);
  styles.active.foreground = Paint::solid(Color::White);
  styles.active.border = Paint::solid(Color::Black);
  styles.active.borderWidth = 1;

  styles.disabled.background = Paint::solid(Color::White);
  styles.disabled.foreground = Paint::dither(Color::LightGray);
  styles.disabled.border = Paint::dither(Color::LightGray);
  styles.disabled.borderWidth = 1;
  return styles;
}

StyleSet defaultListRowStyles() {
  StyleSet styles;
  styles.normal.background = Paint::solid(Color::White);
  styles.normal.foreground = Paint::solid(Color::Black);

  styles.selected.background = Paint::solid(Color::Black);
  styles.selected.foreground = Paint::solid(Color::White);

  styles.focused.background = Paint::dither(Color::LightGray);
  styles.focused.foreground = Paint::solid(Color::Black);
  styles.focused.border = Paint::solid(Color::Black);
  styles.focused.borderWidth = 1;

  styles.active = styles.selected;

  styles.disabled.background = Paint::solid(Color::White);
  styles.disabled.foreground = Paint::dither(Color::LightGray);
  return styles;
}

StyleSet defaultKeyStyles() {
  StyleSet styles = defaultButtonStyles();
  styles.normal.borderWidth = 0;
  styles.selected.borderWidth = 1;
  styles.focused.borderWidth = 1;
  return styles;
}

StyleSet defaultPopupStyles() {
  StyleSet styles;
  styles.normal.background = Paint::solid(Color::White);
  styles.normal.foreground = Paint::solid(Color::Black);
  styles.normal.border = Paint::solid(Color::Black);
  styles.normal.borderWidth = 2;
  styles.selected = styles.normal;
  styles.focused = styles.normal;
  styles.active = styles.normal;
  styles.disabled = styles.normal;
  return styles;
}

ThemeTokens defaultThemeTokens(FontId smallFont, FontId bodyFont, FontId titleFont) {
  ThemeTokens tokens;
  tokens.fontSmall = smallFont;
  tokens.fontBody = bodyFont;
  tokens.fontTitle = titleFont;
  tokens.smallText.font = smallFont;
  tokens.smallText.align = TextAlign::Left;
  tokens.bodyText.font = bodyFont;
  tokens.bodyText.align = TextAlign::Left;
  tokens.titleText.font = titleFont;
  tokens.titleText.align = TextAlign::Left;
  tokens.titleText.bold = true;
  tokens.button = defaultButtonStyles();
  tokens.listRow = defaultListRowStyles();
  tokens.key = defaultKeyStyles();
  tokens.popup = defaultPopupStyles();
  tokens.textField = defaultListRowStyles();
  tokens.textField.normal.border = Paint::solid(Color::Black);
  tokens.textField.normal.borderWidth = 1;
  tokens.textField.selected.border = Paint::solid(Color::Black);
  tokens.textField.selected.borderWidth = 2;
  return tokens;
}

Rect centeredRect(Rect outer, Size inner) {
  return Rect{static_cast<int16_t>(outer.x + (outer.width - inner.width) / 2),
              static_cast<int16_t>(outer.y + (outer.height - inner.height) / 2), inner.width, inner.height};
}

Rect ensureMinTouchRect(Rect visual, int16_t minSize, Rect bounds) {
  // Edge snap: hit rects whose edge lies within EDGE_SNAP_PX of a bounds edge
  // extend to that boundary. The touch transforms clamp bezel-adjacent taps to
  // the exact border pixels (touchToLogical / raw-range clamping), so an inset
  // control near an edge otherwise has a dead gutter its own users tap into —
  // an edge target should reach the physical edge (the Fitts's-law rule).
  constexpr int16_t EDGE_SNAP_PX = 12;

  Rect rect = visual;
  if (rect.width < minSize) {
    const int16_t delta = static_cast<int16_t>(minSize - rect.width);
    rect.x = static_cast<int16_t>(rect.x - delta / 2);
    rect.width = minSize;
  }
  if (rect.height < minSize) {
    const int16_t delta = static_cast<int16_t>(minSize - rect.height);
    rect.y = static_cast<int16_t>(rect.y - delta / 2);
    rect.height = minSize;
  }
  if (rect.x < bounds.x) rect.x = bounds.x;
  if (rect.y < bounds.y) rect.y = bounds.y;
  if (rect.right() > bounds.right()) rect.x = static_cast<int16_t>(bounds.right() - rect.width);
  if (rect.bottom() > bounds.bottom()) rect.y = static_cast<int16_t>(bounds.bottom() - rect.height);

  if (rect.x - bounds.x < EDGE_SNAP_PX) {
    rect.width = static_cast<int16_t>(rect.width + (rect.x - bounds.x));
    rect.x = bounds.x;
  }
  if (rect.y - bounds.y < EDGE_SNAP_PX) {
    rect.height = static_cast<int16_t>(rect.height + (rect.y - bounds.y));
    rect.y = bounds.y;
  }
  if (bounds.right() - rect.right() < EDGE_SNAP_PX) {
    rect.width = static_cast<int16_t>(bounds.right() - rect.x);
  }
  if (bounds.bottom() - rect.bottom() < EDGE_SNAP_PX) {
    rect.height = static_cast<int16_t>(bounds.bottom() - rect.y);
  }
  return rect;
}

uint16_t listVisibleRows(Rect rect, int16_t rowHeight, int16_t rowGap) {
  if (rect.height <= 0 || rowHeight <= 0) return 0;
  if (rowGap < 0) rowGap = 0;
  // n rows occupy n*rowHeight + (n-1)*rowGap, so add one trailing gap to both
  // sides of the division.
  return static_cast<uint16_t>((rect.height + rowGap) / (rowHeight + rowGap));
}

uint16_t listTopIndexFor(int16_t selectedIndex, uint16_t topIndex, uint16_t visibleRows, uint16_t count) {
  if (count == 0 || visibleRows == 0) return 0;
  const uint16_t maxTop = count > visibleRows ? static_cast<uint16_t>(count - visibleRows) : 0;
  uint16_t top = topIndex > maxTop ? maxTop : topIndex;
  if (selectedIndex >= 0 && selectedIndex < static_cast<int16_t>(count)) {
    const uint16_t selected = static_cast<uint16_t>(selectedIndex);
    if (selected < top) {
      top = selected;
    } else if (selected >= top + visibleRows) {
      top = static_cast<uint16_t>(selected - visibleRows + 1);
    }
  }
  return top > maxTop ? maxTop : top;
}

BitmapRef resolveBitmap(AssetResolver* resolver, const AssetRef& asset) {
  if (asset.bitmap) return asset.bitmap;
  if (!resolver || !asset) return BitmapRef{};
  return resolver->bitmapFor(asset);
}

int16_t clampInt16(int32_t value, int16_t minValue, int16_t maxValue) {
  if (value < minValue) return minValue;
  if (value > maxValue) return maxValue;
  return static_cast<int16_t>(value);
}

TextStyle textStyleWithForeground(TextStyle text, Paint foreground) {
  if (foreground.kind == PaintKind::Solid) {
    text.color = foreground.color;
    text.inverted = foreground.color == Color::White;
  }
  return text;
}

void drawText(DrawTarget& target, Rect rect, const char* text, TextStyle style) {
  if (!text || rect.empty()) return;
  target.text(rect, text, style);
}

void drawBitmap(DrawTarget& target, Rect rect, BitmapRef bitmap, BitmapMode mode, Paint foreground) {
  if (!bitmap || rect.empty()) return;
  target.bitmap(rect, bitmap, mode, foreground);
}

}  // namespace ui
}  // namespace freeink
