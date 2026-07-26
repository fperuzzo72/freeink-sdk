#include "ContentProtection.h"

#include <HalStorage.h>
#include <Memory.h>
#include <time.h>

#include "ByteSource.h"
#include "Credential.h"
#include "ProtectedBook.h"
#include "WolfsslCrypto.h"
#include "Zip.h"

namespace freeink {
namespace content {

namespace {

// The access credential is provisioned off-device and dropped here.
// Generic path — the reader carries no scheme name.
constexpr const char* kCredentialPath = "/.crosspoint/content.key";

// One shared crypto backend for the whole read path.
WolfsslCrypto& crypto() {
  static WolfsslCrypto instance;
  return instance;
}

// ByteSource over an SD file (read-only). One open handle per instance.
class SdByteSource : public ByteSource {
 public:
  explicit SdByteSource(std::string path) : path_(std::move(path)) {}
  bool open() {
    file_ = Storage.open(path_.c_str(), O_RDONLY);
    return file_ && file_.isOpen();
  }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (!file_ || !file_.seek64(offset)) return -1;
    return file_.read(dst, len);
  }
  uint64_t size() const override { return file_ ? file_.fileSize64() : 0; }

 private:
  std::string path_;
  mutable HalFile file_;
};

// Adapts an opened ProtectedBook to the reader-facing access interface.
class ProtectedBookDecryptor : public ContentDecryptor {
 public:
  ProtectedBookDecryptor(std::string epubPath, std::unique_ptr<ProtectedBook> book)
      : epubPath_(std::move(epubPath)), book_(std::move(book)) {}

  bool isEncrypted(const std::string& itemPath) const override { return book_->isEncrypted(itemPath); }

  bool decrypt(const std::string& itemPath, std::vector<uint8_t>& out) override {
    SdByteSource source(epubPath_);
    if (!source.open()) return false;
    return book_->decryptEntry(source, crypto(), itemPath, &out);
  }

 private:
  std::string epubPath_;
  std::unique_ptr<ProtectedBook> book_;
};

}  // namespace

std::unique_ptr<ContentDecryptor> openProtectedBook(const std::string& epubPath, std::string& err) {
  err.clear();
  if (!Storage.exists(epubPath.c_str())) return nullptr;

  SdByteSource source(epubPath);
  if (!source.open()) return nullptr;

  // Cheap protection sniff: encryption.xml present? Plain EPUBs return here.
  ZipScan probe;
  if (!probe.open(source) || !probe.find("META-INF/encryption.xml")) return nullptr;

  // Load the access credential.
  SdByteSource credSource(kCredentialPath);
  Credential credential;
  if (!credSource.open() || !parseCredential(credSource, &credential)) {
    err = "no content access key on this device";
    return nullptr;
  }

  auto book = makeUniqueNoThrow<ProtectedBook>();
  if (!book) {
    err = "out of memory";
    return nullptr;
  }
  // Prefer an out-of-band rights document delivered as a sidecar next to the
  // EPUB ("<book>.epub.rights"), so the EPUB on disk stays byte-identical to
  // what the server sent. Falls back to a rights.xml injected into the zip.
  std::string rightsOverride;
  {
    SdByteSource rightsSource(epubPath + ".rights");
    if (rightsSource.open()) {
      const uint64_t rsize = rightsSource.size();
      if (rsize > 0 && rsize <= 256 * 1024) {
        rightsOverride.resize(static_cast<size_t>(rsize));
        const int32_t rn = rightsSource.readAt(0, rightsOverride.data(), static_cast<uint32_t>(rsize));
        if (rn <= 0) rightsOverride.clear();
        else rightsOverride.resize(static_cast<size_t>(rn));
      }
    }
  }
  if (!book->open(source, crypto(), credential, rightsOverride)) {
    err = "cannot open protected content: " + book->lastError();
    return nullptr;
  }

  const int64_t now = static_cast<int64_t>(time(nullptr));
  if (book->isExpired(now)) {
    err = "access expired";
    return nullptr;
  }

  return std::unique_ptr<ContentDecryptor>(new ProtectedBookDecryptor(epubPath, std::move(book)));
}

}  // namespace content
}  // namespace freeink
