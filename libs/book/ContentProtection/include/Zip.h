#pragma once

// FreeInk — content protection: minimal ZIP access for protected EPUBs.
//
// The content module is parser-agnostic: it does its own central-directory
// scan so it works alongside any EPUB parser (CrossPoint's ZipFile,
// FreeInkBook's ZipCatalog) without sharing their state. Only what the read
// path needs: entry lookup, local-header data offsets, raw entry reads.
//
// Freestanding C++17.

#include <stdint.h>

#include <string>
#include <vector>

#include "ByteSource.h"

namespace freeink {
namespace content {

struct ZipEntryInfo {
  std::string name;
  uint32_t compressedSize = 0;
  uint32_t uncompressedSize = 0;
  uint32_t localHeaderOffset = 0;
  uint16_t method = 0;  // 0 = stored, 8 = deflate
};

class ZipScan {
 public:
  // Scans the central directory. Returns false if not a ZIP container.
  bool open(ByteSource& source);

  const ZipEntryInfo* find(const std::string& name) const;
  const std::vector<ZipEntryInfo>& entries() const { return entries_; }

  // Offset of an entry's raw stored data (past its local header).
  bool dataOffset(ByteSource& source, const ZipEntryInfo& entry, uint64_t* out) const;

  // Reads an entry's raw stored bytes (the "compressed" form).
  bool readRaw(ByteSource& source, const ZipEntryInfo& entry, uint8_t* out) const;

 private:
  std::vector<ZipEntryInfo> entries_;
};

}  // namespace content
}  // namespace freeink
