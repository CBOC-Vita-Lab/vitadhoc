/* One translation unit for the two stb libraries the Vita video layer adds
 * (both public domain / MIT, see LICENSE beside this file):
 *
 *   stb_truetype     rasterises Inter into the UI's glyph atlases at start-up
 *                    (vita/video_vita_text.c)
 *   stb_image_write  PNG frame dumps (vita/video_vita.c vid_dump_ge)
 *
 * stb_image itself is the PSP tree's psp/stb_image.h, built by
 * psp/stb_impl.c, exactly as on the PSP.
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define STBI_WRITE_NO_STDIO_HDR_TGA
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
