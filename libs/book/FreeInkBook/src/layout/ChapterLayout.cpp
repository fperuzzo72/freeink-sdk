// FreeInkBook — streaming SAX → block flow → measured lines → placed pages.
//
// Phase 4 shape: paragraphs are laid out in two phases. MEASURE breaks the
// paragraph into line records (libunibreak opportunities, kerning-aware
// widths, hyphenation at overflow); PLACE walks those records onto pages
// with widow/orphan control and resolves alignment (including justification
// as word-level runs with distributed space). Styles come from a per-element
// stack fed by element defaults, the book stylesheet, and inline style
// attributes. Memory remains O(paragraph + page).

#include "layout/ChapterLayout.h"

#include <string.h>

#include <linebreak.h>

#include "epub/ImageProbe.h"
#include "epub/PackageParsers.h"
#include "epub/XmlSax.h"

namespace freeink {
namespace book {

namespace {

// Fixed working-set sizes. These bound layout memory regardless of chapter
// size; a paragraph longer than the buffer is flushed in segments (a layout
// artifact for pathological paragraphs, never a failure).
constexpr uint32_t kParTextCap = 8192;
constexpr uint16_t kMaxSpans = 128;
constexpr uint16_t kMaxRunsPerPage = 768;  // word-level runs when justified
constexpr uint16_t kMaxLinesPerPar = 512;
constexpr uint32_t kPageArenaCap = 24 * 1024;
constexpr uint8_t kMaxElemDepth = 24;
constexpr uint16_t kMaxImagesPerPage = 16;

// LineRec flags.
constexpr uint8_t kLineLast = 1u << 0;    // paragraph-final or hard break — never justify
constexpr uint8_t kLineHyphen = 1u << 1;  // render a hyphen after the last run

const char* localName(const char* qname) {
  const char* colon = strrchr(qname, ':');
  return colon != nullptr ? colon + 1 : qname;
}

const char* attrLocal(const char** atts, const char* local) {
  for (int i = 0; atts != nullptr && atts[i] != nullptr; i += 2) {
    const char* colon = strrchr(atts[i], ':');
    const char* name = colon != nullptr ? colon + 1 : atts[i];
    if (strcmp(name, local) == 0) return atts[i + 1];
  }
  return nullptr;
}

// Counts UTF-8 codepoints in text[0..len).
uint32_t countChars(const char* text, uint32_t len) {
  uint32_t chars = 0;
  for (uint32_t i = 0; i < len; ++i) {
    if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80) ++chars;
  }
  return chars;
}

// Decodes the UTF-8 codepoint starting at text[i]; advances i past it.
uint32_t decodeUtf8(const char* text, uint32_t len, uint32_t& i) {
  const uint8_t b0 = static_cast<uint8_t>(text[i]);
  uint32_t cp = b0;
  uint32_t extra = 0;
  if (b0 >= 0xF0) {
    cp = b0 & 0x07;
    extra = 3;
  } else if (b0 >= 0xE0) {
    cp = b0 & 0x0F;
    extra = 2;
  } else if (b0 >= 0xC0) {
    cp = b0 & 0x1F;
    extra = 1;
  }
  ++i;
  while (extra > 0 && i < len && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80) {
    cp = (cp << 6) | (static_cast<uint8_t>(text[i]) & 0x3F);
    ++i;
    --extra;
  }
  return cp;
}

bool isAsciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// CJK ideographs, kana, Hangul, fullwidth forms, and the supplementary
// ideographic planes — the scripts that justify by inter-character expansion
// and take a quarter-em gap against Latin runs.
bool isCjk(uint32_t cp) {
  return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFF60) ||
         (cp >= 0x20000 && cp <= 0x3FFFF);
}

bool isLatinWordChar(uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

// A script boundary that conventionally gets a quarter-em of air (Japanese
// typesetting practice for Latin words embedded in CJK text).
bool crossesScripts(uint32_t a, uint32_t b) {
  return (isCjk(a) && isLatinWordChar(b)) || (isLatinWordChar(a) && isCjk(b));
}

// User-agent defaults per element, expressed as CSS so the book's own CSS
// cascades over them naturally. Margins are % of em.
CssDecl elementDefaults(const char* local) {
  CssDecl d;
  if (strcmp(local, "h1") == 0) {
    d.sizePct = 200;
    d.weightBold = 1;
    d.marginTopPct = 100;
    d.marginBottomPct = 50;
  } else if (strcmp(local, "h2") == 0) {
    d.sizePct = 160;
    d.weightBold = 1;
    d.marginTopPct = 90;
    d.marginBottomPct = 45;
  } else if (strcmp(local, "h3") == 0) {
    d.sizePct = 130;
    d.weightBold = 1;
    d.marginTopPct = 80;
    d.marginBottomPct = 40;
  } else if (strcmp(local, "h4") == 0 || strcmp(local, "h5") == 0 || strcmp(local, "h6") == 0) {
    d.sizePct = 115;
    d.weightBold = 1;
    d.marginTopPct = 70;
    d.marginBottomPct = 35;
  } else if (strcmp(local, "blockquote") == 0) {
    d.styleItalic = 1;
    d.marginTopPct = 40;
    d.marginBottomPct = 40;
  } else if (strcmp(local, "li") == 0) {
    d.marginTopPct = 10;
    d.marginBottomPct = 10;
  } else if (strcmp(local, "p") == 0 || strcmp(local, "div") == 0) {
    d.marginBottomPct = 40;
  } else if (strcmp(local, "b") == 0 || strcmp(local, "strong") == 0) {
    d.weightBold = 1;
  } else if (strcmp(local, "i") == 0 || strcmp(local, "em") == 0 || strcmp(local, "cite") == 0) {
    d.styleItalic = 1;
  } else if (strcmp(local, "small") == 0) {
    d.sizePct = 87;
  } else if (strcmp(local, "sub") == 0 || strcmp(local, "sup") == 0) {
    d.sizePct = 75;  // no baseline shift yet
  }
  return d;
}

int16_t blockIndentFor(const char* local, uint16_t baseSizePx) {
  if (strcmp(local, "blockquote") == 0) return static_cast<int16_t>(baseSizePx * 2);
  if (strcmp(local, "li") == 0) return static_cast<int16_t>(baseSizePx * 3 / 2);
  return 0;
}

bool isBlockElement(const char* local) {
  static const char* kBlocks[] = {"p",       "h1",      "h2",         "h3",    "h4",
                                  "h5",      "h6",      "blockquote", "li",    "div",
                                  "section", "article", "figure",     "aside", "figcaption",
                                  "ul",      "ol",      "table",      "tr",    "td",
                                  "th",      "dt",      "dd"};
  for (const char* b : kBlocks) {
    if (strcmp(local, b) == 0) return true;
  }
  return false;
}

bool isSuppressedElement(const char* local) {
  return strcmp(local, "head") == 0 || strcmp(local, "style") == 0 ||
         strcmp(local, "script") == 0 || strcmp(local, "title") == 0;
}

class LayoutEngine : public XmlHandler {
 public:
  LayoutEngine(BookSource& source, const ZipCatalog& zip, const char* chapterHref,
               const LayoutParams& params, Arena& scratch, PageSink& sink)
      : source_(source), zip_(zip), params_(params), scratch_(scratch), sink_(sink) {
    if (!dirName(chapterHref != nullptr ? chapterHref : "", chapterDir_, sizeof(chapterDir_))) {
      chapterDir_[0] = '\0';
    }
  }

  bool init() {
    parText_ = static_cast<char*>(scratch_.alloc(kParTextCap, 1));
    breaks_ = static_cast<char*>(scratch_.alloc(kParTextCap, 1));
    spans_ = scratch_.allocArray<Span>(kMaxSpans);
    runs_ = scratch_.allocArray<PageTextRun>(kMaxRunsPerPage);
    images_ = scratch_.allocArray<PageImage>(kMaxImagesPerPage);
    lines_ = scratch_.allocArray<LineRec>(kMaxLinesPerPar);
    // Page-run text lives in its own sub-arena, NOT in `scratch_`: the XML
    // parse that drives this engine keeps its inflate window and parser
    // buffers live in `scratch_` for the whole chapter, and those are
    // allocated *after* init() runs. Releasing per-page copies from the
    // shared arena would free the decompressor out from under the reader.
    void* pageBlock = scratch_.alloc(kPageArenaCap, alignof(max_align_t));
    if (parText_ == nullptr || breaks_ == nullptr || spans_ == nullptr || runs_ == nullptr ||
        images_ == nullptr || lines_ == nullptr || pageBlock == nullptr) {
      return false;
    }
    pageArena_.init(pageBlock, kPageArenaCap);
    pageY_ = params_.marginTop;

    ElemState root;
    root.sizePct = 100;
    root.flags = StyleNone;
    root.align = params_.defaultAlign;
    root.textIndentPct = 0;
    root.displayNone = false;
    stack_[0] = root;
    stackTop_ = 0;

    latchParagraph("p", CssDecl{});
    return true;
  }

  // --- XmlHandler ----------------------------------------------------------

  void onStartElement(const char* name, const char** atts) override {
    if (failed_ || stopParse) return;
    const char* local = localName(name);
    if (isSuppressedElement(local)) {
      ++suppress_;
      return;
    }
    if (strcmp(local, "body") == 0) {
      inBody_ = true;
      // Books style body{} heavily (font-size, text-align); apply it as the
      // root of the element stack.
      if (params_.stylesheet != nullptr) {
        CssDecl bodyDecl = elementDefaults("body");
        bodyDecl.applyOver(cascadeFor(*params_.stylesheet, "body", attrLocal(atts, "class"),
                                      nullptr));
        pushState(bodyDecl);
        latchParagraphFromStack();
      }
      return;
    }
    ++depth_;
    if (!inBody_ || suppress_ > 0) return;

    // Resolve this element's style: defaults ← book CSS ← inline style.
    CssDecl decl = elementDefaults(local);
    const char* styleAttr = attrLocal(atts, "style");
    CssDecl inlineDecl;
    bool hasInline = false;
    if (styleAttr != nullptr) {
      inlineDecl = parseInlineStyle(styleAttr);
      hasInline = true;
    }
    if (params_.stylesheet != nullptr) {
      decl.applyOver(cascadeFor(*params_.stylesheet, local, attrLocal(atts, "class"),
                                hasInline ? &inlineDecl : nullptr));
    } else if (hasInline) {
      decl.applyOver(inlineDecl);
    }
    pushState(decl);

    if (isBlockElement(local)) {
      flushParagraph();
      latchParagraph(local, decl);
    } else if (strcmp(local, "br") == 0) {
      appendRaw('\n');
    } else if (strcmp(local, "hr") == 0) {
      flushParagraph();
      advanceY(params_.font->lineHeight(params_.baseSizePx));
    } else if (strcmp(local, "img") == 0 || strcmp(local, "image") == 0) {
      const char* src = attrLocal(atts, "src");
      if (src == nullptr) src = attrLocal(atts, "href");  // SVG xlink:href
      if (src != nullptr && !stack_[stackTop_].displayNone) {
        flushParagraph();
        placeImage(src);
      }
    } else {
      noteStyleChange();  // inline element may have changed flags/size
    }
  }

  void onEndElement(const char* name) override {
    if (failed_ || stopParse) return;
    const char* local = localName(name);
    if (isSuppressedElement(local)) {
      if (suppress_ > 0) --suppress_;
      return;
    }
    if (strcmp(local, "body") == 0) {
      flushParagraph();
      inBody_ = false;
      return;
    }
    if (depth_ > 0) --depth_;
    if (!inBody_ || suppress_ > 0) return;
    popState();

    if (isBlockElement(local)) {
      flushParagraph();
      // Text following the closed block flows in the parent's style with no
      // extra spacing.
      latchParagraphFromStack();
    } else {
      noteStyleChange();
    }
  }

  void onText(const char* text, int len) override {
    if (failed_ || stopParse || !inBody_ || suppress_ > 0) return;
    if (stack_[stackTop_].displayNone) return;
    for (int i = 0; i < len; ++i) {
      const char c = text[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        pendingSpace_ = parLen_ > 0;  // collapse runs; drop leading whitespace
      } else {
        if (pendingSpace_) {
          appendRaw(' ');
          pendingSpace_ = false;
        }
        appendRaw(c);
      }
    }
  }

  // --- Completion ----------------------------------------------------------

  bool finish() {
    if (failed_) return false;
    flushParagraph();
    if (runCount_ > 0 || imageCount_ > 0) emitPage();
    return !failed_;
  }

  uint32_t pageCount() const { return pageCount_; }
  bool outOfMemory() const { return failed_; }

 private:
  struct Span {
    uint16_t start;
    uint8_t flags;
    uint16_t sizePct;
  };

  struct LineRec {
    uint32_t start;
    uint32_t end;
    int32_t naturalWidth;  // includes the hyphen when kLineHyphen is set
    uint16_t spaceCount;   // adjustable spaces (Latin justification)
    uint16_t cjkGaps;      // CJK-CJK boundaries (inter-character justification)
    uint8_t flags;
  };

  struct ElemState {
    uint16_t sizePct;
    uint8_t flags;
    TextAlign align;
    int16_t textIndentPct;
    bool displayNone;
  };

  struct ParaStyle {
    uint16_t sizePct;
    uint8_t baseFlags;
    TextAlign align;
    int16_t indentPx;
    int16_t textIndentPct;
    uint16_t spaceBeforePct;
    uint16_t spaceAfterPct;
  };

  // --- style stack ----------------------------------------------------------

  void pushState(const CssDecl& decl) {
    const ElemState& parent = stack_[stackTop_];
    ElemState next = parent;
    if (decl.sizePct != 0) {
      const uint32_t scaled = static_cast<uint32_t>(parent.sizePct) * decl.sizePct / 100;
      next.sizePct = static_cast<uint16_t>(scaled < 25 ? 25 : (scaled > 400 ? 400 : scaled));
    }
    if (decl.weightBold == 1) next.flags |= StyleBold;
    if (decl.weightBold == 0) next.flags &= static_cast<uint8_t>(~StyleBold);
    if (decl.styleItalic == 1) next.flags |= StyleItalic;
    if (decl.styleItalic == 0) next.flags &= static_cast<uint8_t>(~StyleItalic);
    if (decl.align != TextAlign::Inherit) next.align = decl.align;
    if (decl.textIndentPct >= 0) next.textIndentPct = decl.textIndentPct;
    if (decl.displayNone == 1) next.displayNone = true;
    if (stackTop_ + 1 < kMaxElemDepth) {
      stack_[++stackTop_] = next;
    } else {
      ++stackOverflow_;  // deeper levels inherit the top unchanged
    }
  }

  void popState() {
    if (stackOverflow_ > 0) {
      --stackOverflow_;
      return;
    }
    if (stackTop_ > 0) --stackTop_;
  }

  uint8_t currentFlags() const { return stack_[stackTop_].flags; }
  uint16_t currentSizePct() const { return stack_[stackTop_].sizePct; }

  // --- paragraph accumulation ------------------------------------------------

  void noteStyleChange() {
    if (spanCount_ == 0) return;  // paragraph text not started yet
    const uint8_t flags = currentFlags();
    const uint16_t sizePct = currentSizePct();
    Span& last = spans_[spanCount_ - 1];
    if (last.flags == flags && last.sizePct == sizePct) return;
    if (last.start == parLen_) {
      last.flags = flags;
      last.sizePct = sizePct;
    } else if (spanCount_ < kMaxSpans) {
      spans_[spanCount_++] = {static_cast<uint16_t>(parLen_), flags, sizePct};
    }
  }

  void appendRaw(char c) {
    if (parLen_ + 1 >= kParTextCap) {
      // Pathological paragraph: flush what we have and continue seamlessly.
      const ParaStyle style = para_;
      flushParagraph();
      para_ = style;
      para_.spaceBeforePct = 0;
      continued_ = true;  // continuation lines get no first-line indent
    }
    if (spanCount_ == 0) {
      spans_[spanCount_++] = {0, currentFlags(), currentSizePct()};
    }
    parText_[parLen_++] = c;
  }

  void latchParagraph(const char* local, const CssDecl& decl) {
    const ElemState& state = stack_[stackTop_];
    para_.sizePct = state.sizePct;
    para_.baseFlags = state.flags;
    para_.align = state.align;
    para_.indentPx = blockIndentFor(local, params_.baseSizePx);
    para_.textIndentPct = state.textIndentPct;
    para_.spaceBeforePct = decl.marginTopPct >= 0 ? static_cast<uint16_t>(decl.marginTopPct) : 0;
    para_.spaceAfterPct =
        decl.marginBottomPct >= 0 ? static_cast<uint16_t>(decl.marginBottomPct) : 0;
    parLen_ = 0;
    spanCount_ = 0;
    pendingSpace_ = false;
  }

  void latchParagraphFromStack() {
    const ElemState& state = stack_[stackTop_];
    para_.sizePct = state.sizePct;
    para_.baseFlags = state.flags;
    para_.align = state.align;
    para_.indentPx = 0;
    para_.textIndentPct = 0;
    para_.spaceBeforePct = 0;
    para_.spaceAfterPct = 0;
    parLen_ = 0;
    spanCount_ = 0;
    pendingSpace_ = false;
  }

  uint16_t paragraphSizePx() const {
    const uint32_t size = static_cast<uint32_t>(params_.baseSizePx) * para_.sizePct / 100;
    return static_cast<uint16_t>(size < 8 ? 8 : size);
  }

  uint16_t spanSizePx(const Span& span) const {
    const uint32_t size = static_cast<uint32_t>(params_.baseSizePx) * span.sizePct / 100;
    return static_cast<uint16_t>(size < 8 ? 8 : size);
  }

  const Span& spanAt(uint32_t byteOffset) const {
    uint16_t found = 0;
    for (uint16_t s = 0; s < spanCount_ && spans_[s].start <= byteOffset; ++s) found = s;
    return spans_[found];
  }

  void advanceY(int16_t dy) {
    if (pageY_ > params_.marginTop) pageY_ += dy;  // no leading gaps at page top
  }

  // --- images ----------------------------------------------------------------

  void placeImage(const char* src) {
    const size_t marked = scratch_.mark();
    const char* resolved = resolveHref(scratch_, chapterDir_, src, nullptr);
    const ZipEntry* entry = resolved != nullptr ? zip_.find(resolved) : nullptr;
    ImageInfo info;
    if (entry != nullptr) probeImage(source_, *entry, scratch_, &info);
    if (entry == nullptr || info.kind == ImageInfo::Kind::Unknown || info.width == 0 ||
        info.height == 0) {
      scratch_.release(marked);  // unknown format or missing target — skip
      return;
    }

    // Fit within the content box, preserving aspect, never upscaling.
    const int32_t contentW = params_.pageWidth - params_.marginLeft - params_.marginRight;
    const int32_t contentH = params_.pageHeight - params_.marginTop - params_.marginBottom;
    int32_t w = info.width;
    int32_t h = info.height;
    if (w > contentW) {
      h = h * contentW / w;
      w = contentW;
    }
    if (h > contentH) {
      w = w * contentH / h;
      h = contentH;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    const bool hasContent = runCount_ > 0 || imageCount_ > 0 || pageY_ > params_.marginTop;
    if ((pageY_ + h > params_.pageHeight - params_.marginBottom || imageCount_ >= kMaxImagesPerPage) &&
        hasContent) {
      emitPage();
      if (stopParse) {
        scratch_.release(marked);
        return;
      }
    }

    const char* hrefCopy = pageArena_.strdup(resolved);
    scratch_.release(marked);
    if (hrefCopy == nullptr || imageCount_ >= kMaxImagesPerPage) {
      failed_ = hrefCopy == nullptr;
      return;
    }
    PageImage& img = images_[imageCount_++];
    img.href = hrefCopy;
    img.width = static_cast<uint16_t>(w);
    img.height = static_cast<uint16_t>(h);
    img.x = static_cast<int16_t>(params_.marginLeft + (contentW - w) / 2);
    img.y = pageY_;
    if (runCount_ == 0 && imageCount_ == 1) pageCharStart_ = parCharBase_;
    pageY_ = static_cast<int16_t>(pageY_ + h + params_.baseSizePx / 2);
  }

  // --- measure phase ----------------------------------------------------------

  int32_t advanceFor(uint32_t cp, uint32_t prevCp, const Span& span) const {
    if (cp == '\n') return 0;
    const uint16_t sizePx = spanSizePx(span);
    int32_t adv = params_.font->advance(cp, sizePx, span.flags);
    if (prevCp != 0) {
      adv += params_.font->kerning(prevCp, cp, sizePx, span.flags);
      if (crossesScripts(prevCp, cp)) adv += sizePx / 4;
    }
    return adv;
  }

  // Width of parText_[from..to) with kerning, resetting kerning at `from`.
  int32_t measureRange(uint32_t from, uint32_t to) const {
    int32_t width = 0;
    uint32_t prev = 0;
    uint32_t i = from;
    while (i < to) {
      const uint32_t charStart = i;
      const uint32_t cp = decodeUtf8(parText_, to, i);
      width += advanceFor(cp, prev, spanAt(charStart));
      prev = cp;
    }
    return width;
  }

  // Attempts to hyphenate the word starting at `wordStart` so that a prefix
  // (plus a hyphen) fits in `budget` measured from `lineStart`. Returns the
  // byte split position, or 0 if hyphenation does not help.
  uint32_t tryHyphenBreak(uint32_t lineStart, uint32_t wordStart, int32_t budget) {
    if (params_.hyphenator == nullptr || !params_.hyphenator->ready()) return 0;
    uint32_t wordEnd = wordStart;
    while (wordEnd < parLen_ && isAsciiLetter(parText_[wordEnd])) ++wordEnd;
    const uint32_t wordLen = wordEnd - wordStart;
    if (wordLen < 5) return 0;

    uint8_t positions[16];
    const uint8_t n = params_.hyphenator->breakPositions(parText_ + wordStart, wordLen,
                                                         positions, sizeof(positions));
    if (n == 0) return 0;

    const int32_t prefixWidth = measureRange(lineStart, wordStart);
    const Span& span = spanAt(wordStart);
    const int32_t hyphenWidth =
        params_.font->advance('-', spanSizePx(span), span.flags);
    for (int i = n - 1; i >= 0; --i) {
      const uint32_t split = wordStart + positions[i];
      if (split <= lineStart) continue;
      const int32_t width = prefixWidth + measureRange(wordStart, split) + hyphenWidth;
      if (width <= budget) return split;
    }
    return 0;
  }

  void measureParagraphLines() {
    lineCount_ = 0;
    set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(parText_), parLen_, params_.language,
                        breaks_);

    const int32_t baseMaxWidth =
        params_.pageWidth - params_.marginLeft - params_.marginRight - para_.indentPx;
    const uint16_t sizePx = paragraphSizePx();
    const int32_t textIndentPx =
        static_cast<int32_t>(sizePx) * (para_.textIndentPct > 0 ? para_.textIndentPct : 0) / 100;

    uint32_t lineStart = 0;
    uint32_t i = 0;
    int32_t lineWidth = 0;
    uint32_t prevCp = 0;
    uint32_t lastBreakEnd = 0;
    uint32_t wordStart = 0;

    while (i < parLen_ && lineCount_ + 1 < kMaxLinesPerPar) {
      const int32_t maxWidth = baseMaxWidth - (lineCount_ == 0 && !continued_ ? textIndentPx : 0);
      const uint32_t charStart = i;
      const uint32_t cp = decodeUtf8(parText_, parLen_, i);
      const int32_t adv = advanceFor(cp, prevCp, spanAt(charStart));

      if (cp != '\n' && lineWidth + adv > maxWidth && charStart > lineStart) {
        // Try a hyphen inside the overflowing word first; it beats breaking
        // at the previous space when it fits meaningfully more text.
        uint32_t breakEnd = 0;
        uint8_t flags = 0;
        const uint32_t hyphenAt =
            tryHyphenBreak(lineStart, wordStart > lineStart ? wordStart : lineStart, maxWidth);
        if (hyphenAt > lineStart &&
            (lastBreakEnd <= lineStart || hyphenAt > lastBreakEnd)) {
          breakEnd = hyphenAt;
          flags = kLineHyphen;
        } else if (lastBreakEnd > lineStart) {
          breakEnd = lastBreakEnd;
        } else {
          breakEnd = charStart;  // force mid-word, no opportunity at all
        }
        recordLine(lineStart, breakEnd, flags);
        lineStart = breakEnd;
        while (lineStart < parLen_ && parText_[lineStart] == ' ') ++lineStart;
        i = lineStart;
        lineWidth = 0;
        prevCp = 0;
        lastBreakEnd = 0;
        wordStart = lineStart;
        continue;
      }
      lineWidth += adv;
      prevCp = cp;

      const char brk = breaks_[i - 1];  // opportunity after this character
      if (brk == LINEBREAK_MUSTBREAK) {
        recordLine(lineStart, i, kLineLast);
        lineStart = i;
        lineWidth = 0;
        prevCp = 0;
        lastBreakEnd = 0;
        wordStart = i;
      } else if (brk == LINEBREAK_ALLOWBREAK) {
        lastBreakEnd = i;
        wordStart = i;
        while (wordStart < parLen_ && parText_[wordStart] == ' ') ++wordStart;
      }
    }
    if (lineStart < parLen_ && lineCount_ < kMaxLinesPerPar) {
      recordLine(lineStart, parLen_, kLineLast);
    }
    if (lineCount_ > 0) lines_[lineCount_ - 1].flags |= kLineLast;
  }

  void recordLine(uint32_t start, uint32_t end, uint8_t flags) {
    while (end > start && (parText_[end - 1] == ' ' || parText_[end - 1] == '\n')) --end;
    LineRec& rec = lines_[lineCount_++];
    rec.start = start;
    rec.end = end;
    rec.flags = flags;
    rec.spaceCount = 0;
    rec.cjkGaps = 0;
    uint32_t prev = 0;
    uint32_t i = start;
    while (i < end) {
      const uint32_t cp = decodeUtf8(parText_, end, i);
      if (cp == ' ') {
        ++rec.spaceCount;
      } else if (prev != 0 && isCjk(prev) && isCjk(cp)) {
        ++rec.cjkGaps;
      }
      prev = cp;
    }
    rec.naturalWidth = measureRange(start, end);
    if (flags & kLineHyphen) {
      const Span& span = spanAt(end > start ? end - 1 : start);
      rec.naturalWidth += params_.font->advance('-', spanSizePx(span), span.flags);
    }
  }

  // --- place phase -------------------------------------------------------------

  void flushParagraph() {
    if (parLen_ == 0) {
      continued_ = false;
      return;
    }
    const uint16_t sizePx = paragraphSizePx();
    const int16_t lineHeight = params_.font->lineHeight(sizePx);
    advanceY(static_cast<int16_t>(static_cast<int32_t>(sizePx) * para_.spaceBeforePct / 100));

    measureParagraphLines();

    uint32_t idx = 0;
    while (idx < lineCount_ && !failed_ && !stopParse) {
      const int32_t availPx = (params_.pageHeight - params_.marginBottom) - pageY_;
      uint32_t avail = availPx > 0 ? static_cast<uint32_t>(availPx / lineHeight) : 0;
      const uint32_t remaining = lineCount_ - idx;
      const bool hasContent = runCount_ > 0 || pageY_ > params_.marginTop;

      if (avail == 0) {
        if (!hasContent) break;  // page too short for even one line — give up
        emitPage();
        continue;
      }
      uint32_t take = avail < remaining ? avail : remaining;

      // Widow control first: never carry a runt to the next page, shrinking
      // this page's share if needed. Then orphan control on the result: if
      // the shrunken share would leave a runt at this page's bottom, push the
      // whole paragraph to the next page instead. (A 3-line paragraph with
      // 2 lines of room goes 0+3, not 2+1 or 1+2.)
      while (take < remaining && remaining - take < params_.widowLines && take > 1) --take;
      if (idx == 0 && take < remaining && take < params_.orphanLines && hasContent) {
        emitPage();
        continue;
      }

      for (uint32_t l = 0; l < take && !failed_ && !stopParse; ++l) {
        placeLine(lines_[idx + l], sizePx, lineHeight, idx + l == 0);
      }
      idx += take;
      if (idx < lineCount_ && !stopParse) emitPage();
    }

    advanceY(static_cast<int16_t>(static_cast<int32_t>(sizePx) * para_.spaceAfterPct / 100));
    parCharBase_ += countChars(parText_, parLen_);
    parLen_ = 0;
    spanCount_ = 0;
    pendingSpace_ = false;
    continued_ = false;
  }

  void placeLine(const LineRec& rec, uint16_t sizePx, int16_t lineHeight, bool firstLine) {
    if (rec.end == rec.start) {  // blank line (e.g. double <br/>)
      pageY_ += lineHeight;
      return;
    }
    if (runCount_ == 0) {
      pageCharStart_ = parCharBase_ + countChars(parText_, rec.start);
    }

    const int32_t baseMaxWidth =
        params_.pageWidth - params_.marginLeft - params_.marginRight - para_.indentPx;
    const int32_t textIndentPx =
        firstLine && !continued_ && para_.textIndentPct > 0
            ? static_cast<int32_t>(sizePx) * para_.textIndentPct / 100
            : 0;
    const int32_t maxWidth = baseMaxWidth - textIndentPx;
    const int32_t leftover = maxWidth - rec.naturalWidth;

    int32_t x = params_.marginLeft + para_.indentPx + textIndentPx;
    const uint32_t gaps = static_cast<uint32_t>(rec.spaceCount) + rec.cjkGaps;
    const bool justify = para_.align == TextAlign::Justify && !(rec.flags & kLineLast) &&
                         gaps > 0 && leftover > 0;
    if (para_.align == TextAlign::Right) {
      x += leftover;
    } else if (para_.align == TextAlign::Center) {
      x += leftover / 2;
    }
    const int32_t perGap = justify ? leftover / static_cast<int32_t>(gaps) : 0;
    int32_t gapRemainder = justify ? leftover % static_cast<int32_t>(gaps) : 0;

    int16_t baselineY = static_cast<int16_t>(pageY_ + params_.font->ascent(sizePx));

    // Slice into runs. Segments close at style-span changes, at spaces and
    // CJK-CJK boundaries when justifying (both become adjustable gaps —
    // spaces for Latin text, inter-character expansion for CJK), and at
    // script boundaries (which get their quarter-em of air explicitly).
    uint32_t segStart = rec.start;
    int32_t segWidth = 0;
    uint32_t segPrev = 0;
    const Span* segSpan = &spanAt(rec.start);
    uint32_t linePrev = 0;
    uint32_t i = rec.start;

    auto flushSeg = [&](uint32_t segEnd, bool addHyphen) -> bool {
      const uint32_t segLen = segEnd - segStart;
      if (segLen == 0 && !addHyphen) return true;
      const uint32_t copyLen = segLen + (addHyphen ? 1u : 0u);
      if (runCount_ >= kMaxRunsPerPage) {
        emitPage();
        if (stopParse) return false;
        baselineY = static_cast<int16_t>(pageY_ + params_.font->ascent(sizePx));
      }
      char* copy = static_cast<char*>(pageArena_.alloc(copyLen, 1));
      if (copy == nullptr) {
        emitPage();  // page text arena full — flush and continue on a fresh page
        if (stopParse) return false;
        baselineY = static_cast<int16_t>(pageY_ + params_.font->ascent(sizePx));
        copy = static_cast<char*>(pageArena_.alloc(copyLen, 1));
        if (copy == nullptr) {
          failed_ = true;
          return false;
        }
      }
      memcpy(copy, parText_ + segStart, segLen);
      if (addHyphen) {
        copy[segLen] = '-';
        segWidth += params_.font->advance('-', spanSizePx(*segSpan), segSpan->flags);
      }
      runs_[runCount_++] = {copy,
                            static_cast<uint16_t>(copyLen),
                            static_cast<int16_t>(x),
                            baselineY,
                            spanSizePx(*segSpan),
                            segSpan->flags};
      x += segWidth;
      segWidth = 0;
      segPrev = 0;
      return true;
    };
    auto gapBump = [&]() {
      if (!justify) return;
      x += perGap;
      if (gapRemainder > 0) {
        ++x;
        --gapRemainder;
      }
    };

    while (i < rec.end && !failed_) {
      const uint32_t charStart = i;
      const Span* span = &spanAt(charStart);
      uint32_t next = charStart;
      const uint32_t cp = decodeUtf8(parText_, rec.end, next);

      if (span != segSpan) {
        if (!flushSeg(charStart, false)) return;
        segStart = charStart;
        segSpan = span;
      }
      if (justify && cp == ' ') {
        if (!flushSeg(charStart, false)) return;
        x += params_.font->advance(' ', spanSizePx(*span), span->flags);
        gapBump();
        segStart = next;
        linePrev = cp;
        i = next;
        continue;
      }
      if (linePrev != 0 && justify && isCjk(linePrev) && isCjk(cp)) {
        if (!flushSeg(charStart, false)) return;
        gapBump();
        segStart = charStart;
      } else if (linePrev != 0 && crossesScripts(linePrev, cp)) {
        if (!flushSeg(charStart, false)) return;
        x += spanSizePx(*span) / 4;
        segStart = charStart;
      }
      segWidth += advanceFor(cp, segPrev, *span);
      segPrev = cp;
      linePrev = cp;
      i = next;
    }
    if (!failed_) flushSeg(rec.end, (rec.flags & kLineHyphen) != 0);
    pageY_ += lineHeight;
  }

  void emitPage() {
    Page page{runs_, runCount_, images_, imageCount_, pageCount_, pageCharStart_};
    ++pageCount_;
    if (!sink_.onPage(page)) stopParse = true;
    runCount_ = 0;
    imageCount_ = 0;
    pageArena_.reset();
    pageY_ = params_.marginTop;
  }

  BookSource& source_;
  const ZipCatalog& zip_;
  const LayoutParams& params_;
  Arena& scratch_;
  PageSink& sink_;
  char chapterDir_[512];

  char* parText_ = nullptr;
  char* breaks_ = nullptr;
  Span* spans_ = nullptr;
  PageTextRun* runs_ = nullptr;
  PageImage* images_ = nullptr;
  LineRec* lines_ = nullptr;
  Arena pageArena_;

  uint32_t parLen_ = 0;
  uint16_t spanCount_ = 0;
  uint32_t lineCount_ = 0;
  ParaStyle para_{100, StyleNone, TextAlign::Left, 0, 0, 0, 40};
  bool pendingSpace_ = false;
  bool continued_ = false;

  ElemState stack_[kMaxElemDepth];
  uint8_t stackTop_ = 0;
  int stackOverflow_ = 0;
  int depth_ = 0;
  int suppress_ = 0;
  bool inBody_ = false;

  uint16_t runCount_ = 0;
  uint16_t imageCount_ = 0;
  int16_t pageY_ = 0;
  uint32_t pageCount_ = 0;
  uint32_t parCharBase_ = 0;    // chapter chars before the current paragraph
  uint32_t pageCharStart_ = 0;  // anchor of the page being assembled
  bool failed_ = false;
};

}  // namespace

BookStatus ChapterLayout::layout(BookSource& source, const ZipCatalog& zip,
                                 const ZipEntry& entry, const char* chapterHref,
                                 const LayoutParams& params, Arena& scratch, PageSink& sink,
                                 uint32_t* pageCountOut) {
  if (params.font == nullptr || params.pageWidth <= 0 || params.pageHeight <= 0) {
    return BookStatus::Unsupported;
  }
  const size_t marked = scratch.mark();
  LayoutEngine engine(source, zip, chapterHref, params, scratch, sink);
  if (!engine.init()) {
    scratch.release(marked);
    return BookStatus::OutOfMemory;
  }
  BookStatus status = XmlSax::parseEntry(source, entry, scratch, engine,
                                         /*filterHtmlEntities=*/true);
  if (status == BookStatus::Ok && !engine.finish()) status = BookStatus::OutOfMemory;
  if (status == BookStatus::Ok && engine.outOfMemory()) status = BookStatus::OutOfMemory;
  if (pageCountOut != nullptr) *pageCountOut = engine.pageCount();
  scratch.release(marked);
  return status;
}

}  // namespace book
}  // namespace freeink
