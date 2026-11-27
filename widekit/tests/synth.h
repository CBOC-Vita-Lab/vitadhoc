/* widekit tests - MIT licence.
 *
 * A synthetic game: tilesets, a world, ring-buffered tilemaps fed by several
 * kinds of scroll engines, a "core adapter" that fills wk_frame and draws
 * candidate margin strips, and a reference renderer that draws the TRUE wide
 * picture from the full world so every pixel widekit claims can be checked. */
#ifndef WK_SYNTH_H
#define WK_SYNTH_H

#include <limits.h>
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

#include "wk_compose.h"
#include "wk_layers.h"
#include "wk_map.h"
#include "wk_profile.h"

namespace synth {

enum { VW = 256, VH = 224, TP = 8 };

struct Tileset {
    uint32_t id;
    uint8_t px[256][64];
    uint16_t pal[8][4];
    uint32_t gh[256];
};
void make_tileset(Tileset &ts, uint32_t seed);

struct Px {
    uint16_t rgb;
    uint8_t op;
};

enum Engine { ENG_STREAM = 0, ENG_PAGE, ENG_ROOMS };

struct Ring {
    int w = 0, h = 0;
    std::vector<uint32_t> code, hash, stamp;
    void init(int w_, int h_)
    {
        w = w_; h = h_;
        code.assign((size_t)w * (size_t)h, 0);
        hash.assign((size_t)w * (size_t)h, 0);
        stamp.assign((size_t)w * (size_t)h, 0);
    }
};

class Sim;
typedef std::function<void(Sim &, int)> Script;

struct Opts {
    std::string name = "scn";
    int frames = 300;
    int margin = 48;
    bool sky = false, mtn = false, hud_layer = false, split = false;
    bool fg = true;
    int engine = ENG_STREAM;
    int lead_r = 2, lead_l = 2;       // columns the engine keeps ahead
    int wcols = 200;
    bool decoy = true;                // ring holds old junk before the level
    bool use_map = true;
    wk_map *shared_map = nullptr;     // use this map (persistence tests)
    std::function<int(int)> cam;      // camera x px per frame
    Script script;                    // called first thing each frame
    int blank_frames = 10;            // forced blank at start
    uint32_t seed = 7;
    int fg_ring_w = 64;
    int sky_ring_w = 32;
    bool fg_stamps = true;            // give wk_layers stamps
    const wk_profile *profile = nullptr;
    int fade_mode = WK_FADE_ALPHA;
    int fade_px = 20;
    int smear_px = 0;
    int ground = 0;                   // 0 random tiles, 1 periodic ground, 2 periodic with pits
    bool skip_lib = false;            // bench: build the frames but do not call the library
    int soft_blur = -1;               // -1 = library default
    int soft_sample = -1;
    int period = 1;                   // wk_compose periodic continuation
    int render_every = 0;             // >0: bench mode, draw/verify only occasionally
    bool want_prov = true;            // false: no provenance map requested (wk_compose_in.prov = NULL)
};

struct Metrics {
    long frames = 0;
    long claimed[4] = {0, 0, 0, 0}, wrong[4] = {0, 0, 0, 0};
    long first_wrong_frame = -1;
    long stale_gfx_drawn = 0;
    long soft_px = 0, margin_px = 0;
    int back_frame[4] = {-1, -1, -1, -1};   // first frame layer is backdrop on all lines
    int hud_frame = -1;                      // first frame hud lines flagged
    int hud_max = 0;
    long hud_on_level_lines = 0;             // level lines wrongly flagged hud
    long period_claimed[4] = {0, 0, 0, 0}, period_wrong[4] = {0, 0, 0, 0};
    long period_wrong_frames = 0;            // frames with at least one wrong predicted px
    int first_period_wrong_frame = -1;
    std::vector<int> period_px;              // predicted px per frame (fg, both sides)
    std::vector<int> period_bad;             // wrong predicted px per frame
    long hs_checked = 0, hs_missed = 0;
    // per-frame series (FG, ground rows): shown px from compose, real from ana
    std::vector<float> shown[2], real[2], ring[2];
    std::vector<int> probe[2];               // shown extent at one line
    std::vector<int> kind[4];
};

class Sim {
public:
    explicit Sim(const Opts &o);
    ~Sim();
    void step();
    void run();
    int frame() const { return f_; }
    const Opts &opts() const { return o_; }
    Metrics &m() { return m_; }
    double t_layers_us = 0, t_map_us = 0, t_compose_us = 0;
    // scripts
    void set_brightness_override(int b) { bright_override_ = b; }
    void swap_tileset();
    void load_room(int k);               // ROOMS engine: write room k to the hidden page
    void flip_to_room(int k);            // ... and jump the camera onto it
    void set_sky_streaming(bool on) { sky_stream_ = on; }
    // output
    // three Wd x VH RGB images: the true wide picture, widekit's output and a
    // provenance map (grey centre, black flat bg, purple soft edge, green
    // backdrop, blue level, cyan fade, red ghost)
    void snapshot(std::vector<uint8_t> &truth, std::vector<uint8_t> &comp,
                  std::vector<uint8_t> &prov, int &w, int &h);
    const wk_out &ana() const { return out_; }
    wk_map *map() { return map_; }
    int period_bans() const { return wk_compose_period_bans(comp_); }
    const std::vector<uint16_t> &composed() const { return wide_; }
    const std::vector<uint8_t> &prov() const { return prov_; }   // last provenance map (valid when want_prov)
    int camx() const { return camx_; }
    int cur_room() const { return room_; }
    uint32_t gfx() const { return ts_[cur_ts_].id; }

private:
    friend struct Verify;
    Opts o_;
    Metrics m_;
    int f_ = 0;
    Tileset ts_[2];
    int cur_ts_ = 0;
    Ring fg_, sky_, mtn_, hud_;
    int camx_ = 0, last_camx_ = 0;
    int lo_ = 0, hi_ = -1;               // stream window of world columns
    int last_page_ = -1;
    int room_ = 0;
    int bright_override_ = -1;
    bool sky_stream_ = false;
    bool loaded_ = false;
    int load_frame_ = 0;
    int sky_sw_col_ = INT_MIN, sky_hi_ = 0;

    wk_frame fr_;
    wk_out out_;
    wk_layers *layers_ = nullptr;
    wk_map *map_ = nullptr;
    bool own_map_ = false;
    wk_compose *comp_ = nullptr;

    int nl_ = 0;
    int idx_[4] = {-1, -1, -1, -1};      // layer index of sky, mtn, fg, hud
    int scroll_[4][VH];

    std::vector<uint16_t> centre_, wide_;
    std::vector<uint8_t> prov_;
    std::vector<uint16_t> cand_[4][2], inner_[4][2];
    std::vector<uint8_t> innerop_[4][2];
    std::vector<uint8_t> op_[4][2];
    std::vector<uint16_t> bg_;

    uint32_t cell_code_world(int col, int row) const;       // FG world
    uint32_t code_sky(int col, int row) const;
    uint32_t code_mtn(int col, int row) const;
    uint32_t code_hud(int col, int row) const;
    uint32_t truth_fg(int wcol, int row) const;
    void fill_ring_hashes(Ring &r);
    void write_fg_col(int wcol);
    void write_page(int page);
    void load_level();
    void engine_update();
    void build_frame();
    void render_centre();
    Px tile_px(uint32_t code, int px, int py) const;
    Px ring_px(const Ring &r, int xl, int yrow, int line_y) const;
    void render_candidates();
    void verify();
    uint16_t bg_colour(int y) const;
    Px truth_px(int slot, int side, int y, int d) const;
};

uint32_t fold16(uint32_t v);

}  // namespace synth

#endif
