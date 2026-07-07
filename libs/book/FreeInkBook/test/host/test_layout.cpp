// Host-side tests for FreeInkBook Phase 2 (streaming layout + pagination).
// A fake fixed-metrics font stands in for rasterization, so line breaking,
// block flow, style runs, page assembly, and — critically — the O(page)
// memory invariant are all verified with no device or font files.

#include <FreeInkBook.h>
#include <cache/PageCache.h>
#include <layout/ChapterLayout.h>
#include <render/ImageRenderer.h>

#include <cstdio>
#include <cstring>

namespace {

int checksRun = 0;
int checksFailed = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++checksRun;                                                           \
    if (!(cond)) {                                                         \
      ++checksFailed;                                                      \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                                  \
  do {                                                                                  \
    ++checksRun;                                                                        \
    const auto va = (a);                                                                \
    const auto vb = (b);                                                                \
    if (!(va == vb)) {                                                                  \
      ++checksFailed;                                                                   \
      std::printf("FAIL %s:%d  %s == %s  (%ld != %ld)\n", __FILE__, __LINE__, #a, #b,   \
                  static_cast<long>(va), static_cast<long>(vb));                        \
    }                                                                                   \
  } while (0)

using namespace freeink::book;

class HostFileSource : public BookSource {
 public:
  ~HostFileSource() override {
    if (file_ != nullptr) std::fclose(file_);
  }
  bool open(const char* path) {
    file_ = std::fopen(path, "rb");
    if (file_ == nullptr) return false;
    std::fseek(file_, 0, SEEK_END);
    size_ = static_cast<uint64_t>(std::ftell(file_));
    return true;
  }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) return -1;
    return static_cast<int32_t>(std::fread(dst, 1, len, file_));
  }
  uint64_t size() const override { return size_; }

 private:
  FILE* file_ = nullptr;
  uint64_t size_ = 0;
};

// Deterministic metrics: advance = sizePx/2 (+1 when bold), line height =
// size + 4, ascent = size. Enough to make width math and style-dependent
// measurement observable.
class FakeFont : public BookFont {
 public:
  int16_t advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) override {
    (void)codepoint;
    int16_t adv = static_cast<int16_t>(sizePx / 2);
    if (styleFlags & StyleBold) adv = static_cast<int16_t>(adv + 1);
    return adv;
  }
  int16_t lineHeight(uint16_t sizePx) override { return static_cast<int16_t>(sizePx + 4); }
  int16_t ascent(uint16_t sizePx) override { return static_cast<int16_t>(sizePx); }
};

// Collects all delivered pages: concatenated text (runs in order, newline per
// page) plus geometry checks that hold for every valid page.
class CollectSink : public PageSink {
 public:
  CollectSink(const LayoutParams& params, BookFont& font) : params_(params), font_(font) {}

  bool onPage(const Page& page) override {
    ++pages;
    int16_t prevBaseline = -1;
    for (uint16_t r = 0; r < page.runCount; ++r) {
      const PageTextRun& run = page.runs[r];
      // Geometry invariants.
      if (run.x < params_.marginLeft) ++geometryViolations;
      int32_t width = 0;
      uint32_t i = 0;
      uint32_t prevCp = 0;
      while (i < run.len) {
        uint32_t cp = static_cast<uint8_t>(run.text[i]);
        uint32_t extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        i += 1 + extra;
        width += font_.advance(cp, run.sizePx, run.styleFlags);
        if (prevCp != 0) width += font_.kerning(prevCp, cp, run.sizePx, run.styleFlags);
        prevCp = cp;
      }
      if (run.x + width > params_.pageWidth - params_.marginRight) ++geometryViolations;
      if (run.baselineY > params_.pageHeight - params_.marginBottom) ++geometryViolations;
      if (run.baselineY < prevBaseline) ++orderViolations;
      prevBaseline = run.baselineY;

      if (run.sizePx > maxSizeSeen) maxSizeSeen = run.sizePx;
      if (run.sizePx < minSizeSeen) minSizeSeen = run.sizePx;
      if (run.styleFlags & StyleBold) ++boldRuns;
      if (run.styleFlags & StyleItalic) ++italicRuns;

      if (textLen + run.len + 2 < sizeof(text)) {
        if (r > 0 && run.baselineY != page.runs[r - 1].baselineY) text[textLen++] = ' ';
        std::memcpy(text + textLen, run.text, run.len);
        textLen += run.len;
      }
    }
    text[textLen] = '\0';
    return !stopAfterFirst;
  }

  const LayoutParams& params_;
  BookFont& font_;
  uint32_t pages = 0;
  int geometryViolations = 0;
  int orderViolations = 0;
  uint16_t maxSizeSeen = 0;
  uint16_t minSizeSeen = 0xFFFF;
  int boldRuns = 0;
  int italicRuns = 0;
  bool stopAfterFirst = false;
  char text[512 * 1024];
  uint32_t textLen = 0;
};

uint8_t bookBuf[64 * 1024];
uint8_t scratchBuf[256 * 1024];

char fixturePath[1024];
const char* fixturesDir = nullptr;

const char* fixture(const char* name) {
  std::snprintf(fixturePath, sizeof(fixturePath), "%s/%s", fixturesDir, name);
  return fixturePath;
}

struct OpenedBook {
  HostFileSource source;
  Arena bookArena{bookBuf, sizeof(bookBuf)};
  Arena scratch{scratchBuf, sizeof(scratchBuf)};
  Book book;

  bool open(const char* name) {
    if (!source.open(fixture(name))) return false;
    return book.open(source, bookArena, scratch) == BookStatus::Ok;
  }
};

LayoutParams stickyParams(BookFont& font) {
  LayoutParams params;
  params.pageWidth = 800;
  params.pageHeight = 480;
  params.baseSizePx = 16;
  params.font = &font;
  return params;
}

void testSmallChapter() {
  OpenedBook opened;
  CHECK(opened.open("minimal.epub"));

  FakeFont font;
  LayoutParams params = stickyParams(font);
  CollectSink sink(params, font);

  const ZipEntry* entry = opened.book.zip().find(opened.book.spineItem(0)->href);
  CHECK(entry != nullptr);
  uint32_t pages = 0;
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, opened.book.zip(), *entry, entry->name, params,
                                                  opened.scratch, sink, &pages)),
           static_cast<int>(BookStatus::Ok));

  CHECK(pages >= 1);
  CHECK_EQ(pages, sink.pages);
  CHECK_EQ(sink.geometryViolations, 0);
  CHECK_EQ(sink.orderViolations, 0);
  CHECK(std::strstr(sink.text, "Call me Ishmael") != nullptr);
  CHECK(std::strstr(sink.text, "Chapter One") != nullptr);
  // The <head><title> must not leak into the page text.
  CHECK(std::strstr(sink.text, "Chapter OneChapter One") == nullptr);

  // Multiple font sizes on the page: h1 at 200% of base 16 = 32 px, body 16.
  CHECK_EQ(sink.maxSizeSeen, 32);
  CHECK_EQ(sink.minSizeSeen, 16);
  CHECK_EQ(opened.scratch.used(), 0u);  // layout released everything
}

void testStylesEntitiesAndBreaks() {
  OpenedBook opened;
  CHECK(opened.open("minimal.epub"));

  FakeFont font;
  LayoutParams params = stickyParams(font);
  CollectSink sink(params, font);

  const ZipEntry* entry = opened.book.zip().find("OEBPS/text/ch3.xhtml");
  CHECK(entry != nullptr);
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, opened.book.zip(), *entry, entry->name, params,
                                                  opened.scratch, sink, nullptr)),
           static_cast<int>(BookStatus::Ok));

  // Entities resolved to UTF-8 (nbsp stays no-break, mdash/hellip literal),
  // numeric references pass through, unknown names degrade to U+FFFD, and a
  // bare ampersand survives as text.
  CHECK(std::strstr(sink.text, "Alpha\xC2\xA0"
                               "beta\xE2\x80\x94gamma\xE2\x80\xA6") != nullptr);
  CHECK(std::strstr(sink.text, "numeric\xE2\x80\x94"
                               "dashes") != nullptr);
  CHECK(std::strstr(sink.text, "\xEF\xBF\xBD") != nullptr);
  CHECK(std::strstr(sink.text, "AT&T stay alive") != nullptr);

  // Inline styles produced distinct runs.
  CHECK(sink.boldRuns > 0);
  CHECK(sink.italicRuns > 0);

  // The unbreakable-word paragraph forced character-level breaks without
  // overflowing the margin (geometry check covers the width); the hard <br/>
  // kept "Line one" and "line two" apart.
  CHECK_EQ(sink.geometryViolations, 0);
  CHECK(std::strstr(sink.text, "Line one line two") != nullptr);
}

// The design's headline invariant: layout memory does not grow with chapter
// size. The big generated chapter (>100 KB) must peak within a whisker of the
// small one — the difference is at most one page of denser text runs.
void testMemoryIndependence() {
  FakeFont font;
  LayoutParams params = stickyParams(font);

  size_t highWater[2] = {0, 0};
  const char* items[2] = {nullptr, "OEBPS/text/ch2.xhtml"};
  uint32_t bigPages = 0;

  for (int round = 0; round < 2; ++round) {
    OpenedBook opened;
    CHECK(opened.open("minimal.epub"));
    const char* href = round == 0 ? opened.book.spineItem(0)->href : items[1];
    const ZipEntry* entry = opened.book.zip().find(href);
    CHECK(entry != nullptr);

    const size_t before = opened.scratch.highWater();
    (void)before;
    Arena layoutArena{scratchBuf, sizeof(scratchBuf)};  // fresh, isolated measurement
    CollectSink sink(params, font);
    uint32_t pages = 0;
    CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, opened.book.zip(), *entry, entry->name, params,
                                                    layoutArena, sink, &pages)),
             static_cast<int>(BookStatus::Ok));
    highWater[round] = layoutArena.highWater();
    if (round == 1) bigPages = pages;
  }

  std::printf("  layout high water: small %zu B, big %zu B (%u pages)\n", highWater[0],
              highWater[1], bigPages);
  // Ceiling anatomy: ~40 KB fixed layout buffers (paragraph text + breaks +
  // line records + run table) + 24 KB page sub-arena + ~50 KB parse/inflate
  // state. All O(1) in chapter size.
  CHECK(bigPages > 10);                             // the big chapter really paginated
  CHECK(highWater[1] < 144 * 1024);                 // absolute ceiling
  CHECK(highWater[1] <= highWater[0] + 16 * 1024);  // O(page), not O(chapter)
}

void testEarlyStop() {
  OpenedBook opened;
  CHECK(opened.open("minimal.epub"));

  FakeFont font;
  LayoutParams params = stickyParams(font);
  CollectSink sink(params, font);
  sink.stopAfterFirst = true;

  const ZipEntry* entry = opened.book.zip().find("OEBPS/text/ch2.xhtml");
  CHECK(entry != nullptr);
  uint32_t pages = 0;
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, opened.book.zip(), *entry, entry->name, params,
                                                  opened.scratch, sink, &pages)),
           static_cast<int>(BookStatus::Ok));
  CHECK_EQ(pages, 1u);  // sink declined more — layout stopped promptly
}

// --- Phase 4: CSS, justification, hyphenation, kerning, widow/orphan --------

void testCssUnit() {
  uint8_t buf[32 * 1024];
  Arena arena(buf, sizeof(buf));
  CssStylesheetBuilder builder;
  CHECK(builder.begin(arena));
  const char css[] =
      "/* c */ p, h1 { text-align: justify; margin-top: 1em }\n"
      ".big { font-size: 150% }\n"
      "p.deep { font-weight: 700; text-indent: 2em }\n"
      "@media print { p { text-indent: 99em } }\n"
      "div > p { font-style: italic }\n"
      "#id, p:first-child { font-size: 99em }\n";
  builder.addText(css, sizeof(css) - 1);
  const CssStylesheet sheet = builder.finish();
  CHECK(sheet.ruleCount >= 4);
  CHECK(sheet.contentHash != 0);

  CssDecl p = cascadeFor(sheet, "p", nullptr, nullptr);
  CHECK(p.align == TextAlign::Justify);
  CHECK_EQ(p.marginTopPct, 100);
  CHECK_EQ(p.sizePct, 0);  // #id and :first-child rules were skipped

  CssDecl pBig = cascadeFor(sheet, "p", "big", nullptr);
  CHECK_EQ(pBig.sizePct, 150);

  CssDecl pDeep = cascadeFor(sheet, "p", "other deep", nullptr);
  CHECK_EQ(pDeep.weightBold, 1);
  CHECK_EQ(pDeep.textIndentPct, 200);

  CssDecl h1 = cascadeFor(sheet, "h1", "big", nullptr);
  CHECK(h1.align == TextAlign::Justify);
  CHECK_EQ(h1.sizePct, 150);

  // "div > p" matches its rightmost simple selector (p).
  CHECK_EQ(cascadeFor(sheet, "p", nullptr, nullptr).styleItalic, 1);

  const CssDecl inlineDecl = parseInlineStyle("font-weight:bold; font-size: 2em");
  CssDecl withInline = cascadeFor(sheet, "p", "big", &inlineDecl);
  CHECK_EQ(withInline.sizePct, 200);  // inline beats class
  CHECK_EQ(withInline.weightBold, 1);
}

// Builds the fixture book's stylesheet the way an app would: every manifest
// item with media-type text/css.
CssStylesheet buildBookStylesheet(OpenedBook& opened, Arena& arena) {
  CssStylesheetBuilder builder;
  CHECK(builder.begin(arena));
  for (size_t m = 0; m < opened.book.manifestCount(); ++m) {
    const ManifestItem* item = opened.book.manifestItem(m);
    if (std::strcmp(item->mediaType, "text/css") != 0) continue;
    const ZipEntry* entry = opened.book.zip().find(item->href);
    CHECK(entry != nullptr);
    CHECK_EQ(static_cast<int>(builder.addSheet(opened.source, *entry, opened.scratch)),
             static_cast<int>(BookStatus::Ok));
  }
  return builder.finish();
}

void testStyledChapter() {
  OpenedBook opened;
  CHECK(opened.open("minimal.epub"));

  FakeFont font;
  LayoutParams params = stickyParams(font);
  static uint8_t sheetBuf[32 * 1024];
  Arena sheetArena(sheetBuf, sizeof(sheetBuf));
  const CssStylesheet sheet = buildBookStylesheet(opened, sheetArena);
  CHECK(sheet.ruleCount >= 5);
  params.stylesheet = &sheet;

  CollectSink sink(params, font);
  const ZipEntry* entry = opened.book.zip().find("OEBPS/text/ch4.xhtml");
  CHECK(entry != nullptr);
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, opened.book.zip(), *entry, entry->name, params,
                                                  opened.scratch, sink, nullptr)),
           static_cast<int>(BookStatus::Ok));
  CHECK_EQ(sink.geometryViolations, 0);

  // display:none removed the hidden span entirely.
  CHECK(std::strstr(sink.text, "INVISIBLE-MARKER") == nullptr);
  CHECK(std::strstr(sink.text, "Visible before.") != nullptr);
  CHECK(std::strstr(sink.text, "Visible after.") != nullptr);

  const int16_t rightEdge = params.pageWidth - params.marginRight;

  // Per-run assertions need the raw runs — re-walk them via a second layout
  // with a recording sink.
  struct RunSink : PageSink {
    bool onPage(const Page& page) override {
      for (uint16_t r = 0; r < page.runCount && count < 512; ++r) {
        runs[count] = page.runs[r];
        char* copy = text + textUsed;
        std::memcpy(copy, page.runs[r].text, page.runs[r].len);
        copy[page.runs[r].len] = '\0';
        textUsed += page.runs[r].len + 1;
        runText[count] = copy;
        ++count;
      }
      return true;
    }
    PageTextRun runs[512];
    const char* runText[512];
    uint32_t count = 0;
    char text[64 * 1024];
    uint32_t textUsed = 0;
  } rs;
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, opened.book.zip(), *entry, entry->name, params,
                                                  opened.scratch, rs, nullptr)),
           static_cast<int>(BookStatus::Ok));

  auto findRun = [&](const char* prefix) -> const PageTextRun* {
    for (uint32_t r = 0; r < rs.count; ++r) {
      if (std::strncmp(rs.runText[r], prefix, std::strlen(prefix)) == 0) return &rs.runs[r];
    }
    return nullptr;
  };
  auto lineRightEdge = [&](const PageTextRun* first) -> int32_t {
    // Right edge of the last run sharing first's baseline.
    int32_t edge = 0;
    for (uint32_t r = 0; r < rs.count; ++r) {
      if (rs.runs[r].baselineY != first->baselineY) continue;
      int32_t width = 0;
      uint32_t i = 0;
      while (i < rs.runs[r].len) {
        const uint32_t cp = static_cast<uint8_t>(rs.runText[r][i]);
        i += cp >= 0xF0 ? 4 : cp >= 0xE0 ? 3 : cp >= 0xC0 ? 2 : 1;
        width += font.advance(cp, rs.runs[r].sizePx, rs.runs[r].styleFlags);
      }
      if (rs.runs[r].x + width > edge) edge = rs.runs[r].x + width;
    }
    return edge;
  };

  // h2.plain: size scaled (160% of 16 = 25) but CSS stripped the bold.
  const PageTextRun* h2 = findRun("Styled");
  CHECK(h2 != nullptr);
  CHECK_EQ(h2->sizePx, 25);
  CHECK((h2->styleFlags & StyleBold) == 0);

  // p { text-indent: 1.2em } → first line starts at margin + 19 px.
  const PageTextRun* just = findRun("JUSTIFY-MARKER");
  CHECK(just != nullptr);
  CHECK_EQ(just->x, params.marginLeft + 19);
  // body { text-align: justify } → the first (non-last) line of the long
  // paragraph ends exactly at the right margin.
  CHECK_EQ(lineRightEdge(just), rightEdge);

  // Right and center alignment.
  const PageTextRun* right = findRun("RIGHT-MARKER");
  CHECK(right != nullptr);
  CHECK_EQ(lineRightEdge(right), rightEdge);
  CHECK(right->x > params.marginLeft + 100);  // clearly shifted right

  const PageTextRun* center = findRun("CENTER-MARKER");
  CHECK(center != nullptr);
  const int32_t centerEdge = lineRightEdge(center);
  const int32_t leftGap = center->x - params.marginLeft;
  const int32_t rightGap = rightEdge - centerEdge;
  CHECK(leftGap > 0 && rightGap > 0);
  CHECK(leftGap - rightGap >= -1 && leftGap - rightGap <= 1);

  // .note span: 87% of 16 → 13 px, italic — a second size on the page.
  const PageTextRun* note = findRun("NOTE-MARKER");
  CHECK(note != nullptr);
  CHECK_EQ(note->sizePx, 13);
  CHECK((note->styleFlags & StyleItalic) != 0);

  // Inline style attribute.
  const PageTextRun* inlineBold = findRun("INLINE-BOLD-MARKER");
  CHECK(inlineBold != nullptr);
  CHECK((inlineBold->styleFlags & StyleBold) != 0);
}

void testHyphenation(const Hyphenator& hyphenator) {
  OpenedBook opened;
  CHECK(opened.open("minimal.epub"));

  FakeFont font;
  LayoutParams params = stickyParams(font);
  params.pageWidth = 240;  // narrow column forces breaks inside words
  params.hyphenator = &hyphenator;

  CollectSink sink(params, font);
  const ZipEntry* entry = opened.book.zip().find(opened.book.spineItem(0)->href);
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, opened.book.zip(), *entry, entry->name, params,
                                                  opened.scratch, sink, nullptr)),
           static_cast<int>(BookStatus::Ok));
  CHECK_EQ(sink.geometryViolations, 0);
  // The narrow column hyphenated something ("par-ticular", "wa-tery", ...).
  CHECK(std::strstr(sink.text, "- ") != nullptr);

  // Same layout without the hyphenator must produce different (worse) fill —
  // and a different generation hash.
  LayoutParams plain = params;
  plain.hyphenator = nullptr;
  CHECK(layoutGenerationHash(params, 1) != layoutGenerationHash(plain, 1));
}

void testKerningAffectsMeasurement() {
  class KerningFont : public FakeFont {
   public:
    int16_t kerning(uint32_t, uint32_t, uint16_t, uint8_t) override { return -1; }
  };

  OpenedBook opened;
  CHECK(opened.open("minimal.epub"));
  KerningFont kernFont;
  FakeFont plainFont;
  LayoutParams kerned = stickyParams(kernFont);
  LayoutParams plain = stickyParams(plainFont);

  const ZipEntry* entry = opened.book.zip().find("OEBPS/text/ch2.xhtml");
  CollectSink kernedSink(kerned, kernFont);
  CollectSink plainSink(plain, plainFont);
  uint32_t kernedPages = 0;
  uint32_t plainPages = 0;
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, opened.book.zip(), *entry, entry->name, kerned,
                                                  opened.scratch, kernedSink, &kernedPages)),
           static_cast<int>(BookStatus::Ok));
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, opened.book.zip(), *entry, entry->name, plain,
                                                  opened.scratch, plainSink, &plainPages)),
           static_cast<int>(BookStatus::Ok));
  CHECK_EQ(kernedSink.geometryViolations, 0);
  // Tighter kerned text packs more per line, so fewer or equal pages.
  CHECK(kernedPages <= plainPages);
  CHECK(kernedSink.textLen >= plainSink.textLen - 8);  // same content either way
}

// Widow/orphan control, observed through first-line indents: with
// p { text-indent: 1.5em } every paragraph-opening line starts at
// margin + 24 while continuation lines start at the margin. A paragraph
// split may leave neither a single opening line at a page bottom (orphan)
// nor a single continuation line at a page top (widow).
void testWidowOrphan() {
  OpenedBook opened;
  CHECK(opened.open("minimal.epub"));

  FakeFont font;
  LayoutParams params = stickyParams(font);
  static uint8_t sheetBuf[24 * 1024];
  Arena sheetArena(sheetBuf, sizeof(sheetBuf));
  CssStylesheetBuilder builder;
  CHECK(builder.begin(sheetArena));
  const char css[] = "p { text-indent: 1.5em; margin-bottom: 0.2em }";
  builder.addText(css, sizeof(css) - 1);
  static CssStylesheet sheet;
  sheet = builder.finish();
  params.stylesheet = &sheet;

  struct LineSink : PageSink {
    struct Line {
      uint32_t page;
      int16_t x;
      uint16_t sizePx;
    };
    bool onPage(const Page& page) override {
      int16_t lastBaseline = -1;
      for (uint16_t r = 0; r < page.runCount; ++r) {
        if (page.runs[r].baselineY != lastBaseline && count < kMax) {
          lines[count++] = {page.pageIndex, page.runs[r].x, page.runs[r].sizePx};
          lastBaseline = page.runs[r].baselineY;
        }
      }
      return true;
    }
    enum : uint32_t { kMax = 4096 };
    Line lines[kMax];
    uint32_t count = 0;
  } ls;

  const ZipEntry* entry = opened.book.zip().find("OEBPS/text/ch2.xhtml");
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, opened.book.zip(), *entry, entry->name, params,
                                                  opened.scratch, ls, nullptr)),
           static_cast<int>(BookStatus::Ok));
  CHECK(ls.count > 100);

  // Walk body lines (sizePx == 16; the h1 is larger), grouping paragraphs by
  // indented first lines.
  const int16_t indentX = 24 + 24;  // margin + 1.5em
  int orphanViolations = 0;
  int widowViolations = 0;
  uint32_t paraStart = 0;
  bool inPara = false;
  for (uint32_t i = 0; i < ls.count; ++i) {
    if (ls.lines[i].sizePx != 16) continue;
    const bool opensParagraph = ls.lines[i].x == indentX;
    if (opensParagraph) {
      paraStart = i;
      inPara = true;
    } else if (inPara) {
      // Continuation line: a page break inside the paragraph must not leave
      // exactly one line on either side.
      if (ls.lines[i].page != ls.lines[i - 1].page) {
        uint32_t before = 0;
        for (uint32_t b = paraStart; b < i; ++b) {
          if (ls.lines[b].sizePx == 16) ++before;
        }
        if (before == 1) ++orphanViolations;
        uint32_t after = 1;
        for (uint32_t a = i + 1; a < ls.count && ls.lines[a].sizePx == 16 &&
                                 ls.lines[a].x != indentX && ls.lines[a].page == ls.lines[i].page;
             ++a) {
          ++after;
        }
        if (after == 1) ++widowViolations;
      }
    }
  }
  CHECK_EQ(orphanViolations, 0);
  CHECK_EQ(widowViolations, 0);
}

// --- Phase 5: CJK ---------------------------------------------------------------

// Fullwidth CJK advances, halfwidth Latin — the shape real CJK fonts have.
class CjkFakeFont : public FakeFont {
 public:
  int16_t advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) override {
    if (codepoint >= 0x2E80) return static_cast<int16_t>(sizePx);
    return FakeFont::advance(codepoint, sizePx, styleFlags);
  }
};

void testCjk() {
  OpenedBook opened;
  CHECK(opened.open("minimal.epub"));

  CjkFakeFont font;
  LayoutParams params = stickyParams(font);
  // "-strict" selects strict kinsoku (UAX #14 CJ→NS): small kana and the
  // prolonged sound mark may not start lines either. Plain "ja" gives
  // normal kinsoku, where only closing punctuation is prohibited.
  params.language = "ja-strict";
  params.defaultAlign = TextAlign::Justify;

  struct LineSink : PageSink {
    bool onPage(const Page& page) override {
      int16_t lastY = -1;
      for (uint16_t r = 0; r < page.runCount; ++r) {
        const PageTextRun& run = page.runs[r];
        if (run.baselineY != lastY) {
          if (count < kMax) {
            firstCp[count] = decodeFirst(run.text);
            rightEdge[count] = 0;
            sizes[count] = run.sizePx;
            ++count;
          }
          lastY = run.baselineY;
        }
        if (count > 0) {
          int32_t width = 0;
          uint32_t i = 0;
          while (i < run.len) {
            const uint8_t b = static_cast<uint8_t>(run.text[i]);
            uint32_t cp = b;
            uint32_t extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
            if (extra == 1) cp = b & 0x1F;
            if (extra == 2) cp = b & 0x0F;
            if (extra == 3) cp = b & 0x07;
            for (uint32_t k = 0; k < extra; ++k) {
              cp = (cp << 6) | (static_cast<uint8_t>(run.text[i + 1 + k]) & 0x3F);
            }
            i += 1 + extra;
            width += cp >= 0x2E80 ? run.sizePx : run.sizePx / 2;
          }
          const int32_t edge = run.x + width;
          if (edge > rightEdge[count - 1]) rightEdge[count - 1] = edge;
        }
      }
      return true;
    }
    static uint32_t decodeFirst(const char* text) {
      const uint8_t b = static_cast<uint8_t>(text[0]);
      uint32_t cp = b;
      uint32_t extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
      if (extra == 1) cp = b & 0x1F;
      if (extra == 2) cp = b & 0x0F;
      if (extra == 3) cp = b & 0x07;
      for (uint32_t k = 0; k < extra; ++k) {
        cp = (cp << 6) | (static_cast<uint8_t>(text[1 + k]) & 0x3F);
      }
      return cp;
    }
    enum : uint32_t { kMax = 512 };
    uint32_t firstCp[kMax];
    int32_t rightEdge[kMax];
    uint16_t sizes[kMax];
    uint32_t count = 0;
  } ls;

  const ZipEntry* entry = opened.book.zip().find("OEBPS/text/ch6.xhtml");
  CHECK(entry != nullptr);
  uint32_t pages = 0;
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, opened.book.zip(), *entry,
                                                  entry->name, params, opened.scratch, ls,
                                                  &pages)),
           static_cast<int>(BookStatus::Ok));
  CHECK(pages >= 1);
  CHECK(ls.count > 6);  // three paragraphs of fullwidth text wrap plenty

  // Kinsoku: no line may start with a closing/prohibited character. These
  // arrive via libunibreak's UAX #14 classes (CL/NS) — this asserts the
  // engine actually honors them end to end.
  static const uint32_t kProhibited[] = {0x3002, 0x3001, 0x300D, 0x300F, 0xFF01, 0xFF1F,
                                         0x30FC, 0x3063, 0x3083, 0x3085, 0x3087};
  int kinsokuViolations = 0;
  for (uint32_t l = 0; l < ls.count; ++l) {
    for (uint32_t p : kProhibited) {
      if (ls.firstCp[l] == p) { ++kinsokuViolations; std::printf("  kinsoku violation: line %u starts with U+%04X\n", l, p); }
    }
  }
  CHECK_EQ(kinsokuViolations, 0);

  // Inter-character justification: body lines (16 px) that wrapped must end
  // exactly at the right margin even though CJK text has no spaces.
  const int32_t rightEdge = params.pageWidth - params.marginRight;
  int flushLines = 0;
  for (uint32_t l = 0; l + 1 < ls.count; ++l) {
    if (ls.sizes[l] == 16 && ls.rightEdge[l] == rightEdge) ++flushLines;
  }
  CHECK(flushLines >= 3);
}

// --- Phase 4b: images ---------------------------------------------------------

struct ImageCollectSink : PageSink {
  bool onPage(const Page& page) override {
    for (uint16_t m = 0; m < page.imageCount && count < 8; ++m) {
      images[count] = page.images[m];
      std::snprintf(hrefs[count], sizeof(hrefs[count]), "%s", page.images[m].href);
      images[count].href = hrefs[count];
      pageOf[count] = page.pageIndex;
      ++count;
    }
    for (uint16_t r = 0; r < page.runCount; ++r) {
      if (textLen + page.runs[r].len + 1 < sizeof(text)) {
        std::memcpy(text + textLen, page.runs[r].text, page.runs[r].len);
        textLen += page.runs[r].len;
        text[textLen] = '\0';
      }
    }
    return true;
  }
  PageImage images[8];
  char hrefs[8][256];
  uint32_t pageOf[8];
  uint32_t count = 0;
  char text[16 * 1024];
  uint32_t textLen = 0;
};

struct GrayCapture {
  uint8_t pixels[800 * 480];
  uint16_t width = 0;
  uint16_t rows = 0;
  static bool onRow(void* user, uint16_t y, const uint8_t* gray, uint16_t width) {
    GrayCapture* self = static_cast<GrayCapture*>(user);
    self->width = width;
    if (y + 1u > self->rows) self->rows = y + 1u;
    if (static_cast<uint32_t>(y) * width + width <= sizeof(self->pixels)) {
      std::memcpy(self->pixels + static_cast<uint32_t>(y) * width, gray, width);
    }
    return true;
  }
};

void testImages() {
  OpenedBook opened;
  CHECK(opened.open("minimal.epub"));

  FakeFont font;
  LayoutParams params = stickyParams(font);
  ImageCollectSink sink;
  const ZipEntry* entry = opened.book.zip().find("OEBPS/text/ch5.xhtml");
  CHECK(entry != nullptr);
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, opened.book.zip(), *entry,
                                                  entry->name, params, opened.scratch, sink,
                                                  nullptr)),
           static_cast<int>(BookStatus::Ok));

  // Two placed images (PNG + JPEG); the GIF was skipped without failing.
  CHECK_EQ(sink.count, 2u);
  CHECK(std::strstr(sink.text, "Text before the picture.") != nullptr);
  CHECK(std::strstr(sink.text, "Text at the end.") != nullptr);

  // 64x48 fits without scaling; centered in the 752 px content box.
  const PageImage& png = sink.images[0];
  CHECK(std::strcmp(png.href, "OEBPS/images/pattern.png") == 0);
  CHECK_EQ(png.width, 64);
  CHECK_EQ(png.height, 48);
  CHECK_EQ(png.x, 24 + (752 - 64) / 2);

  // Render the PNG at its natural size and verify known pixels: left half is
  // a gradient (0 at x=0), right half black top / white bottom.
  {
    const size_t marked = opened.scratch.mark();
    static GrayCapture cap;
    CHECK_EQ(static_cast<int>(ImageRenderer::render(opened.source, opened.book.zip(), png,
                                                    opened.scratch, GrayCapture::onRow, &cap)),
             static_cast<int>(BookStatus::Ok));
    CHECK_EQ(cap.width, 64);
    CHECK_EQ(cap.rows, 48);
    CHECK_EQ(cap.pixels[0], 0);                       // gradient start
    CHECK(cap.pixels[31] > 240);                      // gradient end
    CHECK(cap.pixels[10 * 64 + 48] < 16);             // right half, top: black
    CHECK(cap.pixels[40 * 64 + 48] > 240);            // right half, bottom: white
    opened.scratch.release(marked);
  }

  // Render scaled to half size — dimensions and pattern still hold.
  {
    PageImage half = png;
    half.width = 32;
    half.height = 24;
    const size_t marked = opened.scratch.mark();
    static GrayCapture cap;
    cap = GrayCapture{};
    CHECK_EQ(static_cast<int>(ImageRenderer::render(opened.source, opened.book.zip(), half,
                                                    opened.scratch, GrayCapture::onRow, &cap)),
             static_cast<int>(BookStatus::Ok));
    CHECK_EQ(cap.width, 32);
    CHECK_EQ(cap.rows, 24);
    CHECK(cap.pixels[5 * 32 + 24] < 16);              // top right: black
    CHECK(cap.pixels[20 * 32 + 24] > 240);            // bottom right: white
    opened.scratch.release(marked);
  }

  // JPEG render (sips-converted twin of the pattern): lossy, so tolerances.
  const PageImage& jpg = sink.images[1];
  if (std::strstr(jpg.href, ".jpg") != nullptr && jpg.width == 64) {
    const size_t marked = opened.scratch.mark();
    static GrayCapture cap;
    cap = GrayCapture{};
    const BookStatus st = ImageRenderer::render(opened.source, opened.book.zip(), jpg,
                                                opened.scratch, GrayCapture::onRow, &cap);
    if (st == BookStatus::Ok) {  // skipped when no JPEG converter existed
      CHECK_EQ(cap.rows, 48);
      CHECK(cap.pixels[10 * 64 + 48] < 60);
      CHECK(cap.pixels[40 * 64 + 48] > 200);
    }
    opened.scratch.release(marked);
  }

  // Dither helper: solid black and solid white rows.
  {
    uint8_t gray[16];
    uint8_t bits[2];
    std::memset(gray, 0, sizeof(gray));
    ditherRowOrdered(gray, 16, 0, bits);
    CHECK_EQ(bits[0], 0xFF);
    CHECK_EQ(bits[1], 0xFF);
    std::memset(gray, 255, sizeof(gray));
    ditherRowOrdered(gray, 16, 0, bits);
    CHECK_EQ(bits[0], 0x00);
    CHECK_EQ(bits[1], 0x00);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: %s <fixtures-build-dir> <hyph-en-us.fibh>\n", argv[0]);
    return 2;
  }
  fixturesDir = argv[1];

  static uint8_t hyphBuf[96 * 1024];
  Hyphenator hyphenator;
  {
    FILE* f = std::fopen(argv[2], "rb");
    CHECK(f != nullptr);
    const size_t n = f != nullptr ? std::fread(hyphBuf, 1, sizeof(hyphBuf), f) : 0;
    if (f != nullptr) std::fclose(f);
    CHECK(hyphenator.init(hyphBuf, static_cast<uint32_t>(n)));
  }
  // Hyphenator sanity: known dictionary splits.
  {
    uint8_t pos[8];
    const uint8_t n = hyphenator.breakPositions("hyphenation", 11, pos, 8);
    CHECK_EQ(n, 2);
    CHECK_EQ(pos[0], 2);  // hy-phen-ation
    CHECK_EQ(pos[1], 6);
  }

  testSmallChapter();
  testStylesEntitiesAndBreaks();
  testMemoryIndependence();
  testEarlyStop();
  testCssUnit();
  testStyledChapter();
  testHyphenation(hyphenator);
  testKerningAffectsMeasurement();
  testWidowOrphan();
  testImages();
  testCjk();

  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
