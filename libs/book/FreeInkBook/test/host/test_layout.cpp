// Host-side tests for FreeInkBook Phase 2 (streaming layout + pagination).
// A fake fixed-metrics font stands in for rasterization, so line breaking,
// block flow, style runs, page assembly, and — critically — the O(page)
// memory invariant are all verified with no device or font files.

#include <FreeInkBook.h>
#include <layout/ChapterLayout.h>

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
      while (i < run.len) {
        uint32_t cp = static_cast<uint8_t>(run.text[i]);
        uint32_t extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        i += 1 + extra;
        width += font_.advance(cp, run.sizePx, run.styleFlags);
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
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, *entry, params,
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
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, *entry, params,
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
    CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, *entry, params,
                                                    layoutArena, sink, &pages)),
             static_cast<int>(BookStatus::Ok));
    highWater[round] = layoutArena.highWater();
    if (round == 1) bigPages = pages;
  }

  std::printf("  layout high water: small %zu B, big %zu B (%u pages)\n", highWater[0],
              highWater[1], bigPages);
  // Ceiling anatomy: ~24 KB fixed layout buffers + 24 KB page sub-arena +
  // ~50 KB parse/inflate state. All O(1) in chapter size.
  CHECK(bigPages > 10);                             // the big chapter really paginated
  CHECK(highWater[1] < 112 * 1024);                 // absolute ceiling
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
  CHECK_EQ(static_cast<int>(ChapterLayout::layout(opened.source, *entry, params,
                                                  opened.scratch, sink, &pages)),
           static_cast<int>(BookStatus::Ok));
  CHECK_EQ(pages, 1u);  // sink declined more — layout stopped promptly
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s <fixtures-build-dir>\n", argv[0]);
    return 2;
  }
  fixturesDir = argv[1];

  testSmallChapter();
  testStylesEntitiesAndBreaks();
  testMemoryIndependence();
  testEarlyStop();

  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
