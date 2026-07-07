#pragma once

// FreeInk SDK — streaming XML parsing for FreeInkBook.
//
// Thin SAX facade over the vendored expat. A ZIP entry is inflated and parsed
// in fixed-size chunks, so document size never affects RAM use — there is no
// DOM anywhere in the engine. Element names arrive as raw qualified names
// ("dc:title"); handlers match on the local part because EPUB files bind
// namespace prefixes inconsistently.
//
// Expat's internal pools use the system allocator; that heap use is bounded,
// lives only for the duration of one parse (book open / chapter build), and
// never appears in the steady-state page-turn path.

#include <stdint.h>

#include "BookArena.h"
#include "BookTypes.h"
#include "epub/ZipCatalog.h"

namespace freeink {
namespace book {

class XmlHandler {
 public:
  virtual ~XmlHandler() = default;
  // atts is a NULL-terminated list of name/value pairs, expat-style.
  virtual void onStartElement(const char* name, const char** atts) {
    (void)name;
    (void)atts;
  }
  virtual void onEndElement(const char* name) { (void)name; }
  virtual void onText(const char* text, int len) {
    (void)text;
    (void)len;
  }
};

class XmlSax {
 public:
  // Streams one ZIP entry through the handler. Inflate and parse buffers are
  // taken from `scratch` and released before returning.
  static BookStatus parseEntry(BookSource& source, const ZipEntry& entry, Arena& scratch,
                               XmlHandler& handler);
};

}  // namespace book
}  // namespace freeink
