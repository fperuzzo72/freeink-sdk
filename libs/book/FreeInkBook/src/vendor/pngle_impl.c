/* Compiles the vendored pngle against FreeInkBook's miniz (its quoted
 * "miniz.h" include resolves through -I to third_party/miniz). pngle
 * allocates transient scanline buffers with calloc/free at decode time only;
 * that use is bounded by image width and never touches the page-turn path. */
#include "../../third_party/pngle/pngle.c"
