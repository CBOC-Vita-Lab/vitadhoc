/* widekit tests - MIT licence.  Per-frame cost of the library (layers + map + compose)
 * on a busy scene at the real margin.  usage: bench [frames] [scene] [skip_lib]  scene 0 = random
 * level (nothing periodic), 1 = flat periodic ground (the continuation is active) */
#include "synth.h"
#include <stdio.h>
#include <stdlib.h>
using namespace synth;

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 600;
    int scene = argc > 2 ? atoi(argv[2]) : 0;
    int skip = argc > 3 ? atoi(argv[3]) : 0;      // 1: harness only (instruction-count baseline)
    Opts o;
    o.name = "bench";
    o.frames = frames;
    o.margin = 70;
    o.sky = o.mtn = o.hud_layer = true;
    o.ground = scene;
    o.skip_lib = skip != 0;
    o.render_every = 100000;     // draw the candidate strips once; time only the library
    o.cam = [](int f) { return (int)(f * 1.5); };
    Sim s(o);
    s.run();
    printf("scene %d, %d frames: layers %.1f us/frame, map %.1f us/frame, compose %.1f us/frame, total %.1f us\n",
           scene, frames, s.t_layers_us / frames, s.t_map_us / frames, s.t_compose_us / frames,
           (s.t_layers_us + s.t_map_us + s.t_compose_us) / frames);
    wk_map_stats st;
    wk_map_stats_get(s.map(), &st);
    printf("map: %d cells, %d rooms, bound %d\n", st.cells, st.rooms, st.bound);
    return 0;
}
