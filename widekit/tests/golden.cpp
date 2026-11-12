/* widekit tests - MIT licence.  Golden composed frames: one line per frame of a set of synthetic
 * scenes at margins 48 / 70 / 84, hashing the composed wide picture (FNV-1a 64 over its RGB565
 * words) and the provenance map (0 when no provenance is requested).  Any change to the library
 * must leave every line identical.  usage: golden > file ; diff the two files.
 *   line: <variant> m<margin> g<ground> f<frame> pic <hash> prov <hash> */
#include "synth.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
using namespace synth;

static uint64_t fnv(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

static int pingpong(int f)
{
    const int p = f % 120;
    return (p < 60 ? p * 4 : (120 - p) * 4) + 40;   /* runs right, stops, runs back */
}

struct Variant {
    const char *name;
    void (*apply)(Opts &);
};

static void v_base(Opts &) {}
static void v_stipple(Opts &o) { o.fade_mode = WK_FADE_STIPPLE; }
static void v_smear(Opts &o) { o.smear_px = 12; }
static void v_split(Opts &o) { o.split = true; }
static void v_noperiod(Opts &o) { o.period = 0; }
static void v_noprov(Opts &o) { o.want_prov = false; }
static void v_pingpong(Opts &o) { o.cam = pingpong; }
static void v_swap(Opts &o) { o.script = [](Sim &s, int f) { if (f == 80) s.swap_tileset(); }; }
static void v_page(Opts &o) { o.engine = ENG_PAGE; }
static void v_nostamps(Opts &o) { o.fg_stamps = false; }
static void v_fade12(Opts &o) { o.fade_px = 12; o.smear_px = 6; }

int main()
{
    static const Variant variants[] = {
        {"base", v_base},       {"stipple", v_stipple}, {"smear", v_smear},
        {"split", v_split},     {"noperiod", v_noperiod}, {"noprov", v_noprov},
        {"pingpong", v_pingpong}, {"swap", v_swap},     {"page", v_page},
        {"nostamps", v_nostamps}, {"fade12", v_fade12},
    };
    static const int margins[3] = {48, 70, 84};
    for (const Variant &v : variants)
        for (int m : margins)
            for (int g = 0; g < 3; g++) {
                Opts o;
                o.name = v.name;
                o.frames = 150;
                o.margin = m;
                o.sky = o.mtn = o.hud_layer = true;
                o.ground = g;
                o.cam = [](int f) { return f * 3; };
                v.apply(o);
                Sim s(o);
                for (int f = 0; f < o.frames; f++) {
                    s.step();
                    uint64_t hp = fnv(s.composed().data(), s.composed().size() * 2);
                    uint64_t hv = o.want_prov ? fnv(s.prov().data(), s.prov().size()) : 0;
                    printf("%s m%d g%d f%d pic %016llx prov %016llx\n", v.name, m, g, f,
                           (unsigned long long)hp, (unsigned long long)hv);
                }
            }
    return 0;
}
