#pragma once

// FreeInk — content protection: rights.xml and encryption.xml parsing.

#include <stdint.h>

#include <string>
#include <vector>

namespace freeink {
namespace content {

struct Rights {
  std::string encryptedKey;  // base64, RSA-wrapped content key
  std::string keyType;       // optional: first-pass obfuscation variant
  std::string user;          // account UUID this grant belongs to
  std::string device;        // device UUID
  std::string fulfillment;   // access-grant UUID
  std::string voucher;       // voucher id
  int64_t expiresAt = 0;     // epoch seconds, 0 = no expiry found
};

// Parses META-INF/rights.xml. Returns false on malformed input or a missing
// encryptedKey. Expiry comes from permissions/display (until or duration).
bool parseRightsXml(const std::string& xml, Rights* out);

// Parses META-INF/encryption.xml; appends the URIs of aes128-cbc-encrypted
// resources to `uris`. Returns false on malformed input.
bool parseEncryptionXml(const std::string& xml, std::vector<std::string>* uris);

}  // namespace content
}  // namespace freeink
