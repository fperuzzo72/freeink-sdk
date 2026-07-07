#pragma once

// FreeInk SDK — storage interfaces for FreeInkBook.
//
// The engine never touches a filesystem directly. The application supplies a
// random-access view of the book container file (SD card, flash partition,
// or a host file in unit tests). Interfaces are deliberately tiny so a board
// adapter is a few lines.
//
// Freestanding C++17 — no Arduino or ESP-IDF dependency.

#include <stdint.h>

namespace freeink {
namespace book {

// Random-access byte source for one open book file.
//
// readAt() returns the number of bytes actually read (which may be short at
// end of file), or a negative value on I/O error. Implementations must allow
// arbitrary offsets; FreeInkBook seeks between the ZIP directory and item
// data while streaming.
class BookSource {
 public:
  virtual ~BookSource() = default;
  virtual int32_t readAt(uint64_t offset, void* dst, uint32_t len) = 0;
  virtual uint64_t size() const = 0;
};

}  // namespace book
}  // namespace freeink
