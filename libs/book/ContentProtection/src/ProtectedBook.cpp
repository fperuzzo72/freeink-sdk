#include "ProtectedBook.h"

#include <string.h>

#include "ContentMinizConfig.h"
#include "Util.h"

namespace freeink {
namespace content {

namespace {
constexpr const char* kEncryptionXml = "META-INF/encryption.xml";
constexpr const char* kRightsXml = "META-INF/rights.xml";
constexpr size_t kRsaBlock = 128;  // RSA-1024
}  // namespace

bool ProtectedBook::open(ByteSource& source, Crypto& crypto, const Credential& identity,
                     const std::string& rightsXmlOverride) {
  lastError_.clear();
  protected_ = false;

  if (!zip_.open(source)) {
    lastError_ = "not a zip container";
    return false;
  }

  if (!zip_.find(kEncryptionXml)) {
    return true;  // plain EPUB; nothing to do
  }

  std::string encryptionXml;
  if (!readEntryInflated(source, kEncryptionXml, &encryptionXml)) {
    lastError_ = "failed to read encryption.xml";
    return false;
  }
  if (!parseEncryptionXml(encryptionXml, &encryptedUris_)) {
    lastError_ = "failed to parse encryption.xml";
    return false;
  }

  // encryption.xml is present but lists no AES-128-CBC content — this is a
  // plain EPUB that merely obfuscates its embedded fonts (IDPF/Adobe font
  // mangling), not content protection. Nothing to unwrap; open normally and
  // require no rights document or credential.
  if (encryptedUris_.empty()) {
    return true;  // protected_ stays false
  }

  // Prefer the out-of-band rights document (sidecar); fall back to reading
  // META-INF/rights.xml from the zip for the legacy in-container form.
  std::string rightsXml = rightsXmlOverride;
  if (rightsXml.empty() && !readEntryInflated(source, kRightsXml, &rightsXml)) {
    lastError_ = "failed to read rights.xml";
    return false;
  }
  if (!parseRightsXml(rightsXml, &rights_)) {
    lastError_ = "failed to parse rights.xml";
    return false;
  }

  if (!rights_.user.empty() && !identity.userUuid.empty() && rights_.user != identity.userUuid) {
    lastError_ = "registered to a different account";
    return false;
  }

  if (!unwrapBookKey(crypto, identity, bookKey_)) {
    lastError_ = "content key unavailable (wrong/incomplete credential?)";
    return false;
  }

  protected_ = true;
  return true;
}

bool ProtectedBook::isEncrypted(const std::string& name) const {
  for (const auto& uri : encryptedUris_) {
    if (uri == name) return true;
  }
  return false;
}

bool ProtectedBook::unwrapBookKey(Crypto& crypto, const Credential& identity, uint8_t out[16]) {
  std::string encryptedKey = base64Decode(rights_.encryptedKey);

  // Optional first pass (newer ACS variants): the wrapped key is itself
  // AES-encrypted with a key derived from the keyType attribute and an IV
  // derived from the device/fulfillment/voucher UUIDs.
  if (!rights_.keyType.empty()) {
    uint8_t digest[32];
    crypto.sha256(reinterpret_cast<const uint8_t*>(rights_.keyType.data()), rights_.keyType.size(),
                  digest);
    const long nonce = strtol(rights_.keyType.c_str(), nullptr, 10);
    // keyType is attacker-controlled (from rights.xml); C++ % keeps the sign, so
    // a negative value would make `remainder` negative and drive the memcpys out
    // of bounds. Normalize into [0,16) — matches the reference's unsigned rotate.
    const int remainder = static_cast<int>(((nonce % 16) + 16) % 16);

    uint8_t key[16];
    memcpy(key, digest + remainder * 2, 16 - remainder);
    memcpy(key + (16 - remainder), digest + remainder, remainder);

    uint8_t deviceId[16], fulfillmentId[16], voucherId[16], iv[16];
    if (!uuidBytes(rights_.device, deviceId) || !uuidBytes(rights_.fulfillment, fulfillmentId) ||
        !uuidBytes(rights_.voucher, voucherId)) {
      return false;
    }
    for (int i = 0; i < 16; i++) iv[i] = deviceId[i] ^ fulfillmentId[i] ^ voucherId[i];

    std::vector<uint8_t> firstPass(encryptedKey.size());
    if (!crypto.aes128CbcDecrypt(key, iv, reinterpret_cast<const uint8_t*>(encryptedKey.data()),
                                 encryptedKey.size(), firstPass.data())) {
      return false;
    }
    // A trailing 16x 0x10 block is OpenSSL padding; remove when present.
    size_t len = firstPass.size();
    bool padded = len >= 16;
    for (size_t i = len - 16; padded && i < len; i++) padded = firstPass[i] == 0x10;
    if (padded) len -= 16;
    encryptedKey.assign(reinterpret_cast<const char*>(firstPass.data()), len);
  }

  if (encryptedKey.size() != kRsaBlock) return false;

  const std::string pkcs8 = base64Decode(identity.privateLicenseKey);
  if (pkcs8.empty()) return false;

  uint8_t block[kRsaBlock];
  const int32_t n = crypto.rsaPrivateRaw(
      reinterpret_cast<const uint8_t*>(pkcs8.data()), pkcs8.size(),
      reinterpret_cast<const uint8_t*>(encryptedKey.data()), encryptedKey.size(), block,
      sizeof(block));
  if (n != static_cast<int32_t>(sizeof(block))) return false;

  // PKCS#1 v1.5 type-2 padding: 0x00 0x02 <random nonzero> 0x00 <key at end>.
  if (block[0] != 0x00 || block[1] != 0x02 || block[sizeof(block) - 16 - 1] != 0x00) return false;
  memcpy(out, block + sizeof(block) - 16, 16);
  return true;
}

bool ProtectedBook::decryptEntry(ByteSource& source, Crypto& crypto, const std::string& name,
                             std::vector<uint8_t>* out) {
  const ZipEntryInfo* entry = zip_.find(name);
  if (!entry) {
    lastError_ = "entry not found: " + name;
    return false;
  }
  if (entry->compressedSize < 32 || (entry->compressedSize - 16) % 16 != 0) {
    lastError_ = "entry size not AES-shaped";
    return false;
  }

  std::vector<uint8_t> raw(entry->compressedSize);
  if (!zip_.readRaw(source, *entry, raw.data())) {
    lastError_ = "entry read failed";
    return false;
  }

  uint8_t iv[16];
  memcpy(iv, raw.data(), 16);
  std::vector<uint8_t> plain(raw.size() - 16);
  if (!crypto.aes128CbcDecrypt(bookKey_, iv, raw.data() + 16, raw.size() - 16, plain.data())) {
    lastError_ = "content read failed";
    return false;
  }

  // PKCS#7 unpad.
  const uint8_t pad = plain.back();
  size_t plainLen = plain.size();
  if (pad >= 1 && pad <= 16 && pad <= plainLen) {
    bool valid = true;
    for (size_t i = plainLen - pad; i < plainLen; i++) valid = valid && plain[i] == pad;
    if (valid) plainLen -= pad;
  }

  // Content is stored as raw deflate. Some producers' streams only terminate
  // cleanly with a trailing byte present (the inept 'Z' convention) — retry
  // with one appended if the first attempt fails.
  if (inflateRaw(plain.data(), plainLen, entry->uncompressedSize, out)) return true;
  // Force the byte at plainLen to 'Z' and inflate one extra byte. With invalid
  // padding plainLen == plain.size(), so grow first to keep the write in bounds
  // (resize may reallocate; inflateRaw re-fetches plain.data()).
  if (plainLen == plain.size()) plain.resize(plainLen + 1);
  plain[plainLen] = 'Z';
  if (inflateRaw(plain.data(), plainLen + 1, entry->uncompressedSize, out)) return true;

  lastError_ = "inflate failed";
  return false;
}

bool ProtectedBook::readEntryInflated(ByteSource& source, const std::string& name, std::string* out) {
  const ZipEntryInfo* entry = zip_.find(name);
  if (!entry) return false;

  std::vector<uint8_t> raw(entry->compressedSize);
  if (!zip_.readRaw(source, *entry, raw.data())) return false;

  if (entry->method == 0) {
    out->assign(reinterpret_cast<const char*>(raw.data()), raw.size());
    return true;
  }
  if (entry->method != 8) return false;

  std::vector<uint8_t> inflated;
  if (!inflateRaw(raw.data(), raw.size(), entry->uncompressedSize, &inflated)) return false;
  out->assign(reinterpret_cast<const char*>(inflated.data()), inflated.size());
  return true;
}

bool ProtectedBook::inflateRaw(const uint8_t* in, size_t inLen, size_t expectedOut,
                           std::vector<uint8_t>* out) {
  out->clear();
  out->reserve(expectedOut > 0 ? expectedOut : inLen * 3);

  mz_stream stream;
  memset(&stream, 0, sizeof(stream));
  stream.next_in = in;
  stream.avail_in = static_cast<unsigned int>(inLen);

  if (mz_inflateInit2(&stream, -15) != MZ_OK) return false;

  uint8_t chunk[4096];
  int status;
  do {
    stream.next_out = chunk;
    stream.avail_out = sizeof(chunk);
    status = mz_inflate(&stream, MZ_NO_FLUSH);
    if (status != MZ_OK && status != MZ_STREAM_END && status != MZ_BUF_ERROR) {
      mz_inflateEnd(&stream);
      return false;
    }
    const size_t produced = sizeof(chunk) - stream.avail_out;
    out->insert(out->end(), chunk, chunk + produced);
    if (status == MZ_BUF_ERROR && produced == 0) {
      mz_inflateEnd(&stream);
      return false;  // no progress possible
    }
  } while (status != MZ_STREAM_END);

  mz_inflateEnd(&stream);
  return true;
}

}  // namespace content
}  // namespace freeink
