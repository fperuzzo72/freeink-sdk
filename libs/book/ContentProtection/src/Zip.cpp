#include "Zip.h"

#include <string.h>

#include <algorithm>

#include "Util.h"

namespace freeink {
namespace content {

namespace {
constexpr uint32_t EOCD_SIG = 0x06054b50;
constexpr uint32_t CDIR_SIG = 0x02014b50;
constexpr uint32_t LOCAL_SIG = 0x04034b50;

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace

bool ZipScan::open(ByteSource& source) {
  const uint64_t size = source.size();
  if (size < 22) return false;

  // EOCD lives in the last 64KB + 22 bytes.
  const uint64_t window = size < (0xFFFF + 22) ? size : (0xFFFF + 22);
  const uint64_t windowStart = size - window;
  std::vector<uint8_t> tail(static_cast<size_t>(window));
  if (source.readAt(windowStart, tail.data(), static_cast<uint32_t>(window)) < 0) return false;

  int64_t eocd = -1;
  for (int64_t i = static_cast<int64_t>(window) - 22; i >= 0; i--) {
    if (rd32(&tail[static_cast<size_t>(i)]) == EOCD_SIG) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) return false;

  const uint16_t count = rd16(&tail[static_cast<size_t>(eocd) + 10]);
  const uint32_t cdOffset = rd32(&tail[static_cast<size_t>(eocd) + 16]);

  entries_.clear();
  // Cap the reserve hint: `count` is an untrusted uint16 (up to 65535), so a
  // crafted EOCD could force an outsized reserve that OOMs a low-heap device.
  // The loop still handles a genuinely large directory — it just grows as
  // valid entries are actually read.
  entries_.reserve(count < 256 ? count : 256);

  uint8_t header[46];
  uint64_t p = cdOffset;
  for (uint16_t i = 0; i < count; i++) {
    if (source.readAt(p, header, sizeof(header)) != sizeof(header)) return false;
    if (rd32(header) != CDIR_SIG) return false;
    const uint16_t nameLen = rd16(header + 28);
    const uint16_t extraLen = rd16(header + 30);
    const uint16_t commentLen = rd16(header + 32);

    ZipEntryInfo e;
    e.method = rd16(header + 10);
    e.compressedSize = rd32(header + 20);
    e.uncompressedSize = rd32(header + 24);
    e.localHeaderOffset = rd32(header + 42);
    if (nameLen > 0 && nameLen < 512) {
      char name[512];
      if (source.readAt(p + 46, name, nameLen) != nameLen) return false;
      e.nameHash = fnv1a64(name, nameLen);
      entries_.push_back(e);
    }
    p += 46 + nameLen + extraLen + commentLen;
  }
  std::sort(entries_.begin(), entries_.end(),
            [](const ZipEntryInfo& a, const ZipEntryInfo& b) { return a.nameHash < b.nameHash; });
  return true;
}

const ZipEntryInfo* ZipScan::find(const std::string& name) const {
  const uint64_t hash = fnv1a64(name.data(), name.size());
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), hash,
                                   [](const ZipEntryInfo& e, uint64_t h) { return e.nameHash < h; });
  return (it != entries_.end() && it->nameHash == hash) ? &*it : nullptr;
}

bool ZipScan::dataOffset(ByteSource& source, const ZipEntryInfo& entry, uint64_t* out) const {
  uint8_t header[30];
  if (source.readAt(entry.localHeaderOffset, header, sizeof(header)) != sizeof(header)) {
    return false;
  }
  if (rd32(header) != LOCAL_SIG) return false;
  const uint16_t nameLen = rd16(header + 26);
  const uint16_t extraLen = rd16(header + 28);
  *out = entry.localHeaderOffset + 30 + nameLen + extraLen;
  return true;
}

bool ZipScan::readRaw(ByteSource& source, const ZipEntryInfo& entry, uint8_t* out) const {
  uint64_t offset;
  if (!dataOffset(source, entry, &offset)) return false;
  return source.readAt(offset, out, entry.compressedSize) ==
         static_cast<int32_t>(entry.compressedSize);
}

}  // namespace content
}  // namespace freeink
