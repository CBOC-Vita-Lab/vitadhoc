/* glyph_glue.cpp -- the app's copy of playerkit's glyph cache (pk_glyphs.cpp,
 * MIT).  The app already builds stb_truetype (third_party/stb/stb_impl_vita.c),
 * so the cache uses that one. */
#define PK_GLYPHS_EXTERNAL_STB
#include "../../playerkit/src/pk_glyphs.cpp"
