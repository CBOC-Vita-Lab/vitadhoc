/* widekit tests - MIT licence.
 *
 * Scenario tests run the whole pipeline (wk_layers -> wk_map -> candidate
 * strips -> wk_compose) against a synthetic game and check EVERY claimed
 * margin pixel against the true wide picture ("wrong" must be 0).  Unit tests
 * cover the profile parser, persistence, corruption tolerance, the compose
 * hysteresis and the blend kernel. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "png.h"
#include "synth.h"

using namespace synth;

static int g_fail = 0, g_checks = 0;
static const char *g_shots = NULL;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_fail++;                                                            \
            printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)
#define CHECK_MSG(cond, ...)                                                     \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_fail++;                                                            \
            printf("    FAIL %s:%d  %s  [", __FILE__, __LINE__, #cond);          \
            printf(__VA_ARGS__);                                                 \
            printf("]\n");                                                       \
        }                                                                        \
    } while (0)

static double mean(const std::vector<float> &v, int a, int b)
{
    double s = 0;
    int n = 0;
    a = std::max(a, 0);
    b = std::min(b, (int)v.size());
    for (int i = a; i < b; i++) { s += v[(size_t)i]; n++; }
    return n ? s / n : 0;
}

static long total_wrong(const Metrics &m)
{
    return m.wrong[0] + m.wrong[1] + m.wrong[2] + m.wrong[3];
}

/* top half = truth, bottom half = widekit output, 2x; second image = provenance */
static void shot(Sim &s, const char *name)
{
    if (!g_shots) return;
    std::vector<uint8_t> t, c, p;
    int w, h;
    s.snapshot(t, c, p, w, h);
    const int S = 2;
    const int W = w * S, H = h * S * 2;
    std::vector<uint8_t> out((size_t)W * H * 3), pv((size_t)W * (h * S) * 3);
    for (int y = 0; y < h * S; y++)
        for (int x = 0; x < W; x++)
            for (int k = 0; k < 3; k++) {
                out[((size_t)y * W + x) * 3 + k] = t[((size_t)(y / S) * w + x / S) * 3 + k];
                out[((size_t)(y + h * S) * W + x) * 3 + k] = c[((size_t)(y / S) * w + x / S) * 3 + k];
                pv[((size_t)y * W + x) * 3 + k] = p[((size_t)(y / S) * w + x / S) * 3 + k];
            }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.png", g_shots, name);
    png_write(path, W, H, out);
    snprintf(path, sizeof path, "%s/%s_prov.png", g_shots, name);
    png_write(path, W, h * S, pv);
}

/* ------------------------------------------------------------------------ */
/* scenarios                                                                  */
/* ------------------------------------------------------------------------ */

static void s1_scroller()
{
    printf("S1  scroller: sky + mountains + level + HUD layer, forward / back / forward\n");
    Opts o;
    o.name = "s1";
    o.frames = 860;
    o.sky = o.mtn = o.hud_layer = true;
    o.cam = [](int f) {
        if (f < 310) return f * 3;
        if (f < 330) return 930;
        if (f < 600) return 930 - (f - 330) * 3;
        return 120 + (f - 600) * 3;
    };
    Sim s(o);
    for (int f = 0; f < o.frames; f++) {
        s.step();
        if (f == 150) shot(s, "s1_f150_forward_lead_fade");
        if (f == 280) shot(s, "s1_f280_forward");
        if (f == 500) shot(s, "s1_f500_reverse_history");
        if (f == 700) shot(s, "s1_f700_second_pass");
    }
    Metrics &m = s.m();
    printf("    wrong sky/mtn/fg/other = %ld/%ld/%ld/%ld  claimed %ld/%ld/%ld\n", m.wrong[0], m.wrong[1],
           m.wrong[2], m.wrong[3], m.claimed[0], m.claimed[1], m.claimed[2]);
    printf("    backdrop from frame sky=%d mtn=%d, hud lines from %d (a HUD layer over level lines is not a fixed line), real L/R pass1 %.1f/%.1f  pass2 %.1f/%.1f\n",
           m.back_frame[0], m.back_frame[1], m.hud_frame, mean(m.real[0], 100, 300), mean(m.real[1], 100, 300),
           mean(m.real[0], 640, 780), mean(m.real[1], 640, 780));
    CHECK(total_wrong(m) == 0);
    CHECK(m.first_wrong_frame == -1);
    CHECK(m.stale_gfx_drawn == 0);
    CHECK(m.claimed[2] > 100000 && m.claimed[1] > 100000 && m.claimed[0] > 10000);
    CHECK(m.back_frame[0] > 0 && m.back_frame[0] < 80);
    CHECK(m.back_frame[1] > 0 && m.back_frame[1] < 80);
    CHECK(m.kind[2][300] == WK_KIND_LEVEL);
    CHECK(m.kind[3][300] == WK_KIND_HUD);
    CHECK(m.hs_missed == 0);
    // trail side is history: full margin; lead side only a few tiles on a streaming engine
    CHECK(mean(m.real[0], 100, 300) >= 47.5);
    CHECK(mean(m.real[1], 100, 300) >= 8 && mean(m.real[1], 100, 300) <= 30);
    // coming back over the learned level: right side is full again
    CHECK(mean(m.real[1], 420, 500) >= 47.5);
    // second pass: the learned map extends the lead beyond what pass 1 had; the
    // whole stretch of level pass 1 walked (cam < 930) is served in full
    CHECK(mean(m.real[1], 640, 780) > mean(m.real[1], 100, 300) + 5);
    CHECK(mean(m.real[1], 620, 800) >= 47.5);
    // shown extent never exceeds the real extent
    for (size_t i = 0; i < m.shown[1].size(); i++) CHECK_MSG(m.shown[1][i] <= m.real[1][i] + 0.01f, "f=%zu", i);
}

static void s2_single_screen()
{
    printf("S2  single screen: nothing scrolls, so nothing may be invented\n");
    Opts o;
    o.name = "s2";
    o.frames = 160;
    o.sky = true;
    o.cam = [](int) { return 0; };
    Sim s(o);
    s.run();
    Metrics &m = s.m();
    printf("    wrong %ld  claimed fg %ld sky %ld, soft %ld of %ld margin px\n", total_wrong(m), m.claimed[2],
           m.claimed[0], m.soft_px, m.margin_px);
    CHECK(total_wrong(m) == 0);
    CHECK(m.claimed[0] == 0);                            // sky is not a backdrop without travel
    CHECK(m.soft_px * 100 >= m.margin_px * 70);          // mostly the soft edge
    shot(s, "s2_single_screen");
}

static void s3_rooms()
{
    printf("S3  rooms: screen flips between room 0 / 1 / 2, revisits are recognised\n");
    Opts o;
    o.name = "s3";
    o.frames = 620;
    o.engine = ENG_ROOMS;
    o.sky = true;
    o.script = [](Sim &s, int f) {
        if (f == 100) { s.load_room(1); s.flip_to_room(1); }
        if (f == 200) { s.load_room(2); s.flip_to_room(2); }
        if (f == 300) { s.load_room(0); s.flip_to_room(0); }
        if (f == 400) { s.load_room(1); s.flip_to_room(1); }
        if (f == 500) { s.load_room(2); s.flip_to_room(2); }
    };
    Sim s(o);
    s.run();
    Metrics &m = s.m();
    wk_map_stats st;
    wk_map_stats_get(s.map(), &st);
    printf("    wrong %ld claimed fg %ld, rooms %d new_rooms %u binds %u\n", total_wrong(m), m.claimed[2], st.rooms,
           st.new_rooms, st.binds);
    CHECK(total_wrong(m) == 0);
    CHECK(m.stale_gfx_drawn == 0);
    CHECK(m.claimed[2] == 0);        // every margin lies outside the room
    CHECK(st.new_rooms == 3);        // three distinct rooms ...
    CHECK(st.binds >= 3);            // ... every revisit (flip back to 0, 1, 2) is recognised
    CHECK(st.rooms == 3);
    shot(s, "s3_rooms");
}

static void s4_stale_lead()
{
    printf("S4  stale ring ahead of the camera (lead 1, no map, travel > ring width)\n");
    Opts o;
    o.name = "s4";
    o.frames = 700;
    o.lead_r = o.lead_l = 1;
    o.use_map = false;
    o.cam = [](int f) {
        if (f < 400) return f * 4;
        return 1600 - (f - 400) * 4;
    };
    Sim s(o);
    s.run();
    Metrics &m = s.m();
    printf("    wrong %ld claimed fg %ld, hindsight checked %ld missed %ld\n", total_wrong(m), m.claimed[2],
           m.hs_checked, m.hs_missed);
    CHECK(total_wrong(m) == 0);
    CHECK(m.claimed[2] > 0);
    shot(s, "s4_stale_lead");
}

static void s5_tileset_swap()
{
    printf("S5  tileset swap mid-scroll with a map that knows the old tiles\n");
    Opts o;
    o.name = "s5";
    o.frames = 520;
    o.sky = o.mtn = true;
    o.cam = [](int f) { return f < 300 ? f * 3 : 900 - (f - 300) * 3; };
    o.script = [](Sim &s, int f) {
        if (f == 300) s.swap_tileset();
    };
    Sim s(o);
    s.run();
    Metrics &m = s.m();
    printf("    wrong %ld stale_gfx_drawn %ld\n", total_wrong(m), m.stale_gfx_drawn);
    CHECK(total_wrong(m) == 0);
    CHECK(m.stale_gfx_drawn == 0);
    shot(s, "s5_tileset_swap");
}

static void s5b_margin_cell_gfx_gate()
{
    printf("S5b direct: margin_cell refuses cells recorded with another tileset\n");
    const int W = 64, H = 28;
    std::vector<uint32_t> code((size_t)W * H), hash((size_t)W * H), stamp((size_t)W * H, 1000);
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            code[(size_t)r * W + c] = 1 + (uint32_t)((c * 7 + r * 13) % 200);
            hash[(size_t)r * W + c] = (code[(size_t)r * W + c] * 2654435761u) ^ 0x1234u;
        }
    wk_layers *L = wk_layers_create(NULL);
    wk_map_cfg mc;
    wk_map_cfg_default(&mc);
    wk_map *M = wk_map_create(&mc, 1);
    wk_frame fr;
    wk_out out;
    memset(&out, 0, sizeof out);
    int cam = 0;
    for (int f = 0; f < 200; f++) {
        memset(&fr, 0, sizeof fr);
        fr.frame = 1000 + (uint32_t)f;
        fr.view_w = 256;
        fr.view_h = 224;
        fr.margin = 48;
        fr.brightness = f < 4 ? 0 : 255;
        fr.palette_size = 256;
        fr.nlayers = 1;
        wk_layer &l = fr.layer[0];
        l.active = 1;
        l.role = WK_ROLE_AUTO;
        l.map_w = W;
        l.map_h = H;
        l.tile_px = 8;
        l.hash = hash.data();
        l.code = code.data();
        l.stamp = stamp.data();
        l.gfx_id = 77;
        cam = f < 4 ? 0 : 200 + (f - 4) * 2;
        for (int y = 0; y < 224; y++) {
            l.scroll_x[y] = cam % (W * 8);
            l.row_y[y] = y;
        }
        wk_layers_update(L, &fr, &out);
        wk_map_update(M, &fr, &out);
        wk_map_serve(M, &fr, &out);
    }
    wk_cell cell;
    int ok = 0;
    for (int idx = 1; idx <= 3; idx++)
        for (int y = 40; y < 200; y += 8) ok += wk_map_margin_cell(M, &fr, &out, 0, y, 0, idx, &cell);
    CHECK(ok > 0);
    wk_frame fr2 = fr;
    fr2.layer[0].gfx_id = 78;      // same epoch, different tileset identity
    int bad = 0;
    for (int idx = 1; idx <= 3; idx++)
        for (int y = 40; y < 200; y += 8) bad += wk_map_margin_cell(M, &fr2, &out, 0, y, 0, idx, &cell);
    CHECK(bad == 0);
    wk_map_destroy(M);
    wk_layers_destroy(L);
}

static void s6_split_hud()
{
    printf("S6  NES style: status bar lines 0-23 fixed by a split, level scrolls below\n");
    Opts o;
    o.name = "s6";
    o.frames = 420;
    o.split = true;
    o.cam = [](int f) { return f * 3; };
    Sim s(o);
    s.run();
    Metrics &m = s.m();
    printf("    wrong %ld, hud lines max %d, first flagged %d, level lines wrongly hud %ld\n", total_wrong(m),
           m.hud_max, m.hud_frame, m.hud_on_level_lines);
    CHECK(total_wrong(m) == 0);
    CHECK(m.hud_max >= 24 && m.hud_max <= 32);
    CHECK(m.hud_on_level_lines == 0);
    CHECK(m.hud_frame >= 0 && m.hud_frame < 80);
    std::vector<uint8_t> t, c, p;
    int w, h;
    s.snapshot(t, c, p, w, h);
    // margins of the bar lines are the soft edge (purple in the provenance image)
    int soft = 0, tot = 0;
    for (int y = 4; y < 20; y++)
        for (int x = 0; x < 48; x++) {
            const uint8_t *px = &p[((size_t)y * w + x) * 3];
            tot++;
            if (px[0] == 150 && px[1] == 60 && px[2] == 200) soft++;
        }
    CHECK(soft * 100 >= tot * 95);
    shot(s, "s6_split_hud");
}

static void s7_page_loader()
{
    printf("S7  page loader: the page behind the camera is overwritten, the map keeps it\n");
    Opts o;
    o.name = "s7";
    o.frames = 420;
    o.engine = ENG_PAGE;
    o.cam = [](int f) { return f * 3; };
    Sim s(o);
    s.run();
    Metrics &m = s.m();
    printf("    wrong %ld, mean real L %.1f R %.1f (frames 100..420)\n", total_wrong(m), mean(m.real[0], 100, 420),
           mean(m.real[1], 100, 420));
    CHECK(total_wrong(m) == 0);
    CHECK(m.stale_gfx_drawn == 0);
    CHECK(mean(m.real[0], 100, 420) >= 40);
    shot(s, "s7_page_loader");
}

static void s8_sky_streams()
{
    printf("S8  the sky layer starts streaming new tiles at frame 400\n");
    Opts o;
    o.name = "s8";
    o.frames = 620;
    o.sky = o.mtn = true;
    o.sky_ring_w = 64;     // a streaming engine needs a ring wider than the view (>= 34 tiles)
    o.cam = [](int f) { return f * 3; };
    o.script = [](Sim &s, int f) {
        if (f == 400) s.set_sky_streaming(true);
    };
    Sim s(o);
    s.run();
    Metrics &m = s.m();
    int first_off = -1;
    for (int f = 400; f < 440; f++)
        if (m.kind[0][(size_t)f] != WK_KIND_BACKDROP) { first_off = f; break; }
    printf("    wrong %ld, sky leaves BACKDROP at frame %d (backdrop was held from %d)\n", total_wrong(m), first_off,
           m.back_frame[0]);
    CHECK(total_wrong(m) == 0);
    CHECK(m.back_frame[0] >= 0 && m.back_frame[0] < 400);
    CHECK(first_off >= 400 && first_off <= 405);
    shot(s, "s8_sky_streams");
}

/* ------------------------------------------------------------------------ */
/* profiles                                                                   */
/* ------------------------------------------------------------------------ */

static const char *kProfText =
    "# a comment\n"
    "[crc32=DEADBEEF title=\"Test Game One\"]\n"
    "enable = force\n"
    "bg1 = level\n"
    "bg2 = backdrop   # trailing comment\n"
    "bg3 = hud\n"
    "margin = 40\n"
    "lead_tiles = 3\n"
    "fade_px = 12\n"
    "sprites = off\n"
    "hud_lines = 0-23, 200-223\n"
    "levelmap = layer=0 addr=0x10 w=8 h=4 cell=16 bytes=1 endian=little stride=8 origin=0,0 meta=2x2 tiles=linear:0x40,4\n"
    "levelmap = layer=1 addr=0x40 w=4 h=2 cell=16 bytes=2 endian=big meta=2x2 "
    "tiles=table:1=0x10,0x11,0x12,0x13|2=0x20,0x21,0x22,0x23\n"
    "\n"
    "[title=\"Second Game\"]\n"
    "margin_left = 24\n"
    "margin_right = 8\n"
    "bogus_key = 3\n"
    "bg1 = nonsense\n"
    "levelmap = layer=0 addr=zz\n"
    "margin = 999\n"
    "[crc32=XYZ]\n"
    "enable = off\n"
    "[crc32=00000002 title=\"After Bad\"]\n"
    "enable = off\n";

static void t_profile_parser()
{
    printf("P1  profile parser\n");
    char msgs[2048];
    int np = -1;
    wk_profile_db *db = wk_profile_parse(kProfText, strlen(kProfText), msgs, sizeof msgs, &np);
    CHECK(db != NULL);
    if (!db) return;
    printf("    %d profiles, %d problems\n", wk_profile_count(db), np);
    CHECK(wk_profile_count(db) == 3);
    CHECK(np >= 5);
    const wk_profile *a = wk_profile_find(db, 0xDEADBEEF, NULL);
    CHECK(a != NULL);
    if (a) {
        CHECK(a->enable == WK_ENABLE_FORCE);
        CHECK(a->role[0] == WK_ROLE_LEVEL && a->role[1] == WK_ROLE_BACKDROP && a->role[2] == WK_ROLE_HUD);
        CHECK(a->role[3] == WK_ROLE_AUTO);
        CHECK(a->margin_left == 40 && a->margin_right == 40);
        CHECK(a->lead_tiles == 3 && a->fade_px == 12 && a->sprites_off == 1);
        CHECK(a->nhud == 2 && a->hud_lo[0] == 0 && a->hud_hi[0] == 23 && a->hud_lo[1] == 200 && a->hud_hi[1] == 223);
        CHECK(a->nlevelmaps == 2);
        // linear level map: cell value 5 at cell (3, 1) -> tiles 0x40 + 5*4 + i
        uint8_t ram[0x100];
        memset(ram, 0, sizeof ram);
        ram[0x10 + 1 * 8 + 3] = 5;
        uint32_t tile = 0;
        CHECK(wk_levelmap_tile(&a->levelmap[0], ram, sizeof ram, 3 * 16, 1 * 16, &tile) == 1);
        CHECK(tile == 0x40u + 20u);
        CHECK(wk_levelmap_tile(&a->levelmap[0], ram, sizeof ram, 3 * 16 + 8, 1 * 16, &tile) == 1);
        CHECK(tile == 0x40u + 21u);
        CHECK(wk_levelmap_tile(&a->levelmap[0], ram, sizeof ram, 3 * 16 + 8, 1 * 16 + 8, &tile) == 1);
        CHECK(tile == 0x40u + 23u);
        // out of the level and out of the RAM window are refused
        CHECK(wk_levelmap_tile(&a->levelmap[0], ram, sizeof ram, 8 * 16, 0, &tile) == 0);
        CHECK(wk_levelmap_tile(&a->levelmap[0], ram, sizeof ram, -16, 0, &tile) == 0);
        CHECK(wk_levelmap_tile(&a->levelmap[0], ram, 0x12, 3 * 16, 1 * 16, &tile) == 0);
        // table level map, 16-bit big endian cells at 0x40
        memset(ram, 0, sizeof ram);
        ram[0x40 + 2 * 1 + 0] = 0;
        ram[0x40 + 2 * 1 + 1] = 2;      // cell (1,0) = 2
        CHECK(wk_levelmap_tile(&a->levelmap[1], ram, sizeof ram, 16, 0, &tile) == 1);
        CHECK(tile == 0x20u);
        CHECK(wk_levelmap_tile(&a->levelmap[1], ram, sizeof ram, 16 + 8, 8, &tile) == 1);
        CHECK(tile == 0x23u);
        CHECK(wk_levelmap_tile(&a->levelmap[1], ram, sizeof ram, 0, 0, &tile) == 0);   // value 0 not in table
    }
    const wk_profile *b = wk_profile_find(db, 0x11111111, "SECOND game");
    CHECK(b != NULL);
    if (b) {
        CHECK(b->margin_left == 24 && b->margin_right == 8);
        CHECK(b->role[0] == WK_ROLE_AUTO);          // the bad role line was skipped
        CHECK(b->nlevelmaps == 0);                   // the bad levelmap line was skipped
    }
    CHECK(wk_profile_find(db, 0x22222222, "nothing") == NULL);
    const wk_profile *c = wk_profile_find(db, 2, NULL);
    CHECK(c != NULL && c->enable == WK_ENABLE_OFF);
    // a section after a bad header still parses and the bad section's keys did not leak
    if (c) CHECK(c->margin_left == -1);
    // the crc32 match wins over a title that belongs to another profile
    const wk_profile *d = wk_profile_find(db, 0xDEADBEEF, "Second Game");
    CHECK(d == a);
    wk_profile_free(db);

    // the starter file ships empty: 0 profiles, 0 problems
    FILE *f = fopen("profiles/wide_profiles.ini", "rb");
    if (!f) f = fopen("../profiles/wide_profiles.ini", "rb");
    CHECK(f != NULL);
    if (f) {
        std::vector<char> buf(1 << 16);
        size_t n = fread(buf.data(), 1, buf.size(), f);
        fclose(f);
        int np2 = -1;
        wk_profile_db *d2 = wk_profile_parse(buf.data(), n, NULL, 0, &np2);
        CHECK(d2 && wk_profile_count(d2) == 0 && np2 == 0);
        wk_profile_free(d2);
    }
    // garbage in, no crash, empty out
    std::vector<char> junk(4000);
    unsigned x = 12345;
    for (auto &ch : junk) { x = x * 1103515245u + 12345u; ch = (char)(x >> 16); }
    wk_profile_db *d3 = wk_profile_parse(junk.data(), junk.size(), NULL, 0, NULL);
    CHECK(d3 != NULL);
    wk_profile_free(d3);
    wk_profile_db *d4 = wk_profile_parse("", 0, NULL, 0, NULL);
    CHECK(d4 && wk_profile_count(d4) == 0);
    wk_profile_free(d4);
}

static void t_profile_effect()
{
    printf("P2  profile effects: forced roles and margin limits take effect\n");
    // forced backdrop role on the sky from the first frame (auto needs ~30 frames)
    const char *txt = "[crc32=000000AA]\nbg1 = backdrop\nbg2 = level\nmargin_right = 16\nmargin_left = 24\n";
    wk_profile_db *db = wk_profile_parse(txt, strlen(txt), NULL, 0, NULL);
    const wk_profile *p = wk_profile_find(db, 0xAA, NULL);
    CHECK(p != NULL);
    Opts o;
    o.name = "p2";
    o.frames = 200;
    o.sky = true;
    o.cam = [](int f) { return f * 3; };
    o.profile = p;
    Sim s(o);
    s.run();
    Metrics &m = s.m();
    printf("    wrong %ld, sky backdrop from frame %d, real L %.1f R %.1f\n", total_wrong(m), m.back_frame[0],
           mean(m.real[0], 60, 200), mean(m.real[1], 60, 200));
    CHECK(total_wrong(m) == 0);
    CHECK(m.back_frame[0] >= 0 && m.back_frame[0] <= 14);
    CHECK(mean(m.real[0], 60, 200) <= 24.01);
    CHECK(mean(m.real[1], 60, 200) <= 16.01);
    wk_profile_free(db);
}

/* ------------------------------------------------------------------------ */
/* persistence                                                                */
/* ------------------------------------------------------------------------ */

static std::string tmpdir()
{
    char tmpl[] = "/tmp/wk_test_XXXXXX";
    char *d = mkdtemp(tmpl);
    return d ? std::string(d) : std::string("/tmp");
}

static bool read_file(const std::string &p, std::vector<uint8_t> &buf)
{
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return false;
    buf.clear();
    uint8_t tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    fclose(f);
    return true;
}

static void t_persistence()
{
    printf("M1  persistence: session 2 starts with the level learned in session 1\n");
    std::string dir = tmpdir();
    char path[512];
    wk_map_path(dir.c_str(), 0xC0FFEE, path, sizeof path);

    wk_map_cfg mc;
    wk_map_cfg_default(&mc);
    wk_map *m1 = wk_map_create(&mc, 0xC0FFEE);
    {
        Opts o;
        o.name = "m1a";
        o.frames = 420;
        o.sky = o.mtn = true;
        o.shared_map = m1;
        o.cam = [](int f) { return f * 3; };
        Sim s(o);
        s.run();
        CHECK(total_wrong(s.m()) == 0);
    }
    wk_map_stats st1;
    wk_map_stats_get(m1, &st1);
    printf("    session 1: %d cells, %d rooms\n", st1.cells, st1.rooms);
    CHECK(st1.cells > 1000);
    CHECK(wk_map_save(m1, path) == 0);
    std::vector<uint8_t> file1;
    CHECK(read_file(path, file1) && file1.size() > 100 && file1.size() <= (size_t)mc.file_cap);
    CHECK(file1.size() >= 4 && memcmp(file1.data(), "WKM1", 4) == 0);
    printf("    file: %zu bytes\n", file1.size());

    // session 2: a different map object, loaded from disk
    wk_map *m2 = wk_map_create(&mc, 0xC0FFEE);
    CHECK(wk_map_load(m2, path) == 0);
    wk_map_stats st2;
    wk_map_stats_get(m2, &st2);
    CHECK(st2.cells == st1.cells && st2.rooms == st1.rooms);
    // wrong game: refused and empty
    wk_map *m3 = wk_map_create(&mc, 0xBAD);
    CHECK(wk_map_load(m3, path) < 0);
    wk_map_stats st3;
    wk_map_stats_get(m3, &st3);
    CHECK(st3.cells == 0 && st3.rooms == 0);
    wk_map_destroy(m3);
    {
        Opts o;
        o.name = "m1b";
        o.frames = 260;
        o.sky = o.mtn = true;
        o.shared_map = m2;
        o.cam = [](int f) { return f * 3; };
        Sim s(o);
        s.run();
        Metrics &m = s.m();
        wk_map_stats st;
        wk_map_stats_get(m2, &st);
        printf("    session 2: wrong %ld, mean real R %.1f (first pass; session 1 had %.1f), new_rooms %u\n",
               total_wrong(m), mean(m.real[1], 100, 260), 0.0, st.new_rooms);
        CHECK(total_wrong(m) == 0);
        CHECK(mean(m.real[1], 100, 260) >= 47.5);       // full lead on the very first pass
        CHECK(st.new_rooms == 0);
        shot(s, "m1_session2_full_lead");
    }
    // the map must not drift on the way out: load -> save is stable in size
    wk_map_destroy(m2);
    wk_map_destroy(m1);

    // atomic save: a failing save leaves the old file alone
    std::string blocker = dir + "/blocked.wkm";
    mkdir(blocker.c_str(), 0700);
    std::string inner = blocker + "/x";
    FILE *ff = fopen(inner.c_str(), "wb");
    if (ff) { fputs("x", ff); fclose(ff); }
    wk_map *m4 = wk_map_create(&mc, 0xC0FFEE);
    CHECK(wk_map_load(m4, path) == 0);
    CHECK(wk_map_save(m4, blocker.c_str()) != 0);      // a non-empty directory cannot be replaced
    struct stat sb;
    CHECK(stat(blocker.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode));
    CHECK(wk_map_save(m4, "/nonexistent_dir_wk/zzz") != 0);
    std::vector<uint8_t> file1b;
    CHECK(read_file(path, file1b) && file1b == file1);
    // a successful save leaves no temp file behind
    CHECK(wk_map_save(m4, path) == 0);
    int tmpfiles = 0;
    {
        std::string cmd = "ls " + dir + " | grep -c tmp > " + dir + "/n.txt";
        int rc = system(cmd.c_str());
        (void)rc;
        std::vector<uint8_t> nb;
        if (read_file(dir + "/n.txt", nb) && !nb.empty()) tmpfiles = atoi((const char *)nb.data());
    }
    CHECK(tmpfiles == 0);
    wk_map_destroy(m4);

    // corruption tolerance: every damaged variant is refused AND leaves a usable, empty map
    printf("M2  corruption tolerance\n");
    int refused = 0, variants = 0;
    auto try_variant = [&](const std::vector<uint8_t> &v, bool must_fail) {
        wk_map *mm = wk_map_create(&mc, 0xC0FFEE);
        int rc = wk_map_deserialize(mm, v.data(), v.size());
        wk_map_stats st;
        wk_map_stats_get(mm, &st);
        variants++;
        if (rc != 0) {
            refused++;
            CHECK(st.cells == 0 && st.rooms == 0);
        } else {
            CHECK(!must_fail);
        }
        // the map must still be usable after a refusal
        Opts o;
        o.name = "corrupt";
        o.frames = 60;
        o.shared_map = mm;
        o.cam = [](int f) { return f * 3; };
        Sim s(o);
        s.run();
        CHECK(total_wrong(s.m()) == 0);
        wk_map_destroy(mm);
    };
    unsigned x = 99;
    for (int i = 0; i < 24; i++) {                     // byte flips anywhere
        std::vector<uint8_t> v = file1;
        x = x * 1103515245u + 12345u;
        size_t pos = (x >> 8) % v.size();
        v[pos] ^= (uint8_t)(1u << (i % 8));
        try_variant(v, true);
    }
    for (size_t cut : {(size_t)0, (size_t)1, (size_t)7, (size_t)39, (size_t)40, (size_t)41, file1.size() / 2,
                       file1.size() - 1}) {            // truncations
        std::vector<uint8_t> v(file1.begin(), file1.begin() + (long)cut);
        try_variant(v, true);
    }
    {
        std::vector<uint8_t> v = file1;                 // trailing junk
        v.push_back(0);
        try_variant(v, true);
    }
    {
        std::vector<uint8_t> v(5000);                   // garbage
        for (auto &b : v) { x = x * 1103515245u + 12345u; b = (uint8_t)(x >> 16); }
        try_variant(v, true);
        memcpy(v.data(), "WKM1", 4);                    // garbage with the right magic
        try_variant(v, true);
    }
    printf("    %d of %d damaged files refused\n", refused, variants);
    CHECK(refused == variants);
    wk_map *ok = wk_map_create(&mc, 0xC0FFEE);
    CHECK(wk_map_deserialize(ok, file1.data(), file1.size()) == 0);
    wk_map_destroy(ok);

    std::string cmd = "rm -rf " + dir;
    int rc = system(cmd.c_str());
    (void)rc;
}

static void t_map_limits()
{
    printf("M3  size cap and LRU eviction\n");
    wk_map_cfg mc;
    wk_map_cfg_default(&mc);
    mc.max_cells = 6000;
    mc.max_rooms = 3;
    mc.file_cap = 4096;
    wk_map *m = wk_map_create(&mc, 5);
    // several different "levels" in a row, each a fresh scene
    int ncells_max = 0, rooms_max = 0;
    for (int k = 0; k < 6; k++) {
        Opts o;
        o.name = "lim";
        o.frames = 150;
        o.seed = 100 + (uint32_t)k * 17;
        o.shared_map = m;
        o.cam = [](int f) { return f * 4; };
        Sim s(o);
        s.run();
        CHECK(total_wrong(s.m()) == 0);
        wk_map_stats st;
        wk_map_stats_get(m, &st);
        ncells_max = std::max(ncells_max, st.cells);
        rooms_max = std::max(rooms_max, st.rooms);
    }
    printf("    peak cells %d (cap 6000), peak rooms %d (cap 3)\n", ncells_max, rooms_max);
    CHECK(ncells_max <= 6000);
    CHECK(rooms_max <= 3);
    std::vector<uint8_t> buf(wk_map_serialize_max(m));
    size_t n = wk_map_serialize(m, buf.data(), buf.size());
    CHECK(n > 0 && n <= (size_t)mc.file_cap);
    wk_map *m2 = wk_map_create(&mc, 5);
    CHECK(wk_map_deserialize(m2, buf.data(), n) == 0);
    wk_map_destroy(m2);
    wk_map_destroy(m);
}

/* ------------------------------------------------------------------------ */
/* compose                                                                    */
/* ------------------------------------------------------------------------ */

struct CompHarness {
    enum { V = 64, H = 32, M = 64 };
    wk_compose *c;
    wk_out ana;
    std::vector<uint16_t> centre, bg, out, cand[2];
    std::vector<uint8_t> prov, op;
    wk_compose_in in;
    int target[2];
    explicit CompHarness(const wk_compose_cfg *cfg)
    {
        c = wk_compose_create(cfg, V, H, M);
        memset(&ana, 0, sizeof ana);
        ana.nlines = H;
        ana.margin = M;
        centre.assign((size_t)V * H, 0);
        bg.assign(H, 0x2104);
        for (size_t i = 0; i < centre.size(); i++) centre[i] = (uint16_t)(0x1234 + (i * 2654435761u >> 20));
        for (int s = 0; s < 2; s++) cand[s].assign((size_t)M * H, 0xFFFF);
        op.assign((size_t)M * H, 1);
        out.assign((size_t)(V + 2 * M) * H, 0);
        prov.assign(out.size(), 0);
        set_real(0, 0);
    }
    ~CompHarness() { wk_compose_destroy(c); }
    void set_real(int l, int r)
    {
        target[0] = l;
        target[1] = r;
        for (int y = 0; y < H; y++) {
            wk_line_out &lo = ana.line[y];
            memset(&lo, 0, sizeof lo);
            for (int s = 0; s < 2; s++) lo.real[s][0] = lo.ring[s][0] = (uint16_t)target[s];
            lo.level_mask = 1;
            lo.level_real[0] = (uint16_t)l;
            lo.level_real[1] = (uint16_t)r;
        }
        ana.kind[0] = WK_KIND_LEVEL;
    }
    void frame()
    {
        memset(&in, 0, sizeof in);
        in.ana = &ana;
        in.centre = centre.data();
        in.centre_stride = V;
        in.bg = bg.data();
        for (int s = 0; s < 2; s++) {
            in.cand[0][s] = cand[s].data();
            in.opaque[0][s] = op.data();
        }
        in.norder = 1;
        in.order[0] = 0;
        in.out = out.data();
        in.out_stride = V + 2 * M;
        in.prov = prov.data();
        wk_compose_frame(c, &in);
    }
    int stride() const { return V + 2 * M; }
    // pixels of kind k on line y, side s (0 left)
    int count(int y, int s, int kind0, int kind1 = -1) const
    {
        int n = 0;
        for (int d = 0; d < M; d++) {
            int x = s == 0 ? M - 1 - d : M + V + d;
            int k = prov[(size_t)y * (size_t)stride() + (size_t)x] & 15;
            if (k == kind0 || k == kind1) n++;
        }
        return n;
    }
    int ext(int y, int s) const     // outermost distance covered by level / fade, plus one
    {
        for (int d = M - 1; d >= 0; d--) {
            int x = s == 0 ? M - 1 - d : M + V + d;
            int k = prov[(size_t)y * (size_t)stride() + (size_t)x] & 15;
            if (k == WK_PROV_LEVEL || k == WK_PROV_FADE) return d + 1;
        }
        return 0;
    }
};

static void t_compose()
{
    printf("C1  compose: hysteresis, ghost, HUD, fade\n");
    wk_compose_cfg cfg;
    wk_compose_cfg_default(&cfg);
    CHECK(cfg.smear_px == 0);
    {
        CompHarness h(&cfg);
        h.frame();
        CHECK(h.ext(5, 1) == 0);
        h.set_real(40, 40);
        int first_seen = -1, reached = -1, prev = 0;
        bool mono = true, over = false;
        for (int f = 0; f < 80; f++) {
            h.frame();
            int e = h.ext(5, 1);
            if (e > 0 && first_seen < 0) first_seen = f;
            if (e >= 37 && reached < 0) reached = f;   // the last px of a fade ramp has zero weight
            if (e < prev) mono = false;
            if (e > 40) over = true;
            prev = e;
        }
        printf("    appear after %d frames, full at %d\n", first_seen, reached);
        CHECK(first_seen >= cfg.appear_frames - 1);          // delayed, never instant
        CHECK(reached > first_seen && reached <= first_seen + 40 / cfg.grow_px + 2);
        CHECK(mono && !over);
        CHECK(h.ext(5, 0) >= 37 && h.ext(5, 0) <= 40 && h.ext(31, 1) >= 37 && h.ext(31, 1) <= 40);

        // shrink at once: never a wrong tile
        h.set_real(10, 10);
        h.frame();
        CHECK(h.ext(5, 1) <= 10);
        // ghost: a big shrink leaves a short fade, which is gone after ghost_frames
        CHECK(h.count(5, 1, WK_PROV_GHOST) > 0);
        for (int f = 0; f < cfg.ghost_frames + 2; f++) h.frame();
        CHECK(h.count(5, 1, WK_PROV_GHOST) == 0);
        CHECK(h.ext(5, 1) <= 10);
    }
    {   // flicker: wanted on alternate frames only -> never grows
        CompHarness h(&cfg);
        int maxe = 0;
        for (int f = 0; f < 60; f++) {
            h.set_real(f & 1 ? 40 : 0, f & 1 ? 40 : 0);
            h.frame();
            maxe = std::max(maxe, h.ext(7, 1));
        }
        CHECK(maxe == 0);
        // an on/off pattern with period 6 (shorter than appear_frames) also stays hidden
        int maxe2 = 0;
        for (int f = 0; f < 120; f++) {
            h.set_real(f % 6 < 3 ? 40 : 0, f % 6 < 3 ? 40 : 0);
            h.frame();
            maxe2 = std::max(maxe2, h.ext(7, 1));
        }
        CHECK(maxe2 == 0);
    }
    {   // fade ramp: outer pixels dissolve monotonically into what lies below
        wk_compose_cfg c2 = cfg;
        c2.fade_px = 20;
        CompHarness h(&c2);
        h.set_real(44, 44);
        for (int f = 0; f < 60; f++) h.frame();
        CHECK(h.ext(9, 1) >= 41 && h.ext(9, 1) <= 44);
        double prev = 1e9;
        bool monotone = true;
        double inner = 0, outer = 0;
        for (int d = 0; d < 44; d++) {
            double sum = 0;
            for (int y = 0; y < CompHarness::H; y++) {
                int x = CompHarness::M + CompHarness::V + d;
                uint16_t p = h.out[(size_t)y * (size_t)h.stride() + (size_t)x];
                sum += (p >> 5) & 63;
            }
            sum /= CompHarness::H;
            if (d >= 44 - 20 && sum > prev + 1.0) monotone = false;     // allow dither noise
            if (d >= 44 - 20) prev = std::min(prev, sum);
            if (d < 10) inner += sum / 10;
            if (d >= 40) outer += sum / 4;
        }
        printf("    colour of the run: inner %.1f, outer %.1f (of 63)\n", inner, outer);
        CHECK(monotone);
        CHECK(inner > 60);              // the candidate (white) shows fully where it is certain
        CHECK(outer < inner - 20);      // and has dissolved at the far end
        // STIPPLE mode yields the same extent
        c2.fade_mode = WK_FADE_STIPPLE;
        CompHarness hs(&c2);
        hs.set_real(44, 44);
        for (int f = 0; f < 60; f++) hs.frame();
        CHECK(hs.ext(9, 1) >= 34 && hs.ext(9, 1) <= 44);   // stipple dots thin out to nothing
    }
    {   // HUD lines: the centre is never touched, margins are the soft edge only
        CompHarness h(&cfg);
        for (int y = 0; y < 8; y++) {
            h.ana.line[y].hud = 1;
            h.ana.line[y].level_mask = 0;
        }
        h.set_real(0, 0);
        for (int y = 0; y < 8; y++) h.ana.line[y].hud = 1;
        for (int f = 0; f < 40; f++) h.frame();
        int diffs = 0, soft = 0, tot = 0;
        for (int y = 0; y < CompHarness::H; y++)
            for (int x = 0; x < CompHarness::V; x++)
                diffs += h.out[(size_t)y * (size_t)h.stride() + (size_t)(CompHarness::M + x)] !=
                         h.centre[(size_t)y * CompHarness::V + (size_t)x];
        for (int y = 0; y < 8; y++)
            for (int s = 0; s < 2; s++) {
                soft += h.count(y, s, WK_PROV_SOFT);
                tot += CompHarness::M;
            }
        CHECK(diffs == 0);
        CHECK(soft == tot);
    }
    {   // backdrop: full margin straight away; HUD marker lines stay soft
        CompHarness h(&cfg);
        for (int y = 0; y < CompHarness::H; y++) {
            h.ana.line[y].backdrop_mask = 1;
            h.ana.line[y].level_mask = 0;
            for (int s = 0; s < 2; s++) h.ana.line[y].real[s][0] = CompHarness::M;
        }
        h.ana.kind[0] = WK_KIND_BACKDROP;
        for (int f = 0; f < 90; f++) h.frame();
        int bd = 0;
        for (int y = 0; y < CompHarness::H; y++) bd += h.count(y, 0, WK_PROV_BACKDROP) + h.count(y, 1, WK_PROV_BACKDROP);
        printf("    backdrop px %d of %d\n", bd, 2 * CompHarness::M * CompHarness::H);
        CHECK(bd == 2 * CompHarness::M * CompHarness::H);
    }
}

static uint16_t ref_blend(uint16_t d, uint16_t s, int w, int dith)
{
    int r = 0;
    int shifts[3] = {11, 5, 0}, bits[3] = {5, 6, 5};
    for (int k = 0; k < 3; k++) {
        int mask = (1 << bits[k]) - 1;
        int dc = (d >> shifts[k]) & mask, sc = (s >> shifts[k]) & mask;
        int v = (sc * w + dc * (32 - w) + dith) >> 5;
        if (v > mask) v = mask;
        r |= v << shifts[k];
    }
    return (uint16_t)r;
}

static void t_blend()
{
    printf("C2  blend kernel: NEON path == scalar == plain formula (host: both are scalar)\n");
    unsigned x = 4242;
    auto rnd = [&]() { x = x * 1103515245u + 12345u; return x >> 8; };
    int bad = 0;
    for (int round = 0; round < 300; round++) {
        const int n = 1 + (int)(rnd() % 70);
        std::vector<uint16_t> a(80), b(80), a2;
        std::vector<uint8_t> w(80), dth(80);
        for (int i = 0; i < 80; i++) {
            a[(size_t)i] = (uint16_t)rnd();
            b[(size_t)i] = (uint16_t)rnd();
            w[(size_t)i] = (uint8_t)(rnd() % 33);
            dth[(size_t)i] = (uint8_t)(rnd() % 32);
        }
        a2 = a;
        std::vector<uint16_t> a3 = a;
        wk_blend_row(a.data(), b.data(), w.data(), dth.data(), n);
        wk_blend_row_scalar(a2.data(), b.data(), w.data(), dth.data(), n);
        for (int i = 0; i < 80; i++) {
            uint16_t want = i < n ? ref_blend(a3[(size_t)i], b[(size_t)i], w[(size_t)i], dth[(size_t)i]) : a3[(size_t)i];
            if (a[(size_t)i] != a2[(size_t)i] || a2[(size_t)i] != want) bad++;
        }
    }
    CHECK(bad == 0);
    printf("    neon compiled in: %s\n", wk_have_neon() ? "yes" : "no");
}

/* ------------------------------------------------------------------------ */


static void shot_named(Sim &s, const std::string &base)
{
    shot(s, base.c_str());
}

static void t_periodic()
{
    printf("D1  periodic continuation of flat terrain (checked pixel by pixel, counted separately)\n");
    {   // a flat, truly periodic ground strip: the continuation must be exact
        Opts o;
        o.name = "d1a";
        o.frames = 360;
        o.margin = 70;
        o.ground = 1;
        o.cam = [](int f) { return f * 3; };
        Sim s(o);
        for (int f = 0; f < o.frames; f++) {
            s.step();
            if (f == 300) shot(s, "d1a_periodic_ground_m70");
        }
        Metrics &m = s.m();
        printf("    flat ground: real-kind wrong %ld, predicted px %ld, predicted wrong %ld, bans %d, mean predicted/frame %.0f\n",
               total_wrong(m), m.period_claimed[2], m.period_wrong[2], s.period_bans(),
               [&]() { double a = 0; for (int f = 150; f < 360; f++) a += m.period_px[(size_t)f]; return a / 210; }());
        CHECK(total_wrong(m) == 0);
        CHECK(m.period_wrong[2] == 0);
        CHECK(m.period_claimed[2] > 100000);
        CHECK(s.period_bans() == 0);
        // the same scene with the feature off shows no predicted pixels at all
    }
    {
        Opts o;
        o.name = "d1off";
        o.frames = 200;
        o.margin = 70;
        o.ground = 1;
        o.period = 0;
        o.cam = [](int f) { return f * 3; };
        Sim s(o);
        s.run();
        CHECK(s.m().period_claimed[2] == 0);
        CHECK(total_wrong(s.m()) == 0);
    }
    {   // the ordinary random world: nothing periodic, so nothing may be invented
        Opts o;
        o.name = "d1b";
        o.frames = 320;
        o.margin = 70;
        o.cam = [](int f) { return f * 3; };
        Sim s(o);
        s.run();
        Metrics &m = s.m();
        printf("    random world: predicted px %ld, wrong %ld\n", m.period_claimed[2], m.period_wrong[2]);
        CHECK(total_wrong(m) == 0);
        CHECK(m.period_claimed[2] == 0);
    }
    {   // periodic ground with pits: the prediction is wrong over a pit until hindsight shows
        // it; then it must be off for a long time
        Opts o;
        o.name = "d1c";
        o.frames = 520;
        o.margin = 70;
        o.ground = 2;
        o.cam = [](int f) { return f * 3; };
        Sim s(o);
        for (int f = 0; f < o.frames; f++) {
            s.step();
            if (f == 200) shot(s, "d1c_periodic_pits_m70");
        }
        Metrics &m = s.m();
        const int F0 = m.first_period_wrong_frame;
        int later = 0;
        for (int f = F0 + 40; f < std::min(F0 + 250, (int)m.period_px.size()); f++) later += m.period_px[(size_t)f];
        printf("    ground with pits: real-kind wrong %ld, predicted px %ld, predicted wrong %ld in %ld frames (first %d), bans %d, predicted px 40..250 frames later: %d\n",
               total_wrong(m), m.period_claimed[2], m.period_wrong[2], m.period_wrong_frames, F0, s.period_bans(), later);
        CHECK(total_wrong(m) == 0);
        CHECK(F0 >= 0);                 // the pit is a genuine miss ...
        CHECK(s.period_bans() >= 1);    // ... hindsight sees it ...
        CHECK(later == 0);              // ... and the feature stays off afterwards
        CHECK(m.period_wrong_frames <= 40);
    }
}

static void t_real_margins()
{
    printf("R1  real margins (70 px per side for 256x224, 84 px for the 256x240 NES picture)\n");
    const int margins[2] = {70, 84};
    for (int mi = 0; mi < 2; mi++) {
        const int M = margins[mi];
        char tag[32];
        snprintf(tag, sizeof tag, "_m%d", M);
        {   // horizontal scroller, first and second pass
            Opts o;
            o.name = "r1";
            o.frames = 540;
            o.margin = M;
            o.sky = o.mtn = o.hud_layer = true;
            o.cam = [](int f) {
                if (f < 200) return f * 3;
                if (f < 210) return 600;
                if (f < 370) return 600 - (f - 210) * 3;
                return 120 + (f - 370) * 3;
            };
            Sim s(o);
            for (int f = 0; f < o.frames; f++) {
                s.step();
                if (f == 190) shot_named(s, std::string("r1_scroller_first_pass") + tag);
                if (f == 530) shot_named(s, std::string("r1_scroller_second_pass") + tag);
            }
            Metrics &m = s.m();
            printf("    M=%d scroller: wrong %ld, period wrong %ld, real R pass1 %.1f pass2 %.1f, stale %ld\n", M,
                   total_wrong(m), m.period_wrong[2], mean(m.real[1], 100, 200), mean(m.real[1], 400, 480),
                   m.stale_gfx_drawn);
            CHECK(total_wrong(m) == 0);
            CHECK(m.stale_gfx_drawn == 0);
            CHECK(mean(m.real[0], 100, 200) >= M - 0.5);                // history side is full
            CHECK(mean(m.real[1], 400, 480) >= M - 0.5);                // learned lead is full
            CHECK(mean(m.real[1], 400, 480) > mean(m.real[1], 100, 200) + 20);
        }
        {   // single screen
            Opts o;
            o.name = "r1s";
            o.frames = 120;
            o.margin = M;
            o.sky = true;
            o.cam = [](int) { return 0; };
            Sim s(o);
            s.run();
            shot_named(s, std::string("r1_single_screen") + tag);
            CHECK(total_wrong(s.m()) == 0);
            CHECK(s.m().claimed[0] == 0);
        }
        {   // rooms
            Opts o;
            o.name = "r1r";
            o.frames = 330;
            o.margin = M;
            o.engine = ENG_ROOMS;
            o.sky = true;
            o.script = [](Sim &s, int f) {
                if (f == 100) { s.load_room(1); s.flip_to_room(1); }
                if (f == 200) { s.load_room(2); s.flip_to_room(2); }
            };
            Sim s(o);
            for (int f = 0; f < o.frames; f++) {
                s.step();
                if (f == 150) shot_named(s, std::string("r1_rooms") + tag);
            }
            CHECK(total_wrong(s.m()) == 0);
            CHECK(s.m().claimed[2] == 0);
        }
    }
}

struct TestEntry { const char *name; void (*fn)(); };

static void run_test(const TestEntry &t)
{
    int f0 = g_fail, c0 = g_checks;
    t.fn();
    printf("=== %-22s %s  (%d checks, %d failed)\n", t.name, g_fail == f0 ? "PASS" : "FAIL", g_checks - c0, g_fail - f0);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_shots = getenv("WK_SHOTS");
    if (g_shots && !*g_shots) g_shots = NULL;
    if (g_shots) {
        std::string cmd = std::string("mkdir -p '") + g_shots + "'";
        int rc = system(cmd.c_str());
        (void)rc;
    }
    static const TestEntry tests[] = {
        {"blend", t_blend}, {"compose", t_compose}, {"profile_parser", t_profile_parser},
        {"s2_single_screen", s2_single_screen}, {"s5b_gfx_gate", s5b_margin_cell_gfx_gate},
        {"s6_split_hud", s6_split_hud}, {"s3_rooms", s3_rooms}, {"s4_stale_lead", s4_stale_lead},
        {"s7_page_loader", s7_page_loader}, {"s8_sky_streams", s8_sky_streams},
        {"s5_tileset_swap", s5_tileset_swap}, {"profile_effect", t_profile_effect},
        {"map_limits", t_map_limits}, {"persistence", t_persistence}, {"periodic", t_periodic},
        {"real_margins", t_real_margins}, {"s1_scroller", s1_scroller},
    };
    for (const TestEntry &t : tests) {
        bool want = argc < 2;
        for (int i = 1; i < argc; i++)
            if (!strcmp(argv[i], t.name)) want = true;
        if (want) run_test(t);
    }
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
