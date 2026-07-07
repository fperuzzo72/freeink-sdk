/* FreeInkBook only needs miniz's low-level streaming inflate (tinfl). The
 * archive, deflate, stdio, and zlib-compatibility layers are compiled out so
 * the vendored library stays small and never touches the filesystem or clock.
 * Include this header instead of <miniz.h> so every translation unit sees the
 * same configuration. */
#pragma once

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES

#include <miniz.h>
