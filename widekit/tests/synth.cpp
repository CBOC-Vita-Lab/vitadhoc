/* widekit tests - MIT licence.  See synth.h. */

#include "synth.h"
#include <stdio.h>
#include <stdlib.h>
#include "png.h"

#include <limits.h>
#include <math.h>
#include <string.h>
#include <time.h>

namespace synth {

static double now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);   // CPU time: the host is shared and loaded
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static uint32_t h2(uint32_t a, uint32_t b)
{
    a ^= b * 0x9E3779B1u;
    a = (a << 13) | (a >> 19);
    a *= 0x85EBCA6Bu;
    a ^= a >> 15;
    a *= 0xC2B2AE35u;
    return a ^ (a >> 16);
}
static int fdiv(int a, int b)
{
    int q = a / b;
    if ((a % b) && ((a < 0) != (b < 0))) q--;
    return q;
}
static int fmodp(int a, int b)
{
    int r = a % b;
    if (r && ((r < 0) != (b < 0))) r += b;
    return r;
}

uint32_t fold16(uint32_t v) { return (v ^ (v >> 16)) & 0xFFFF; }

static uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Colours for the 8 palettes (3 shades each), kept game-like so the
// screenshots can be judged by eye.  Tileset B shifts the hues.
static const uint8_t kPal[8][3][3] = {
    {{210, 60, 70}, {250, 120, 120}, {255, 220, 90}},      // 0 flowers
    {{140, 90, 40}, {190, 130, 60}, {90, 55, 25}},         // 1 planks
    {{70, 160, 60}, {120, 200, 80}, {110, 70, 40}},        // 2 ground
    {{170, 70, 60}, {210, 110, 90}, {80, 40, 40}},         // 3 bricks
    {{235, 240, 250}, {200, 215, 240}, {165, 185, 225}},   // 4 clouds
    {{70, 90, 140}, {100, 125, 175}, {45, 60, 105}},       // 5 rock / mountains
    {{250, 220, 60}, {255, 255, 255}, {60, 60, 80}},       // 6 hud
    {{120, 120, 120}, {180, 180, 180}, {70, 70, 70}}};     // 7 grey

void make_tileset(Tileset &ts, uint32_t seed)
{
    const bool alt = (seed & 1024) != 0 || seed >= 1000;
    ts.id = h2(seed, 0x7154);
    for (int p = 0; p < 8; p++) {
        ts.pal[p][0] = 0;
        for (int k = 0; k < 3; k++) {
            int r = kPal[p][k][0], g = kPal[p][k][1], b = kPal[p][k][2];
            if (alt) { int t = r; r = b; b = g; g = t; }
            ts.pal[p][k + 1] = rgb565(r, g, b);
        }
    }
    for (int id = 0; id < 256; id++) {
        uint32_t gh = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                uint32_t h = h2(seed ^ (uint32_t)(id * 977), (uint32_t)(y * 8 + x));
                int v = 0;
                if (id == 0) v = 0;
                else if (id < 10) {                      // flower: stem + head
                    if (x == 3 || x == 4) v = (y > 3) ? 2 : 0;
                    if (y >= 1 && y <= 3 && x >= 2 && x <= 5) v = (x == 3 || x == 4) && y == 2 ? 3 : 1;
                } else if (id < 50) {                    // cloud blob
                    int cx = 3 + (id % 3), cy = 4;
                    int dx = x - cx, dy = (y - cy) * 2;
                    int dd = dx * dx + dy * dy;
                    v = dd < 12 ? 1 : (dd < 20 ? 2 : (dd < 28 && ((x + y) & 1) ? 3 : 0));
                } else if (id < 100) {                   // bricks
                    int off = (y / 4) & 1 ? 4 : 0;
                    bool mortar = (y % 4 == 3) || ((x + off) % 8 == 7);
                    v = mortar ? 3 : (1 + ((x + off) / 4 + y / 4) % 2);
                } else if (id < 150) {                   // planks
                    v = (y % 4 == 3) ? 3 : 1 + (y / 4) % 2;
                    if ((x + id) % 8 == 0 && y % 4 == 1) v = 3;
                } else if (id < 170) {                   // ground: grass cap
                    v = y < 2 ? 2 : (y == 2 ? 1 : 3);
                    if (y >= 3 && (h & 7) == 0) v = 1;
                } else if (id < 200) {                   // earth
                    v = 3;
                    if ((h & 7) < 2) v = 1;
                } else if (id < 220) {                   // hud glyph
                    v = ((x ^ y) & 1) || (x > 5) ? 0 : 1;
                    if (y == 0 || y == 7) v = 2;
                } else {                                 // rock
                    v = 1 + ((y * 2 + (x >> 2)) / 5) % 2;
                    if (((x * 3 + y * 5 + id) % 11) == 0) v = 3;
                }
                // every id gets its own marker pixel so a wrong tile can never
                // look identical to the right one
                if (id != 0 && x == id % 8 && y == (id / 8) % 8) v = v == 3 ? 2 : 3;
                if (id != 0 && x == (id * 5 + 3) % 8 && y == (id / 4 + 5) % 8 && v) v = 1 + (v % 3);
                ts.px[id][y * 8 + x] = (uint8_t)v;
                gh = h2(gh, (uint32_t)v);
            }
        ts.gh[id] = gh;
    }
}

Sim::Sim(const Opts &o) : o_(o)
{
    make_tileset(ts_[0], o.seed);
    make_tileset(ts_[1], o.seed + 1000);
    int fgw = o.fg_ring_w;
    if (o.fg) fg_.init(fgw, 28);
    sky_.init(o.sky_ring_w, 28);
    mtn_.init(64, 28);
    hud_.init(32, 28);
    nl_ = 0;
    if (o.sky) idx_[0] = nl_++;
    if (o.mtn) idx_[1] = nl_++;
    if (o.fg) idx_[2] = nl_++;
    if (o.hud_layer) idx_[3] = nl_++;
    layers_ = wk_layers_create(NULL);
    if (o.profile) wk_layers_set_profile(layers_, o.profile);
    if (o.shared_map) {
        map_ = o.shared_map;
    } else if (o.use_map) {
        wk_map_cfg mc;
        wk_map_cfg_default(&mc);
        map_ = wk_map_create(&mc, 0xC0FFEE);
        own_map_ = true;
    }
    wk_compose_cfg cc;
    wk_compose_cfg_default(&cc);
    cc.fade_mode = o.fade_mode;
    cc.fade_px = o.fade_px;
    cc.smear_px = o.smear_px;
    cc.period = o.period;
    if (o.soft_blur >= 0) cc.soft_blur = o.soft_blur;
    if (o.soft_sample >= 0) cc.soft_sample = o.soft_sample;
    comp_ = wk_compose_create(&cc, VW, VH, o.margin);
    centre_.assign((size_t)VW * VH, 0);
    const int Wd = VW + 2 * o.margin;
    wide_.assign((size_t)Wd * VH, 0);
    prov_.assign((size_t)Wd * VH, 0);
    bg_.resize(VH);
    for (int y = 0; y < VH; y++) bg_[(size_t)y] = bg_colour(y);
    for (int l = 0; l < 4; l++)
        for (int s = 0; s < 2; s++) {
            inner_[l][s].assign((size_t)WK_MAX_INNER * VH, 0);
            innerop_[l][s].assign((size_t)WK_MAX_INNER * VH, 0);
            cand_[l][s].assign((size_t)o.margin * VH, 0);
            op_[l][s].assign((size_t)o.margin * VH, 0);
        }
    load_frame_ = o.blank_frames > 3 ? o.blank_frames - 3 : 0;
    m_.shown[0].reserve((size_t)o.frames); m_.shown[1].reserve((size_t)o.frames);
    memset(&out_, 0, sizeof out_);
    memset(&fr_, 0, sizeof fr_);
}

Sim::~Sim()
{
    wk_compose_destroy(comp_);
    wk_layers_destroy(layers_);
    if (own_map_) wk_map_destroy(map_);
}

uint16_t Sim::bg_colour(int y) const { return rgb565(30 + y / 6, 80 + y / 3, 190); }

/* ------------------------------ world --------------------------------------- */

uint32_t Sim::cell_code_world(int c, int r) const
{
    if (c < 0 || c >= o_.wcols) return 0;
    int hud_rows = o_.split ? 3 : 0;
    if (r < hud_rows) return 0;
    if (o_.ground) {
        bool pit = o_.ground == 2 && c > 12 && (c % 40) >= 20 && (c % 40) <= 22;
        if (pit) return 0;
        if (r == 25) return (150 + (uint32_t)(c % 2)) | (2u << 8);
        if (r > 25) return (170 + (uint32_t)(c % 2) + 2u * (uint32_t)(r % 3)) | (2u << 8);
        return 0;
    }
    uint32_t h = h2((uint32_t)c, (uint32_t)r + o_.seed * 100);
    bool pit = (c > 12) && (c % 23 >= 7 && c % 23 <= 9);
    if (r == 25 && !pit) return (150 + h % 20) | (2u << 8);
    if (r > 25 && !pit) return (170 + h % 30) | (2u << 8);
    if (h2((uint32_t)(c / 6), 77 + o_.seed) % 5 == 0 && r == 18) return (100 + h % 40) | (1u << 8);
    if (h2((uint32_t)(c / 9), 99 + o_.seed) % 4 == 0) {
        int tc = c / 9 * 9 + 4;
        int ht = 3 + (int)(h2((uint32_t)tc, 5) % 6);
        if (c == tc && r >= 24 - ht && r <= 24) return (50 + h % 50) | (3u << 8);
    }
    if (h % 97 == 0 && r >= 15 && r <= 23) return (1 + h % 9) | (0u << 8);
    return 0;
}

static uint32_t room_code(int k, int c, int r, uint32_t seed)
{
    uint32_t h = h2((uint32_t)(k * 64 + c), (uint32_t)r + seed * 100 + 5000);
    if (r >= 24) return (150 + h % 30) | (2u << 8);
    if (h % 7 == 0) return (50 + h % 50) | (3u << 8);
    return 0;
}

uint32_t Sim::truth_fg(int wcol, int row) const
{
    if (o_.engine == ENG_ROOMS) {
        int c = wcol - 32 * room_;
        if (c < 0 || c >= 32) return 0xFFFFFFFFu;
        return room_code(room_, c, row, o_.seed);
    }
    return cell_code_world(wcol, row);
}

uint32_t Sim::code_sky(int c, int r) const
{
    if (r >= 2 && r <= 5 && ((c * 3 + r * 5) % 8) < 3)
        return (uint32_t)(10 + (c % 8) + (r % 4) * 8) | (4u << 8);
    return 0;
}

static uint32_t code_sky_stream(int wc, int r, uint32_t seed)
{
    uint32_t h = h2((uint32_t)wc, (uint32_t)r + seed + 31);
    if (r >= 2 && r <= 9 && h % 3 == 0) return (10 + h % 40) | (4u << 8);
    return 0;
}

uint32_t Sim::code_mtn(int c, int r) const
{
    double a = 2 * M_PI * c / 64.0;
    int hgt = (int)(7 + 4 * sin(a) + 3 * sin(3 * a + 1.0));
    if (r >= 24 - hgt && r <= 27) return (uint32_t)(220 + h2((uint32_t)c, (uint32_t)r) % 36) | (5u << 8);
    return 0;
}

uint32_t Sim::code_hud(int c, int r) const
{
    if (r <= 2 && c >= 1 && c <= 30) return (200 + h2((uint32_t)c, (uint32_t)r) % 20) | (6u << 8);
    return 0;
}

void Sim::fill_ring_hashes(Ring &r)
{
    const Tileset &t = ts_[cur_ts_];
    for (size_t i = 0; i < r.code.size(); i++) {
        uint32_t c = r.code[i];
        r.hash[i] = h2(c, t.gh[c & 255] + ((c >> 8) & 7) * 0x10001u);
    }
}

void Sim::write_fg_col(int wcol)
{
    int slot = fmodp(wcol, fg_.w);
    int hud_rows = o_.split ? 3 : 0;
    const Tileset &t = ts_[cur_ts_];
    for (int r = hud_rows; r < 28; r++) {
        uint32_t code = o_.engine == ENG_ROOMS ? 0 : cell_code_world(wcol, r);
        size_t i = (size_t)r * (size_t)fg_.w + (size_t)slot;
        fg_.code[i] = code;
        fg_.hash[i] = h2(code, t.gh[code & 255] + ((code >> 8) & 7) * 0x10001u);
        fg_.stamp[i] = (uint32_t)f_ + 1000;
    }
}

void Sim::write_page(int page)
{
    for (int c = page * 32; c < page * 32 + 32; c++) write_fg_col(c);
}

void Sim::load_room(int k)
{
    int half = k % 2;
    const Tileset &t = ts_[cur_ts_];
    for (int c = 0; c < 32; c++)
        for (int r = 0; r < 28; r++) {
            uint32_t code = room_code(k, c, r, o_.seed);
            size_t i = (size_t)r * (size_t)fg_.w + (size_t)(half * 32 + c);
            fg_.code[i] = code;
            fg_.hash[i] = h2(code, t.gh[code & 255] + ((code >> 8) & 7) * 0x10001u);
            fg_.stamp[i] = (uint32_t)f_ + 1000;
        }
}

void Sim::flip_to_room(int k)
{
    room_ = k;
}

void Sim::swap_tileset()
{
    cur_ts_ ^= 1;
    fill_ring_hashes(fg_);
    fill_ring_hashes(sky_);
    fill_ring_hashes(mtn_);
    fill_ring_hashes(hud_);
}

void Sim::load_level()
{
    loaded_ = true;
    const uint32_t st = (uint32_t)f_ + 1000;
    const Tileset &t = ts_[cur_ts_];
    if (o_.fg) {
        if (o_.decoy)
            for (size_t i = 0; i < fg_.code.size(); i++) {
                uint32_t code = (50 + h2((uint32_t)i, 99) % 149) | ((h2((uint32_t)i, 98) % 4) << 8);
                fg_.code[i] = code;
                fg_.hash[i] = h2(code, t.gh[code & 255] + ((code >> 8) & 7) * 0x10001u);
                fg_.stamp[i] = 0;
            }
        int c0 = fdiv(camx_, 8);
        if (o_.engine == ENG_STREAM) {
            lo_ = c0;
            hi_ = c0 - 1;
            while (hi_ < c0 + 32 + o_.lead_r) { hi_++; write_fg_col(hi_); }
        } else if (o_.engine == ENG_PAGE) {
            int page = fdiv(camx_, 256);
            write_page(page);
            write_page(page + 1);
            last_page_ = page;
        } else {
            room_ = 0;
            load_room(0);
        }
        if (o_.split) {
            for (int c = 0; c < fg_.w; c++)
                for (int r = 0; r < 3; r++) {
                    uint32_t code = code_hud(c % 32, r);
                    size_t i = (size_t)r * (size_t)fg_.w + (size_t)c;
                    fg_.code[i] = code;
                    fg_.hash[i] = h2(code, t.gh[code & 255] + ((code >> 8) & 7) * 0x10001u);
                    fg_.stamp[i] = st;
                }
        }
    }
    for (int c = 0; c < sky_.w; c++)
        for (int r = 0; r < 28; r++) {
            size_t i = (size_t)r * (size_t)sky_.w + (size_t)c;
            sky_.code[i] = code_sky(c, r);
            sky_.stamp[i] = st;
        }
    for (int c = 0; c < 32; c++)
        for (int r = 0; r < 28; r++) {
            size_t i = (size_t)r * 32 + (size_t)c;
            hud_.code[i] = code_hud(c, r);
            hud_.stamp[i] = st;
        }
    for (int c = 0; c < 64; c++)
        for (int r = 0; r < 28; r++) {
            size_t i = (size_t)r * 64 + (size_t)c;
            mtn_.code[i] = code_mtn(c, r);
            mtn_.stamp[i] = st;
        }
    fill_ring_hashes(sky_);
    fill_ring_hashes(mtn_);
    fill_ring_hashes(hud_);
}

void Sim::engine_update()
{
    if (o_.fg && o_.engine == ENG_STREAM) {
        if (camx_ > last_camx_) {
            int want = fdiv(camx_ + VW - 1, 8) + o_.lead_r;
            while (hi_ < want) {
                hi_++;
                write_fg_col(hi_);
                if (hi_ - lo_ + 1 > fg_.w) lo_ = hi_ - fg_.w + 1;
            }
        } else if (camx_ < last_camx_) {
            int want = fdiv(camx_, 8) - o_.lead_l;
            while (lo_ > want) {
                lo_--;
                write_fg_col(lo_);
                if (hi_ - lo_ + 1 > fg_.w) hi_ = lo_ + fg_.w - 1;
            }
        }
    } else if (o_.fg && o_.engine == ENG_PAGE) {
        int page = fdiv(camx_, 256);
        if (page != last_page_) {
            write_page(page + 1);
            last_page_ = page;
        }
    }
    if (sky_stream_ && o_.sky) {
        const Tileset &t = ts_[cur_ts_];
        int sc = camx_ / 4;
        auto wcol = [&](int wc) {
            int slot = fmodp(wc, sky_.w);
            for (int r = 0; r < 28; r++) {
                uint32_t code = code_sky_stream(wc, r, o_.seed);
                size_t i = (size_t)r * (size_t)sky_.w + (size_t)slot;
                sky_.code[i] = code;
                sky_.hash[i] = h2(code, t.gh[code & 255] + ((code >> 8) & 7) * 0x10001u);
                sky_.stamp[i] = (uint32_t)f_ + 1000;
            }
        };
        if (sky_sw_col_ == INT_MIN) {
            int c0 = fdiv(sc, 8);
            sky_sw_col_ = c0;
            sky_hi_ = c0 - 1;
            while (sky_hi_ < c0 + 33) { sky_hi_++; wcol(sky_hi_); }
        } else {
            int want = fdiv(sc + VW - 1, 8) + 2;
            while (sky_hi_ < want) { sky_hi_++; wcol(sky_hi_); }
        }
    }
    last_camx_ = camx_;
}

/* ------------------------------ drawing ------------------------------------- */

Px Sim::tile_px(uint32_t code, int px, int py) const
{
    Px p;
    if (code == 0xFFFFFFFFu) { p.rgb = 0xF81F; p.op = 1; return p; }
    const Tileset &t = ts_[cur_ts_];
    int id = (int)(code & 255), pal = (int)((code >> 8) & 7);
    int v = t.px[id][py * 8 + px];
    p.op = v != 0;
    p.rgb = v ? t.pal[pal][v] : 0;
    return p;
}

Px Sim::ring_px(const Ring &r, int xl, int yrow, int line_y) const
{
    (void)line_y;
    int col = fmodp(fdiv(xl, 8), r.w), row = fmodp(fdiv(yrow, 8), r.h);
    return tile_px(r.code[(size_t)row * (size_t)r.w + (size_t)col], fmodp(xl, 8), fmodp(yrow, 8));
}

void Sim::render_centre()
{
    for (int y = 0; y < VH; y++)
        for (int x = 0; x < VW; x++) {
            uint16_t c = bg_[(size_t)y];
            for (int slot = 0; slot < 4; slot++) {
                if (idx_[slot] < 0) continue;
                const Ring &r = slot == 0 ? sky_ : slot == 1 ? mtn_ : slot == 2 ? fg_ : hud_;
                Px p = ring_px(r, scroll_[slot][y] + x, y, y);
                if (p.op) c = p.rgb;
            }
            centre_[(size_t)y * VW + (size_t)x] = c;
        }
}

void Sim::build_frame()
{
    int bright = 255;
    if (bright_override_ >= 0) bright = bright_override_;
    else if (f_ < o_.blank_frames) bright = 0;
    else if (f_ < o_.blank_frames + 8) bright = (f_ - o_.blank_frames + 1) * 32;
    if (bright > 255) bright = 255;
    memset(&fr_, 0, sizeof fr_);
    fr_.frame = (uint32_t)f_ + 1000;
    fr_.view_w = VW;
    fr_.view_h = VH;
    fr_.margin = o_.margin;
    fr_.brightness = bright;
    fr_.palette_size = 256;
    fr_.nlayers = nl_;
    for (int slot = 0; slot < 4; slot++) {
        if (idx_[slot] < 0) continue;
        wk_layer &L = fr_.layer[idx_[slot]];
        const Ring &r = slot == 0 ? sky_ : slot == 1 ? mtn_ : slot == 2 ? fg_ : hud_;
        L.active = 1;
        L.role = WK_ROLE_AUTO;
        L.priority = slot;
        L.map_w = r.w;
        L.map_h = r.h;
        L.tile_px = 8;
        L.hash = r.hash.data();
        L.code = r.code.data();
        L.stamp = (slot == 2 && !o_.fg_stamps) ? NULL : r.stamp.data();
        L.gfx_id = ts_[cur_ts_].id;
        int sc = 0;
        if (slot == 0) sc = camx_ / 4;
        else if (slot == 1) sc = camx_ / 2;
        else if (slot == 2) sc = camx_;
        for (int y = 0; y < VH; y++) {
            int s = sc;
            if (slot == 2 && o_.split && y < 24) s = 0;
            if (slot == 3) s = 0;
            scroll_[slot][y] = s;
            L.scroll_x[y] = fmodp(s, r.w * 8);
            L.row_y[y] = y;
        }
    }
}

void Sim::render_candidates()
{
    const int M = o_.margin;
    for (int slot = 0; slot < 3; slot++) {
        if (idx_[slot] < 0) continue;
        const int l = idx_[slot];
        const Ring &r = slot == 0 ? sky_ : slot == 1 ? mtn_ : fg_;
        for (int s = 0; s < 2; s++)
            for (int y = 0; y < VH; y++) {
                const wk_line_out &lo = out_.line[y];
                wk_geom g;
                bool have_geom = out_.coherent[l][y] != 0;
                if (have_geom) wk_line_geom(&fr_, &out_, l, y, &g);
                for (int d = 0; d < M; d++) {
                    int xl = s == 0 ? scroll_[slot][y] - 1 - d : scroll_[slot][y] + VW + d;
                    Px p = ring_px(r, xl, y, y);
                    if (have_geom && d >= lo.ring[s][l] && d < lo.real[s][l]) {
                        int wx = out_.world_x[l][y];
                        int idx = s == 0 ? g.tile_first - fdiv(wx - 1 - d, g.tp)
                                         : fdiv(wx + VW + d, g.tp) - (g.tile_first + g.nvis - 1);
                        wk_cell cell;
                        if (wk_map_margin_cell(map_, &fr_, &out_, l, y, s, idx, &cell)) {
                            if (cell.gfx != fold16(ts_[cur_ts_].id)) m_.stale_gfx_drawn++;
                            int xw = s == 0 ? wx - 1 - d : wx + VW + d;
                            p = tile_px(cell.code, fmodp(xw, 8), y % 8);
                        }
                    }
                    cand_[slot][s][(size_t)y * (size_t)M + (size_t)d] = p.rgb;
                    op_[slot][s][(size_t)y * (size_t)M + (size_t)d] = p.op;
                }
                for (int e = 0; e < WK_MAX_INNER; e++) {
                    int xl = s == 0 ? scroll_[slot][y] + e : scroll_[slot][y] + VW - 1 - e;
                    Px p = ring_px(r, xl, y, y);
                    inner_[slot][s][(size_t)y * WK_MAX_INNER + (size_t)e] = p.rgb;
                    innerop_[slot][s][(size_t)y * WK_MAX_INNER + (size_t)e] = p.op;
                }
            }
    }
}

Px Sim::truth_px(int slot, int side, int y, int d) const
{
    int x;
    uint32_t code;
    if (slot == 0) {
        x = camx_ / 4 + (side == 0 ? -1 - d : VW + d);
        if (sky_stream_ && fdiv(x, 8) >= sky_sw_col_)
            code = code_sky_stream(fdiv(x, 8), y / 8, o_.seed);
        else
            code = code_sky(fmodp(fdiv(x, 8), 32), y / 8);
    } else if (slot == 1) {
        x = camx_ / 2 + (side == 0 ? -1 - d : VW + d);
        code = code_mtn(fmodp(fdiv(x, 8), 64), y / 8);
    } else {
        int wcam = o_.engine == ENG_ROOMS ? room_ * 256 : camx_;
        x = wcam + (side == 0 ? -1 - d : VW + d);
        code = truth_fg(fdiv(x, 8), y / 8);
    }
    return tile_px(code, fmodp(x, 8), y % 8);
}

void Sim::verify()
{
    const int M = o_.margin, Wd = VW + 2 * M;
    long soft = 0, tot = 0;
    int per_px = 0, per_bad = 0;
    for (int y = 0; y < VH; y++)
        for (int s = 0; s < 2; s++)
            for (int d = 0; d < M; d++) {
                int xs = s == 0 ? M - 1 - d : M + VW + d;
                uint8_t pv = prov_[(size_t)y * (size_t)Wd + (size_t)xs];
                int kind = pv & 15, layer = pv >> 4;
                tot++;
                if (kind == WK_PROV_SOFT) soft++;
                if (kind == WK_PROV_PERIOD) {
                    int sl = -1;
                    for (int k = 0; k < 4; k++)
                        if (idx_[k] == layer) sl = k;
                    if (sl < 0 || sl > 2) { m_.wrong[3]++; continue; }
                    m_.period_claimed[sl]++;
                    per_px++;
                    Px t = truth_px(sl, s, y, d);
                    uint16_t got = wide_[(size_t)y * (size_t)Wd + (size_t)xs];
                    if (!t.op || t.rgb != got) {
                        m_.period_wrong[sl]++;
                        per_bad++;
                    }
                    continue;
                }
                if (kind != WK_PROV_BACKDROP && kind != WK_PROV_LEVEL && kind != WK_PROV_FADE)
                    continue;
                int slot = -1;
                for (int k = 0; k < 4; k++)
                    if (idx_[k] == layer) slot = k;
                if (slot < 0 || slot > 2) { m_.wrong[3]++; continue; }
                m_.claimed[slot]++;
                Px t = truth_px(slot, s, y, d);
                size_t ci = (size_t)y * (size_t)M + (size_t)d;
                bool op = op_[slot][s][ci] != 0;
                bool bad = (t.op != 0) != op || (op && t.rgb != cand_[slot][s][ci]);
                if (slot == 2 && o_.split && y < 24) bad = true;
                if (bad) {
                    if (getenv("WK_DBG") && m_.wrong[slot] < 40 && (m_.wrong[slot] % 7) == 0)
                        fprintf(stderr, "WRONG f=%d slot=%d side=%d y=%d d=%d kind=%d real=%d ring=%d cand=%04x/%d truth=%04x/%d ana=%d\n", f_, slot, s, y, d, kind,
                                out_.line[y].real[s][idx_[slot]], out_.line[y].ring[s][idx_[slot]], cand_[slot][s][ci], (int)op,
                                t.rgb, (int)t.op, (int)out_.coherent[idx_[slot]][y]);
                    m_.wrong[slot]++;
                    if (m_.first_wrong_frame < 0) m_.first_wrong_frame = f_;
                }
            }
    m_.soft_px += soft;
    m_.margin_px += tot;
    m_.period_px.push_back(per_px);
    m_.period_bad.push_back(per_bad);
    if (per_bad) {
        m_.period_wrong_frames++;
        if (m_.first_period_wrong_frame < 0) m_.first_period_wrong_frame = f_;
    }
}

void Sim::step()
{
    if (o_.script) o_.script(*this, f_);
    if (o_.engine == ENG_ROOMS) camx_ = (room_ % 2) * 256;
    else camx_ = o_.cam ? o_.cam(f_) : 0;
    if (camx_ < 0) camx_ = 0;
    if (!loaded_ && f_ >= load_frame_) {
        last_camx_ = camx_;
        load_level();
    }
    if (loaded_) engine_update();
    build_frame();
    double t0 = now_us();
    if (!o_.skip_lib) wk_layers_update(layers_, &fr_, &out_);
    double t1 = now_us();
    if (map_ && !o_.skip_lib) {
        wk_map_update(map_, &fr_, &out_);
        wk_map_serve(map_, &fr_, &out_);
    }
    double t2 = now_us();
    const bool draw = o_.render_every <= 0 || f_ % o_.render_every == 0;
    if (draw) {
        render_centre();
        render_candidates();
    }

    wk_compose_in ci;
    memset(&ci, 0, sizeof ci);
    ci.ana = &out_;
    ci.centre = centre_.data();
    ci.centre_stride = VW;
    ci.bg = bg_.data();
    for (int slot = 0; slot < 3; slot++) {
        if (idx_[slot] < 0) continue;
        int l = idx_[slot];
        for (int s = 0; s < 2; s++) {
            ci.cand[l][s] = cand_[slot][s].data();
            ci.opaque[l][s] = op_[slot][s].data();
            ci.inner[l][s] = inner_[slot][s].data();
            ci.inner_opaque[l][s] = innerop_[slot][s].data();
        }
        ci.order[ci.norder++] = l;
    }
    ci.inner_n = WK_MAX_INNER;
    ci.out = wide_.data();
    ci.out_stride = VW + 2 * o_.margin;
    ci.prov = o_.render_every > 0 ? NULL : prov_.data();
    double t3 = now_us();
    if (!o_.skip_lib) wk_compose_frame(comp_, &ci);
    double t4 = now_us();
    t_layers_us += t1 - t0;
    t_map_us += t2 - t1;
    t_compose_us += t4 - t3;
    if (o_.render_every <= 0) verify();

    // series for the fg layer on the ground rows
    if (o_.render_every <= 0 && idx_[2] >= 0) {
        const int l = idx_[2], M = o_.margin, Wd = VW + 2 * M;
        double sh[2] = {0, 0}, re[2] = {0, 0}, ri[2] = {0, 0};
        int probe[2] = {0, 0};
        for (int y = 80; y < VH; y++)
            for (int s = 0; s < 2; s++) {
                if (y < 200) {
                    re[s] += out_.line[y].real[s][l];
                    ri[s] += out_.line[y].ring[s][l];
                    continue;
                }
                // ground rows: opaque, so the shown extent is measurable
                int ext = 0;
                for (int d = M - 1; d >= 0; d--) {
                    int xs = s == 0 ? M - 1 - d : M + VW + d;
                    uint8_t pv = prov_[(size_t)y * (size_t)Wd + (size_t)xs];
                    if ((pv >> 4) == l && ((pv & 15) == WK_PROV_LEVEL || (pv & 15) == WK_PROV_FADE)) {
                        ext = d + 1;
                        break;
                    }
                }
                sh[s] += ext;
                if (y == 210) probe[s] = ext;
            }
        for (int s = 0; s < 2; s++) {
            m_.shown[s].push_back((float)(sh[s] / 24));
            m_.real[s].push_back((float)(re[s] / 120));
            m_.ring[s].push_back((float)(ri[s] / 120));
            m_.probe[s].push_back(probe[s]);
        }
    }
    for (int slot = 0; slot < 4; slot++) {
        if (idx_[slot] < 0) { m_.kind[slot].push_back(-1); continue; }
        int l = idx_[slot];
        m_.kind[slot].push_back(out_.kind[l]);
        if (m_.back_frame[slot] < 0 && out_.kind[l] == WK_KIND_BACKDROP) {
            int n = 0;
            for (int y = 0; y < VH; y++) n += (out_.line[y].backdrop_mask >> l) & 1;
            if (n == VH && out_.line[VH / 2].real[0][l] == out_.cap[0]) m_.back_frame[slot] = f_;
        }
    }
    if (out_.hud_lines > 0 && m_.hud_frame < 0) m_.hud_frame = f_;
    if (out_.hud_lines > m_.hud_max) m_.hud_max = out_.hud_lines;
    if (o_.split) {
        for (int y = 24; y < VH; y++)
            if (out_.line[y].hud) m_.hud_on_level_lines++;
    }
    m_.hs_checked += out_.hs_checked;
    m_.hs_missed += out_.hs_missed;
    m_.frames++;
    f_++;
}

void Sim::run()
{
    while (f_ < o_.frames) step();
}

void Sim::snapshot(std::vector<uint8_t> &truth, std::vector<uint8_t> &comp,
                   std::vector<uint8_t> &prov, int &w, int &h)
{
    const int M = o_.margin, Wd = VW + 2 * M;
    w = Wd;
    h = VH;
    truth.assign((size_t)w * (size_t)h * 3, 0);
    comp.assign(truth.size(), 0);
    prov.assign(truth.size(), 0);
    static const uint8_t pc[16][3] = {{90, 90, 90},  {20, 20, 20},   {150, 60, 200},
                                      {40, 200, 80}, {50, 90, 240},  {60, 220, 230},
                                      {230, 60, 60}, {255, 255, 255}, {240, 220, 60},
                                      {240, 150, 40}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
                                      {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (int y = 0; y < VH; y++)
        for (int x = 0; x < Wd; x++) {
            uint16_t c;
            int side = x < M ? 0 : (x >= M + VW ? 1 : -1);
            if (side < 0) {
                c = centre_[(size_t)y * VW + (size_t)(x - M)];
            } else {
                int d = side == 0 ? M - 1 - x : x - (M + VW);
                c = bg_[(size_t)y];
                for (int slot = 0; slot < 3; slot++) {
                    if (idx_[slot] < 0) continue;
                    if (slot == 2 && o_.split && y < 24) continue;
                    Px p = truth_px(slot, side, y, d);
                    if (p.op && p.rgb != 0xF81F) c = p.rgb;
                }
            }
            size_t o = ((size_t)y * (size_t)w + (size_t)x) * 3;
            rgb565_to_rgb(c, &truth[o]);
            rgb565_to_rgb(wide_[(size_t)y * (size_t)Wd + (size_t)x], &comp[o]);
            uint8_t pv = prov_[(size_t)y * (size_t)Wd + (size_t)x];
            const uint8_t *col = pc[pv & 15];
            prov[o] = col[0]; prov[o + 1] = col[1]; prov[o + 2] = col[2];
        }
}

}  // namespace synth
