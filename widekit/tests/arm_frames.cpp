/* widekit tests - MIT licence.  Whole-frame determinism: the composed pictures of a short
 * synthetic run are hashed; the hash must be identical on the host (scalar) and under
 * qemu-arm (NEON blend / masked copy).  Prints one line per scene. */
#include "synth.h"
#include "wk_layers.h"
#include <stdio.h>
#include <stdint.h>
using namespace synth;

static uint32_t crc_step(uint32_t h, uint16_t v) { return (h ^ v) * 16777619u; }

int main()
{
    for (int scene = 0; scene < 3; scene++) {
        Opts o;
        o.name = "frames";
        o.frames = 110;
        o.margin = 70;
        o.sky = o.mtn = o.hud_layer = true;
        o.ground = scene == 1 ? 1 : (scene == 2 ? 2 : 0);
        o.cam = [](int f) { return f * 3; };
        Sim s(o);
        uint32_t h = 2166136261u;
        for (int f = 0; f < o.frames; f++) {
            s.step();
            for (uint16_t v : s.composed()) h = crc_step(h, v);
        }
        printf("scene %d hash %08x\n", scene, h);
    }
    return 0;
}
