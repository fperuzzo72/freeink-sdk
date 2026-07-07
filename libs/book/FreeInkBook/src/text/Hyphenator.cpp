// FreeInkBook — Liang pattern matcher over FIBH data.

#include "text/Hyphenator.h"

#include <string.h>

namespace freeink {
namespace book {

namespace {

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint8_t toLowerAscii(uint8_t c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + 32) : c;
}

}  // namespace

bool Hyphenator::init(const uint8_t* data, uint32_t len) {
  patternCount_ = 0;
  if (data == nullptr || len < 12 || memcmp(data, "FIBH", 4) != 0) return false;
  const uint16_t version = static_cast<uint16_t>(data[4] | (data[5] << 8));
  if (version != 1) return false;
  maxPatLen_ = data[6];
  const uint32_t count = getU32(data + 8);
  if (len < 12 + count * 4) return false;
  data_ = data;
  offsets_ = reinterpret_cast<const uint32_t*>(data + 12);
  blob_ = data + 12 + count * 4;
  patternCount_ = count;
  return true;
}

const uint8_t* Hyphenator::keyAt(uint32_t index, uint8_t* keyLenOut) const {
  const uint8_t* rec = blob_ + getU32(reinterpret_cast<const uint8_t*>(offsets_ + index));
  *keyLenOut = rec[0];
  return rec + 1;
}

uint8_t Hyphenator::breakPositions(const char* word, uint32_t wordLen, uint8_t* out,
                                   uint8_t outCap) const {
  if (!ready() || wordLen < kLeftMin + kRightMin || wordLen > kMaxWord - 2) return 0;

  // Boundary-marked lowercase copy: ".word."
  uint8_t marked[kMaxWord + 2];
  marked[0] = '.';
  for (uint32_t i = 0; i < wordLen; ++i) {
    marked[i + 1] = toLowerAscii(static_cast<uint8_t>(word[i]));
  }
  marked[wordLen + 1] = '.';
  const uint32_t markedLen = wordLen + 2;

  uint8_t values[kMaxWord + 3];
  memset(values, 0, sizeof(values));

  // For every start position, narrow a sorted range one character at a time;
  // an exact-length match applies its inter-character values (max-merge).
  for (uint32_t start = 0; start < markedLen; ++start) {
    uint32_t lo = 0;
    uint32_t hi = patternCount_;
    const uint32_t maxLen =
        markedLen - start < maxPatLen_ ? markedLen - start : maxPatLen_;
    for (uint32_t depth = 0; depth < maxLen && lo < hi; ++depth) {
      const uint8_t c = marked[start + depth];
      // Narrow [lo, hi) to keys with key[depth] == c (all share the first
      // `depth` characters already).
      uint32_t a = lo;
      uint32_t b = hi;
      while (a < b) {  // first key with key[depth] >= c (or shorter keys first)
        const uint32_t mid = (a + b) / 2;
        uint8_t klen;
        const uint8_t* key = keyAt(mid, &klen);
        if (klen <= depth || key[depth] < c) {
          a = mid + 1;
        } else {
          b = mid;
        }
      }
      lo = a;
      b = hi;
      while (a < b) {  // first key with key[depth] > c
        const uint32_t mid = (a + b) / 2;
        uint8_t klen;
        const uint8_t* key = keyAt(mid, &klen);
        if (klen <= depth || key[depth] <= c) {
          a = mid + 1;
        } else {
          b = mid;
        }
      }
      hi = a;
      if (lo >= hi) break;

      // Exact match of length depth+1 sits at the range start (sorted order
      // puts shorter keys before their extensions).
      uint8_t klen;
      const uint8_t* key = keyAt(lo, &klen);
      if (klen == depth + 1) {
        const uint8_t* vals = key + klen;  // klen+1 values follow the key
        for (uint32_t v = 0; v <= klen; ++v) {
          if (vals[v] > values[start + v]) values[start + v] = vals[v];
        }
      }
    }
  }

  // values index v corresponds to a break between marked[v-1] and marked[v];
  // map back to word byte offsets and apply the typesetting minimums.
  uint8_t found = 0;
  for (uint32_t v = 1; v < markedLen - 1 && found < outCap; ++v) {
    const uint32_t wordPos = v - 1;  // break BEFORE word[wordPos]
    if ((values[v] & 1) == 0) continue;
    if (wordPos < kLeftMin || wordPos > wordLen - kRightMin) continue;
    out[found++] = static_cast<uint8_t>(wordPos);
  }
  return found;
}

}  // namespace book
}  // namespace freeink
