/* VitAdhoc: the VDP worker's renderer. The same source file as vdp_render.c compiled a second time with its names changed
 * (vdp_render_w_names.h), so the unchanged line renderer runs on the worker's own copy of the VDP state (vdp_thread.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vdp_render_w_names.h"
#include "vdp_render.c"
