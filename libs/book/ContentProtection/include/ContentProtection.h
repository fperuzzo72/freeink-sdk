#pragma once

// Content protection — the reader-facing seam for protected content.
//
// Scheme-neutral by design: the reader knows only "some items are encrypted;
// access them on read." The credential that grants access to protected content
// is produced off-device and dropped on the SD card;
// the reader never activates, never talks to a server, never handles account
// credentials. Today the one backend unwraps an RSA-wrapped content key from a
// rights document; a second backend (e.g. LCP) can be added behind the same
// interface without touching the reader.
//
// Freestanding C++17 core; storage/crypto injected. Nothing here can write the
// content out in the clear — access is on-read, into memory only.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace freeink {
namespace content {

// Provides in-memory access to protected items of an opened container.
class ContentDecryptor {
 public:
  virtual ~ContentDecryptor() = default;
  // Is this container-relative item stored encrypted?
  virtual bool isEncrypted(const std::string& itemPath) const = 0;
  // Read one protected item fully into `out` (transient, RAM only).
  virtual bool decrypt(const std::string& itemPath, std::vector<uint8_t>& out) = 0;
};

// Opens the protected read path for `epubPath` when the content is protected
// and this device holds the credential to read it. Returns:
//   non-null            -> route protected item reads through it
//   null, err empty     -> plain content (not protected) — read normally
//   null, err non-empty -> protected but unreadable (no credential / expired)
std::unique_ptr<ContentDecryptor> openProtectedBook(const std::string& epubPath, std::string& err);

}  // namespace content
}  // namespace freeink
