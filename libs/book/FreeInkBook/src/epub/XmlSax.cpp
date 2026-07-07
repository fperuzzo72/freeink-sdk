// FreeInkBook — expat-backed streaming XML parse of ZIP entries.

#include "epub/XmlSax.h"

#include <expat.h>

#include <new>

#include "text/EntityFilter.h"

namespace freeink {
namespace book {

namespace {

constexpr uint32_t kParseChunk = 2048;

void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  static_cast<XmlHandler*>(userData)->onStartElement(name, atts);
}

void XMLCALL endElement(void* userData, const XML_Char* name) {
  static_cast<XmlHandler*>(userData)->onEndElement(name);
}

void XMLCALL characterData(void* userData, const XML_Char* text, int len) {
  static_cast<XmlHandler*>(userData)->onText(text, len);
}

}  // namespace

BookStatus XmlSax::parseEntry(BookSource& source, const ZipEntry& entry, Arena& scratch,
                              XmlHandler& handler, bool filterHtmlEntities) {
  const size_t marked = scratch.mark();

  ZipEntryReader reader;
  BookStatus status = reader.open(source, entry, scratch);
  if (status != BookStatus::Ok) {
    scratch.release(marked);
    return status;
  }

  char* buf = static_cast<char*>(scratch.alloc(kParseChunk, 1));
  EntityFilter* filter = nullptr;
  if (filterHtmlEntities) {
    filter = static_cast<EntityFilter*>(scratch.alloc(sizeof(EntityFilter), alignof(EntityFilter)));
    if (filter != nullptr) {
      filter = new (filter) EntityFilter();
      filter->reset();
    }
  }
  if (buf == nullptr || (filterHtmlEntities && filter == nullptr)) {
    scratch.release(marked);
    return BookStatus::OutOfMemory;
  }

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (parser == nullptr) {
    scratch.release(marked);
    return BookStatus::OutOfMemory;
  }
  XML_SetUserData(parser, &handler);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  status = BookStatus::Ok;
  for (;;) {
    const int32_t n = filter != nullptr ? filter->read(reader, buf, kParseChunk)
                                        : reader.read(buf, kParseChunk);
    if (n < 0) {
      status = BookStatus::IoError;
      break;
    }
    if (XML_Parse(parser, buf, n, n == 0) == XML_STATUS_ERROR) {
      status = BookStatus::ParseError;
      break;
    }
    if (n == 0 || handler.stopParse) break;
  }

  XML_ParserFree(parser);
  scratch.release(marked);
  return status;
}

}  // namespace book
}  // namespace freeink
