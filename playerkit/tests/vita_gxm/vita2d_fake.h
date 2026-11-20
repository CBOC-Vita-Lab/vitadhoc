/*
 * A GPU-faithful stand-in for libvita2d, for the host: the REAL vita2d.h and psp2/gxm.h types and enums
 * (so a texture format is the real SDK constant, not a copy), and a small software rasteriser that follows
 * what GXM does with them: ABGR vertex/tint colours, the texture's component swizzle, source-alpha blending,
 * the scissor. It records every draw so a test can ask what the console would do with the same calls.
 * Nothing here runs on a Vita; it is a model of the documented behaviour.
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef VITA2D_FAKE_H
#define VITA2D_FAKE_H

#include <stdint.h>
#include <vector>

namespace fakegxm {

/* what a texture's one byte becomes, decoded from the SDK's swizzle field */
struct Swizzle { bool known, alpha_from_data, rgb_from_data, alpha_const1; };
Swizzle decode_u8(unsigned format);

struct TexDraw {
	unsigned format;          /* SceGxmTextureFormat */
	uint32_t tint;            /* 0xAABBGGRR as handed to vita2d */
	float dx, dy, dw, dh;     /* destination box */
	double bg_lum;            /* mean relative luminance of the framebuffer under the box, before the draw */
	double ink_lum;           /* relative luminance of the tint, blended onto that background at full coverage */
	bool uploaded;            /* the texture held some data when it was drawn */
	bool same_thread;         /* created/written on the thread that drew it */
};

void reset();                              /* new frame store (960x544, black), empty log */
void clear(uint32_t abgr);
const std::vector<TexDraw> &tex_draws();
const uint32_t *framebuffer();             /* 960x544, 0xAABBGGRR, alpha forced 255 */
int bad_calls();                           /* null/invalid arguments seen by the fake */

}  // namespace fakegxm

#endif
