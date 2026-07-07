#!/bin/sh
# Builds and runs the FreeInkBook host tests. No device or PlatformIO needed —
# the library is freestanding C++17. Fixture EPUBs are assembled here with the
# system `zip` (mimetype stored first, per the EPUB OCF spec).
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeinkbook-tests"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/fixtures" "$BUILD_DIR/obj"

# --- fixtures ---------------------------------------------------------------
cp -R ../fixtures/minimal "$BUILD_DIR/fixtures/minimal"
cp -R ../fixtures/ncxonly "$BUILD_DIR/fixtures/ncxonly"
python3 ../fixtures/gen_big_chapter.py "$BUILD_DIR/fixtures/minimal/OEBPS/text/ch2.xhtml"

# make_epub <stage-dir> <out.epub> <compression-flag>
make_epub() {
  (cd "$1" \
    && zip -X -q "$3" "$BUILD_DIR/fixtures/$2" mimetype \
    && zip -X -q -r "$3" "$BUILD_DIR/fixtures/$2" META-INF OEBPS)
}
make_epub "$BUILD_DIR/fixtures/minimal" minimal.epub -9
make_epub "$BUILD_DIR/fixtures/minimal" stored.epub -0
make_epub "$BUILD_DIR/fixtures/ncxonly" ncxonly.epub -9
head -c 4096 /dev/zero > "$BUILD_DIR/fixtures/garbage.bin"

# --- build ------------------------------------------------------------------
CC_FLAGS="-O1 -std=c99 -I../../include -I../../third_party/expat -I../../third_party/miniz"
for src in miniz_impl expat_xmlparse expat_xmlrole expat_xmltok; do
  cc $CC_FLAGS -c "../../src/vendor/$src.c" -o "$BUILD_DIR/obj/$src.o"
done

c++ -std=c++17 -Wall -Wextra -Werror \
  -I../../include -I../../third_party/expat -I../../third_party/miniz \
  ../../src/FreeInkBook.cpp \
  ../../src/epub/ZipCatalog.cpp \
  ../../src/epub/XmlSax.cpp \
  ../../src/epub/PackageParsers.cpp \
  test_freeinkbook.cpp \
  "$BUILD_DIR"/obj/*.o \
  -o "$BUILD_DIR/test_freeinkbook"

"$BUILD_DIR/test_freeinkbook" "$BUILD_DIR/fixtures"
