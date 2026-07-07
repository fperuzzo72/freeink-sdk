// FreeInkBook — streaming PNG/JPEG decode → scaled grayscale rows.

#include "render/ImageRenderer.h"

#include <string.h>

#include <pngle.h>
#include <tjpgd.h>

#include "epub/ImageProbe.h"

namespace freeink {
namespace book {

namespace {

// Maps completed source rows to output rows (nearest-neighbor both axes).
struct RowScaler {
  uint16_t srcW = 0;
  uint16_t srcH = 0;
  uint16_t dstW = 0;
  uint16_t dstH = 0;
  uint8_t* dstRow = nullptr;
  ImageRowFn rowFn = nullptr;
  void* user = nullptr;
  uint32_t nextDst = 0;
  bool stopped = false;
  bool failed = false;

  // Delivers every output row whose nearest source row is `srcY`.
  bool feedSourceRow(uint32_t srcY, const uint8_t* gray) {
    while (nextDst < dstH &&
           static_cast<uint64_t>(nextDst) * srcH / dstH == srcY) {
      for (uint32_t x = 0; x < dstW; ++x) {
        dstRow[x] = gray[static_cast<uint64_t>(x) * srcW / dstW];
      }
      if (!rowFn(user, static_cast<uint16_t>(nextDst), dstRow, dstW)) {
        stopped = true;
        return false;
      }
      ++nextDst;
    }
    return true;
  }
};

uint8_t rgbaToGray(const uint8_t rgba[4]) {
  const int32_t gray =
      (rgba[0] * 299 + rgba[1] * 587 + rgba[2] * 114) / 1000;
  // Blend over white (e-paper background).
  return static_cast<uint8_t>(255 - ((255 - gray) * rgba[3]) / 255);
}

// --- PNG ---------------------------------------------------------------------

struct PngState {
  RowScaler scaler;
  uint8_t* srcRow = nullptr;   // srcW grayscale accumulation
  uint32_t currentY = 0;
  bool unsupported = false;    // interlaced
};

void pngInitCallback(pngle_t* pngle, uint32_t w, uint32_t h) {
  PngState* state = static_cast<PngState*>(pngle_get_user_data(pngle));
  (void)w;
  (void)h;
  if (pngle_get_ihdr(pngle)->interlace != 0) state->unsupported = true;
}

void pngDrawCallback(pngle_t* pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     const uint8_t rgba[4]) {
  PngState* state = static_cast<PngState*>(pngle_get_user_data(pngle));
  if (state->unsupported || state->scaler.stopped) return;
  (void)w;
  (void)h;
  if (y != state->currentY) {
    // Source row finished (pngle emits strictly in raster order when
    // non-interlaced).
    if (!state->scaler.feedSourceRow(state->currentY, state->srcRow)) return;
    state->currentY = y;
  }
  if (x < state->scaler.srcW) state->srcRow[x] = rgbaToGray(rgba);
}

BookStatus renderPng(BookSource& source, const ZipEntry& entry, const PageImage& image,
                     uint16_t srcW, uint16_t srcH, Arena& scratch, ImageRowFn rowFn,
                     void* user) {
  PngState state;
  state.scaler.srcW = srcW;
  state.scaler.srcH = srcH;
  state.scaler.dstW = image.width;
  state.scaler.dstH = image.height;
  state.scaler.rowFn = rowFn;
  state.scaler.user = user;
  state.scaler.dstRow = static_cast<uint8_t*>(scratch.alloc(image.width, 1));
  state.srcRow = static_cast<uint8_t*>(scratch.alloc(srcW, 1));
  uint8_t* readBuf = static_cast<uint8_t*>(scratch.alloc(4096, 1));
  if (state.scaler.dstRow == nullptr || state.srcRow == nullptr || readBuf == nullptr) {
    return BookStatus::OutOfMemory;
  }
  memset(state.srcRow, 0xFF, srcW);

  ZipEntryReader reader;
  BookStatus status = reader.open(source, entry, scratch);
  if (status != BookStatus::Ok) return status;

  pngle_t* pngle = pngle_new();
  if (pngle == nullptr) return BookStatus::OutOfMemory;
  pngle_set_user_data(pngle, &state);
  pngle_set_init_callback(pngle, pngInitCallback);
  pngle_set_draw_callback(pngle, pngDrawCallback);

  status = BookStatus::Ok;
  for (;;) {
    const int32_t n = reader.read(readBuf, 4096);
    if (n < 0) {
      status = BookStatus::IoError;
      break;
    }
    if (n == 0) break;
    if (pngle_feed(pngle, readBuf, static_cast<size_t>(n)) < 0) {
      status = BookStatus::ParseError;
      break;
    }
    if (state.unsupported) {
      status = BookStatus::Unsupported;
      break;
    }
    if (state.scaler.stopped) break;
  }
  if (status == BookStatus::Ok && !state.scaler.stopped) {
    state.scaler.feedSourceRow(state.currentY, state.srcRow);  // flush last row
  }
  pngle_destroy(pngle);
  return status;
}

// --- JPEG --------------------------------------------------------------------

struct JpegState {
  RowScaler scaler;
  ZipEntryReader* reader = nullptr;
  uint8_t* skipBuf = nullptr;
  uint8_t* band = nullptr;      // srcW × kBandRows grayscale
  uint16_t bandW = 0;
  uint32_t bandTop = 0;         // source y of band row 0
  uint32_t bandFilled = 0;      // rows known to be complete (top-based)
  bool ioError = false;

  static constexpr uint32_t kBandRows = 16;

  void flushBandRows(uint32_t upToSrcY) {  // exclusive
    while (bandTop < upToSrcY && !scaler.stopped) {
      scaler.feedSourceRow(bandTop, band + (bandTop % kBandRows) * bandW);
      ++bandTop;
    }
  }
};

size_t jpegInput(JDEC* jd, uint8_t* buf, size_t len) {
  JpegState* state = static_cast<JpegState*>(jd->device);
  size_t total = 0;
  while (total < len) {
    uint8_t* dst = buf != nullptr ? buf + total : state->skipBuf;
    const uint32_t want =
        buf != nullptr ? static_cast<uint32_t>(len - total)
                       : static_cast<uint32_t>((len - total) < 1024 ? (len - total) : 1024);
    const int32_t n = state->reader->read(dst, want);
    if (n < 0) {
      state->ioError = true;
      return 0;
    }
    if (n == 0) break;
    total += static_cast<size_t>(n);
  }
  return total;
}

int jpegOutput(JDEC* jd, void* bitmap, JRECT* rect) {
  JpegState* state = static_cast<JpegState*>(jd->device);
  if (state->scaler.stopped) return 0;
  // Blocks arrive left→right within a band, bands top→bottom. When a new
  // band starts, all rows of the previous band are complete.
  if (static_cast<uint32_t>(rect->top) >= state->bandFilled) {
    state->flushBandRows(state->bandFilled);
    state->bandFilled = rect->bottom + 1;
  }
  const uint8_t* src = static_cast<const uint8_t*>(bitmap);
  const uint16_t rw = static_cast<uint16_t>(rect->right - rect->left + 1);
  for (uint16_t y = rect->top; y <= rect->bottom; ++y) {
    uint8_t* dst = state->band + (y % JpegState::kBandRows) * state->bandW + rect->left;
    memcpy(dst, src, rw);
    src += rw;
  }
  return 1;
}

BookStatus renderJpeg(BookSource& source, const ZipEntry& entry, const PageImage& image,
                      Arena& scratch, ImageRowFn rowFn, void* user) {
  JpegState state;
  state.skipBuf = static_cast<uint8_t*>(scratch.alloc(1024, 1));
  void* work = scratch.alloc(8 * 1024, alignof(max_align_t));
  ZipEntryReader reader;
  BookStatus status = reader.open(source, entry, scratch);
  if (status != BookStatus::Ok) return status;
  state.reader = &reader;
  if (state.skipBuf == nullptr || work == nullptr) return BookStatus::OutOfMemory;

  JDEC jd;
  if (jd_prepare(&jd, jpegInput, work, 8 * 1024, &state) != JDR_OK) {
    return state.ioError ? BookStatus::IoError : BookStatus::ParseError;
  }

  // Use TJpgDec's power-of-two downscale to get close to the target size
  // cheaply; the RowScaler handles the remainder.
  uint8_t scale = 0;
  while (scale < 3 && (jd.width >> (scale + 1)) >= image.width &&
         (jd.height >> (scale + 1)) >= image.height) {
    ++scale;
  }
  const uint16_t srcW = static_cast<uint16_t>(jd.width >> scale);
  const uint16_t srcH = static_cast<uint16_t>(jd.height >> scale);
  if (srcW == 0 || srcH == 0) return BookStatus::Unsupported;

  state.scaler.srcW = srcW;
  state.scaler.srcH = srcH;
  state.scaler.dstW = image.width < srcW ? image.width : srcW;
  state.scaler.dstH = image.height < srcH ? image.height : srcH;
  state.scaler.rowFn = rowFn;
  state.scaler.user = user;
  state.scaler.dstRow = static_cast<uint8_t*>(scratch.alloc(state.scaler.dstW, 1));
  state.bandW = srcW;
  state.band = static_cast<uint8_t*>(scratch.alloc(srcW * JpegState::kBandRows, 1));
  if (state.scaler.dstRow == nullptr || state.band == nullptr) return BookStatus::OutOfMemory;

  const JRESULT jr = jd_decomp(&jd, jpegOutput, scale);
  if (state.ioError) return BookStatus::IoError;
  if (jr != JDR_OK && !state.scaler.stopped) return BookStatus::ParseError;
  if (!state.scaler.stopped) state.flushBandRows(srcH);
  return BookStatus::Ok;
}

}  // namespace

void ditherRowOrdered(const uint8_t* gray, uint16_t width, uint16_t y, uint8_t* outBits) {
  static const uint8_t kBayer4[4][4] = {
      {15, 135, 45, 165}, {195, 75, 225, 105}, {60, 180, 30, 150}, {240, 120, 210, 90}};
  memset(outBits, 0, (width + 7u) / 8u);
  for (uint16_t x = 0; x < width; ++x) {
    if (gray[x] < kBayer4[y & 3][x & 3]) {
      outBits[x >> 3] |= static_cast<uint8_t>(0x80u >> (x & 7));
    }
  }
}

BookStatus ImageRenderer::render(BookSource& source, const ZipCatalog& zip,
                                 const PageImage& image, Arena& scratch, ImageRowFn rowFn,
                                 void* user) {
  if (image.width == 0 || image.height == 0) return BookStatus::Unsupported;
  const ZipEntry* entry = zip.find(image.href);
  if (entry == nullptr) return BookStatus::NotFound;

  const size_t marked = scratch.mark();
  ImageInfo info;
  BookStatus status = probeImage(source, *entry, scratch, &info);
  if (status == BookStatus::Ok) {
    switch (info.kind) {
      case ImageInfo::Kind::Png:
        status = renderPng(source, *entry, image, info.width, info.height, scratch, rowFn,
                           user);
        break;
      case ImageInfo::Kind::Jpeg:
        status = renderJpeg(source, *entry, image, scratch, rowFn, user);
        break;
      case ImageInfo::Kind::Unknown:
        status = BookStatus::Unsupported;
        break;
    }
  }
  scratch.release(marked);
  return status;
}

}  // namespace book
}  // namespace freeink
