/* widekit - MIT licence.  wk_layers: see ../include/wk_layers.h.
 *
 * How a margin cell is certified (per tile row, per side, outward from the
 * view edge):
 *   SEEN   the very same ring slot was displayed earlier in this scene at
 *          exactly this world tile column and its content hash is unchanged
 *          since.  That is history; it is proof, not a guess.
 *   CHAIN  never displayed: accepted only as a run that starts at the view
 *          edge and consists of entries the game wrote in this scene, each
 *          not older than the one before and the first strictly newer than
 *          the entry at the edge (it was written for the incoming seam), or
 *          equal when the layer has proven resident (camera travelled with
 *          no writes).  Entries aliasing visible columns or each other are
 *          never taken.  Chain predictions are checked when the cell finally
 *          scrolls into view (hindsight); a miss switches chains off.
 * Backdrop continuation is a separate, evidence driven decision per layer
 * (periodic rows, slow parallax, wrap-around travel) that is revoked the
 * moment the game writes beyond the layer's edges again. */

#include <stdio.h>
#include <stdlib.h>
#include "wk_layers.h"
#include "wk_util.h"

#include <stdlib.h>
#include <string.h>

namespace {

const int kMaxDim = 256;

struct LS {
    int init;
    int map_w, map_h, tp, cells;
    uint32_t *seen_hash;
    int16_t *seen_wc;
    uint8_t *seen_ep;
    uint32_t *pred_hash;
    int16_t *pred_wc;
    uint8_t *pred_ep;
    uint32_t *prev_hash;
    uint8_t row_streak[kMaxDim];
    int32_t prev_sx[WK_MAX_LINES];
    int32_t prev_row[WK_MAX_LINES];
    uint8_t prev_on[WK_MAX_LINES];
    int prev_ref;              /* reference line of the previous frame, -1 none */
    int have_prev;
    uint32_t gfx_prev;
    int have_gfx;
    int32_t U, V, Umin, Umax;
    int travel_ema;            /* |dx| * 256, EMA 1/16 */
    int ratio_q8, ratio_n;
    uint32_t quiet_travel, quiet_own;
    int dir;
    uint8_t still[WK_MAX_LINES];
    uint8_t follow[WK_MAX_LINES];
    uint8_t hud[WK_MAX_LINES];
    int chain_ban;
    int stream_ema;
    int streamed;              /* wrote beyond its edges at any time this scene */
    int was_main;              /* was the camera layer at any time this scene   */
    int kind, conf;
};

struct Scratch {
    int32_t dsx[WK_MAX_LAYERS][WK_MAX_LINES];
    int32_t dsy[WK_MAX_LAYERS][WK_MAX_LINES];
    int32_t dom_dx[WK_MAX_LAYERS], dom_dy[WK_MAX_LAYERS];
    int ref_y[WK_MAX_LAYERS];
    int valid[WK_MAX_LAYERS];
    int role[WK_MAX_LAYERS];
    int jump[WK_MAX_LAYERS];
    uint8_t rowmark[kMaxDim];
    uint8_t vis[kMaxDim];
    uint8_t claimed[kMaxDim];
    uint8_t periodic[kMaxDim];
};

}  // namespace

struct wk_layers {
    wk_layers_cfg cfg;
    LS ls[WK_MAX_LAYERS];
    Scratch sc;
    const wk_profile *prof;
    int have;
    uint32_t last_frame;
    uint32_t epoch, epoch_start, last_event;
    int in_cluster;
    uint8_t ep8;
    int main_layer;
};

void wk_layers_cfg_default(wk_layers_cfg *c)
{
    c->jump_px = 64;
    c->quiet_px = 96;
    c->periodic_frames = 30;
    c->par_px = 48;
    c->par_frames = 24;
    c->hud_still = 24;
    c->hud_main_min_q8 = 96;
    c->stamp_tol = 2;
    c->resident_px = 0;
    c->cluster_gap = 8;
    c->blank_thr = 2;
    c->max_cells = 4096;
    c->chain_ban_frames = 300;
}

static void ls_free(LS *s)
{
    free(s->seen_hash);
    free(s->seen_wc);
    free(s->seen_ep);
    free(s->pred_hash);
    free(s->pred_wc);
    free(s->pred_ep);
    free(s->prev_hash);
}

wk_layers *wk_layers_create(const wk_layers_cfg *cfg)
{
    wk_layers *w = (wk_layers *)calloc(1, sizeof *w);
    if (!w)
        return NULL;
    if (cfg)
        w->cfg = *cfg;
    else
        wk_layers_cfg_default(&w->cfg);
    if (w->cfg.max_cells < 64)
        w->cfg.max_cells = 64;
    size_t n = (size_t)w->cfg.max_cells;
    for (int l = 0; l < WK_MAX_LAYERS; l++) {
        LS *s = &w->ls[l];
        s->seen_hash = (uint32_t *)calloc(n, 4);
        s->seen_wc = (int16_t *)calloc(n, 2);
        s->seen_ep = (uint8_t *)calloc(n, 1);
        s->pred_hash = (uint32_t *)calloc(n, 4);
        s->pred_wc = (int16_t *)calloc(n, 2);
        s->pred_ep = (uint8_t *)calloc(n, 1);
        s->prev_hash = (uint32_t *)calloc(n, 4);
        if (!s->seen_hash || !s->seen_wc || !s->seen_ep || !s->pred_hash ||
            !s->pred_wc || !s->pred_ep || !s->prev_hash) {
            wk_layers_destroy(w);
            return NULL;
        }
    }
    w->main_layer = -1;
    w->ep8 = 1;
    return w;
}

void wk_layers_destroy(wk_layers *w)
{
    if (!w)
        return;
    for (int l = 0; l < WK_MAX_LAYERS; l++)
        ls_free(&w->ls[l]);
    free(w);
}

static void ls_clear_scene(LS *s)
{
    s->quiet_travel = s->quiet_own = 0;
    s->ratio_q8 = 0;
    s->ratio_n = 0;
    s->chain_ban = 0;
    s->stream_ema = 0;
    s->streamed = 0;
    s->was_main = 0;
    s->kind = WK_KIND_UNKNOWN;
    s->conf = 0;
    memset(s->row_streak, 0, sizeof s->row_streak);
    memset(s->still, 0, sizeof s->still);
    memset(s->follow, 0, sizeof s->follow);
    memset(s->hud, 0, sizeof s->hud);
}

void wk_layers_reset(wk_layers *w)
{
    for (int l = 0; l < WK_MAX_LAYERS; l++) {
        LS *s = &w->ls[l];
        s->init = 0;
        s->have_prev = 0;
        s->prev_ref = -1;
        s->have_gfx = 0;
        s->travel_ema = 0;
        s->dir = 0;
        ls_clear_scene(s);
        memset(s->seen_ep, 0, (size_t)w->cfg.max_cells);
        memset(s->pred_ep, 0, (size_t)w->cfg.max_cells);
    }
    w->have = 0;
    w->in_cluster = 0;
    w->main_layer = -1;
    w->ep8++;
    if (w->ep8 == 0)
        w->ep8 = 1;
}

void wk_layers_set_profile(wk_layers *w, const wk_profile *p) { w->prof = p; }

void wk_out_refresh_line(wk_out *out, int y)
{
    wk_line_out *lo = &out->line[y];
    for (int s = 0; s < 2; s++) {
        int v = out->margin;
        for (int l = 0; l < WK_MAX_LAYERS; l++)
            if ((lo->level_mask >> l) & 1)
                v = wk_min(v, lo->real[s][l]);
        lo->level_real[s] = (uint16_t)v;
    }
}

void wk_out_raise(wk_out *out, int layer, int y, int side, int px)
{
    wk_line_out *lo = &out->line[y];
    px = wk_min(px, out->margin);
    px = wk_min(px, (int)out->cap[side]);
    if (px > lo->real[side][layer])
        lo->real[side][layer] = (uint16_t)px;
    wk_out_refresh_line(out, y);
}

/* ------------------------------------------------------------------------- */

static int row_period(const uint32_t *r, int n)
{
    for (int p = 1; p <= n / 2; p++) {
        if (n % p || r[0] != r[p])
            continue;
        int ok = 1;
        for (int i = 0; i < n - p; i++)
            if (r[i] != r[i + p]) {
                ok = 0;
                break;
            }
        if (ok)
            return p;
    }
    return n;
}

static int layer_valid(const wk_layers *w, const wk_layer *L, int role)
{
    if (!L->active || role == WK_ROLE_OFF || !L->hash)
        return 0;
    if (L->map_w < 1 || L->map_h < 1 || L->tile_px < 1)
        return 0;
    if (L->map_w > kMaxDim || L->map_h > kMaxDim)
        return 0;
    if (L->map_w * L->map_h > w->cfg.max_cells)
        return 0;
    return 1;
}

static int on(const wk_layer *L, int y) { return !L->line_on || L->line_on[y]; }

/* Certification of margin cells for one tile row. */
struct Walk {
    wk_layers *w;
    const wk_layer *L;
    LS *s;
    int tr, mw, tp, M;
    int32_t tf;        /* first visible unwrapped tile column */
    int nvis;
    int lead_cap;      /* -1 = none */
    int fresh_edge;    /* edge entry written within the last 2 frames */
    uint32_t frame;
};

static int seen_ok(const Walk &k, int idx, int32_t ut)
{
    return k.s->seen_ep[idx] == k.w->ep8 && k.s->seen_wc[idx] == (int16_t)ut &&
           k.s->seen_hash[idx] == k.L->hash[idx];
}

/* Returns cells certified on one side.  dirn = -1 left, +1 right. */
static int walk_side(const Walk &k, int dirn, int partial, Scratch &sc, int stamps)
{
    const wk_layers_cfg &c = k.w->cfg;
    int n = 0;
    int32_t first = dirn < 0 ? k.tf - 1 : k.tf + k.nvis;
    int32_t edge_t = dirn < 0 ? k.tf : k.tf + k.nvis - 1;
    int rc;
    /* verified history */
    for (; partial + k.tp * n < k.M; n++) {
        int32_t ut = first + dirn * n;
        rc = wk_floormod(ut, k.mw);
        if (sc.vis[rc])
            break;
        int idx = k.tr * k.mw + rc;
        if (!seen_ok(k, idx, ut)) {
            if (getenv("WKDBG2") && k.frame == (uint32_t)atoi(getenv("WKDBG2")) && k.tr == 12)
                fprintf(stderr, "hist dir=%d n=%d ut=%d rc=%d ep=%d/%d wc=%d/%d hash=%08x/%08x\n", dirn, n, ut, rc, k.s->seen_ep[idx], k.w->ep8, k.s->seen_wc[idx], (int16_t)ut, k.s->seen_hash[idx], k.L->hash[idx]);
            break;
        }
        sc.claimed[rc] = 1;
    }
    if (n > 0 || !stamps || k.s->chain_ban > 0)
        return n;
    /* chain from the edge */
    int eidx = k.tr * k.mw + wk_floormod(edge_t, k.mw);
    int64_t e = (int64_t)k.L->stamp[eidx];
    int64_t prev = e;
    int64_t floor_t = (int64_t)k.w->epoch_start - 4;
    int resident = c.resident_px > 0 && k.s->quiet_travel >= (uint32_t)c.resident_px;
    int cnt = 0;
    for (; partial + k.tp * n < k.M; n++) {
        if (k.lead_cap >= 0 && cnt >= k.lead_cap)
            break;
        int32_t ut = first + dirn * n;
        rc = wk_floormod(ut, k.mw);
        if (sc.vis[rc] || sc.claimed[rc])
            break;
        int idx = k.tr * k.mw + rc;
        int64_t st = (int64_t)k.L->stamp[idx];
        if (st < floor_t || st > (int64_t)k.frame)
            break;
        if (st + c.stamp_tol < prev)
            break;
        if (n == 0) {
            int newer = st > e;
            if (!(newer || k.fresh_edge || (resident && st + c.stamp_tol >= e)))
                break;
        }
        prev = st;
        sc.claimed[rc] = 1;
        k.s->pred_hash[idx] = k.L->hash[idx];
        k.s->pred_wc[idx] = (int16_t)ut;
        k.s->pred_ep[idx] = k.w->ep8;
        cnt++;
    }
    return n;
}

void wk_layers_update(wk_layers *w, const wk_frame *f, wk_out *out)
{
    memset(out, 0, sizeof *out);
    Scratch &sc = w->sc;
    const wk_layers_cfg &cfg = w->cfg;
    const int H = wk_clamp(f->view_h, 1, WK_MAX_LINES);
    const int W = f->view_w;
    int M = wk_clamp(f->margin, 0, WK_MAX_MARGIN);
    out->nlines = H;
    out->margin = M;
    out->cap[0] = out->cap[1] = (uint16_t)M;
    if (w->prof) {
        if (w->prof->margin_left >= 0) out->cap[0] = (uint16_t)wk_min(M, w->prof->margin_left);
        if (w->prof->margin_right >= 0) out->cap[1] = (uint16_t)wk_min(M, w->prof->margin_right);
    }
    out->main_layer = -1;

    if (!w->have || f->frame < w->last_frame) {
        wk_layers_reset(w);
        w->epoch_start = f->frame;
    }
    w->have = 1;
    w->last_frame = f->frame;

    const int nl = wk_clamp(f->nlayers, 0, WK_MAX_LAYERS);
    const wk_profile *pf = w->prof;

    /* Which layers we can use, and their deltas. */
    for (int l = 0; l < WK_MAX_LAYERS; l++)
        sc.valid[l] = 0;
    for (int l = 0; l < nl; l++) {
        const wk_layer *L = &f->layer[l];
        int role = L->role;
        if (pf && pf->role[l] != WK_ROLE_AUTO)
            role = pf->role[l];
        sc.role[l] = role;
        if (!layer_valid(w, L, role))
            continue;
        LS *s = &w->ls[l];
        sc.valid[l] = 1;
        if (!s->init || s->map_w != L->map_w || s->map_h != L->map_h ||
            s->tp != L->tile_px) {
            s->init = 1;
            s->map_w = L->map_w;
            s->map_h = L->map_h;
            s->tp = L->tile_px;
            s->cells = L->map_w * L->map_h;
            s->have_prev = 0;
        s->prev_ref = -1;
            s->have_gfx = 0;
            s->travel_ema = 0;
            memset(s->seen_ep, 0, (size_t)s->cells);
            memset(s->pred_ep, 0, (size_t)s->cells);
            memcpy(s->prev_hash, L->hash, (size_t)s->cells * 4);
            ls_clear_scene(s);
        }
        const int wrap = s->map_w * s->tp, wraph = s->map_h * s->tp;
        /* per line deltas */
        for (int y = 0; y < H; y++) {
            if (s->have_prev) {
                sc.dsx[l][y] = wk_wrapdiff(L->scroll_x[y] - s->prev_sx[y], wrap);
                sc.dsy[l][y] = wk_wrapdiff(L->row_y[y] - s->prev_row[y], wraph);
            } else {
                sc.dsx[l][y] = sc.dsy[l][y] = 0;
            }
        }
        /* plurality (Boyer-Moore) of dx and dy over the lines that are on */
        int32_t cand = 0, cand_y = 0;
        int cnt = 0, cnty = 0;
        for (int y = 0; y < H; y++) {
            if (!on(L, y))
                continue;
            if (cnt == 0) { cand = sc.dsx[l][y]; cnt = 1; }
            else if (sc.dsx[l][y] == cand) cnt++;
            else cnt--;
            if (cnty == 0) { cand_y = sc.dsy[l][y]; cnty = 1; }
            else if (sc.dsy[l][y] == cand_y) cnty++;
            else cnty--;
        }
        sc.dom_dx[l] = cand;
        sc.dom_dy[l] = cand_y;
        sc.ref_y[l] = 0;
        {
            /* keep last frame's reference line while it still moves with the
             * plurality: a split screen (fixed status bar + scrolling field)
             * would otherwise flip the reference between the bar and the field
             * whenever the camera stops or starts  [NES adapter fix] */
            const LS *ps = &w->ls[l];
            if (s->have_prev && ps->prev_ref >= 0 && ps->prev_ref < H && on(L, ps->prev_ref) &&
                sc.dsx[l][ps->prev_ref] == cand) {
                sc.ref_y[l] = ps->prev_ref;
            } else {
                for (int y = 0; y < H; y++)
                    if (on(L, y) && sc.dsx[l][y] == cand) {
                        sc.ref_y[l] = y;
                        break;
                    }
            }
        }
        sc.jump[l] = (wk_abs(cand) > cfg.jump_px || wk_abs(cand_y) > cfg.jump_px) &&
                     role != WK_ROLE_HUD;
    }

    /* Scene change detection. */
    int event = 0;
    for (int l = 0; l < nl; l++) {
        if (!sc.valid[l])
            continue;
        LS *s = &w->ls[l];
        if (sc.jump[l]) { event = 1; if (getenv("WKDBG")) fprintf(stderr, "ev f=%u jump l=%d dx=%d dy=%d\n", f->frame, l, sc.dom_dx[l], sc.dom_dy[l]); }
        if (s->have_gfx && s->gfx_prev != f->layer[l].gfx_id) { event = 1; if (getenv("WKDBG")) fprintf(stderr, "ev f=%u gfx l=%d\n", f->frame, l); }
    }
    if (f->brightness <= cfg.blank_thr) { event = 1; if (getenv("WKDBG")) fprintf(stderr, "ev f=%u dark\n", f->frame); }
    if (f->palette_size > 0 && f->palette_delta * 2 >= f->palette_size) { event = 1; if (getenv("WKDBG")) fprintf(stderr, "ev f=%u pal %d\n", f->frame, f->palette_delta); }
    if (event) {
        out->scene_event = 1;
        if (!w->in_cluster || f->frame - w->last_event > (uint32_t)cfg.cluster_gap) {
            w->epoch++;
            w->epoch_start = f->frame;
        }
        w->last_event = f->frame;
        w->in_cluster = 1;
        w->ep8++;
        if (w->ep8 == 0)
            w->ep8 = 1;
    } else if (w->in_cluster && f->frame - w->last_event > (uint32_t)cfg.cluster_gap) {
        w->in_cluster = 0;
    }
    out->epoch = w->epoch;

    /* Tracking: unwrapped position, travel, direction. */
    for (int l = 0; l < nl; l++) {
        if (!sc.valid[l])
            continue;
        const wk_layer *L = &f->layer[l];
        LS *s = &w->ls[l];
        const int ry = sc.ref_y[l];
        if (event || !s->have_prev) {
            s->U = L->scroll_x[ry];
            s->V = L->row_y[ry];
            s->Umin = s->Umax = s->U;
            ls_clear_scene(s);
            sc.dom_dx[l] = sc.dom_dy[l] = 0;
            for (int y = 0; y < H; y++)
                sc.dsx[l][y] = sc.dsy[l][y] = 0;
        } else if (s->prev_ref >= 0 && s->prev_ref != ry && s->prev_on[ry]) {
            /* the reference line changed: continue from where the new reference
             * line was last frame (relative to the old one, in last frame's
             * picture) plus its own delta, so world coordinates do not jump
             * [NES adapter fix] */
            const int pry = s->prev_ref;
            const int32_t wrapx = s->map_w * s->tp, wraph = s->map_h * s->tp;
            int32_t dv = 0, pr = s->prev_row[pry];
            const int step = ry > pry ? 1 : -1;
            for (int y = pry + step;; y += step) {
                if (s->prev_on[y] || y == ry) {
                    dv += wk_wrapdiff(s->prev_row[y] - pr, wraph);
                    pr = s->prev_row[y];
                }
                if (y == ry)
                    break;
            }
            s->U += wk_wrapdiff(s->prev_sx[ry] - s->prev_sx[pry], wrapx) + sc.dsx[l][ry];
            s->V += dv + sc.dsy[l][ry];
            if (s->U < s->Umin) s->Umin = s->U;
            if (s->U > s->Umax) s->Umax = s->U;
        } else {
            s->U += sc.dom_dx[l];
            s->V += sc.dom_dy[l];
            if (s->U < s->Umin) s->Umin = s->U;
            if (s->U > s->Umax) s->Umax = s->U;
        }
        if (sc.dom_dx[l] != 0)
            s->dir = sc.dom_dx[l] > 0 ? 1 : -1;
        s->travel_ema += ((wk_abs(sc.dom_dx[l]) << 8) - s->travel_ema) / 16;
        for (int y = 0; y < H; y++) {
            s->prev_sx[y] = L->scroll_x[y];
            s->prev_row[y] = L->row_y[y];
            s->prev_on[y] = (uint8_t)on(L, y);
        }
        s->prev_ref = ry;
        s->have_prev = 1;
        s->gfx_prev = L->gfx_id;
        s->have_gfx = 1;
        if (s->chain_ban > 0)
            s->chain_ban--;
    }

    /* The camera layer: the LEVEL layer, else the fastest mover. */
    int mainl = -1, best = -1;
    for (int l = 0; l < nl; l++) {
        if (!sc.valid[l] || sc.role[l] == WK_ROLE_HUD)
            continue;
        int sc_v = w->ls[l].travel_ema + (sc.role[l] == WK_ROLE_LEVEL ? 100000 : 0);
        if (sc_v > best || (sc_v == best && mainl >= 0 &&
                            f->layer[l].priority > f->layer[mainl].priority)) {
            best = sc_v;
            mainl = l;
        }
    }
    if (mainl >= 0 && w->ls[mainl].travel_ema < 16 && sc.role[mainl] != WK_ROLE_LEVEL)
        mainl = -1;
    w->main_layer = mainl;
    if (mainl >= 0)
        w->ls[mainl].was_main = 1;
    out->main_layer = mainl;
    const int main_dx = mainl >= 0 ? sc.dom_dx[mainl] : 0;
    const int main_fast = mainl >= 0 && w->ls[mainl].travel_ema >= cfg.hud_main_min_q8;

    for (int l = 0; l < nl; l++) {
        out->dir[l] = w->ls[l].dir;
        out->kind[l] = sc.valid[l] ? WK_KIND_UNKNOWN : WK_KIND_OFF;
    }

    /* Blank frame: nothing is real. */
    if (f->brightness <= cfg.blank_thr) {
        for (int y = 0; y < H; y++)
            for (int s = 0; s < 2; s++)
                out->line[y].level_real[s] = 0;
        return;
    }

    int hud_total = 0;
    uint8_t line_hud[WK_MAX_LINES];
    uint8_t line_has_hud[WK_MAX_LINES];
    memset(line_hud, 0, sizeof line_hud);
    memset(line_has_hud, 0, sizeof line_has_hud);

    for (int y = 0; y < H; y++)
        for (int s = 0; s < 2; s++)
            for (int l = 0; l < WK_MAX_LAYERS; l++) {
                out->line[y].real[s][l] = (uint16_t)M;
                out->line[y].ring[s][l] = (uint16_t)M;
            }

    /* Per layer. */
    for (int l = 0; l < nl; l++) {
        if (!sc.valid[l])
            continue;
        const wk_layer *L = &f->layer[l];
        LS *s = &w->ls[l];
        const int mw = s->map_w, mh = s->map_h, tp = s->tp;
        const int wrap = mw * tp;
        const int role = sc.role[l];
        const int ry = sc.ref_y[l];

        /* rows in view */
        memset(sc.rowmark, 0, (size_t)mh);
        int nrows = 0;
        for (int y = 0; y < H; y++) {
            if (!on(L, y))
                continue;
            int tr = wk_floormod(wk_floordiv(L->row_y[y], tp), mh);
            if (!sc.rowmark[tr]) {
                sc.rowmark[tr] = 1;
                nrows++;
            }
        }

        /* streaming evidence: fresh writes at / beyond the view edges */
        int stream_now = 0;
        {
            const int32_t sxr = L->scroll_x[ry];
            const int32_t tfr = wk_floordiv(sxr, tp);
            const int cf = wk_floormod(tfr, mw);
            int nv = (int)(wk_floordiv(sxr + W - 1, tp) - tfr + 1);
            if (nv > mw) nv = mw;
            for (int tr = 0; tr < mh && !stream_now; tr++) {
                if (!sc.rowmark[tr])
                    continue;
                for (int rel = 0; rel < mw; rel++) {
                    if (rel >= 2 && rel <= nv - 3)
                        continue;
                    int idx = tr * mw + (cf + rel) % mw;
                    int fresh;
                    if (L->stamp)
                        fresh = (int64_t)f->frame - (int64_t)L->stamp[idx] <= 1;
                    else
                        fresh = L->hash[idx] != s->prev_hash[idx];
                    if (fresh) {
                        stream_now = 1;
                        break;
                    }
                }
            }
            memcpy(s->prev_hash, L->hash, (size_t)s->cells * 4);
        }
        out->streaming[l] = stream_now;
        if (stream_now) {
            s->quiet_travel = s->quiet_own = 0;
            s->stream_ema = 255;
            s->streamed = 1;
        } else {
            uint32_t trav = (uint32_t)wk_max(wk_abs(sc.dom_dx[l]), wk_abs(main_dx));
            s->quiet_travel += trav;
            s->quiet_own += (uint32_t)wk_abs(sc.dom_dx[l]);
            s->stream_ema -= s->stream_ema >> 6;
        }
        if (s->quiet_travel > 1000000u) s->quiet_travel = 1000000u;
        if (s->quiet_own > 1000000u) s->quiet_own = 1000000u;

        /* periodic rows */
        for (int tr = 0; tr < mh; tr++) {
            if (!sc.rowmark[tr]) {
                s->row_streak[tr] = 0;
                continue;
            }
            int p = row_period(L->hash + (size_t)tr * mw, mw);
            sc.periodic[tr] = p < mw || mw == 1;
            if (sc.periodic[tr]) {
                if (s->row_streak[tr] < 255) s->row_streak[tr]++;
            } else {
                s->row_streak[tr] = 0;
            }
        }

        /* parallax ratio against the camera */
        if (mainl >= 0 && l != mainl && wk_abs(main_dx) >= 1) {
            int sample = wk_abs(sc.dom_dx[l]) * 256 / wk_abs(main_dx);
            s->ratio_q8 += (sample - s->ratio_q8) / 8;
            if (s->ratio_n < 1000) s->ratio_n++;
        }
        const int wrapped = (s->Umax - s->Umin) >= wrap;
        const int qualifies = s->quiet_travel >= (uint32_t)cfg.quiet_px;
        const int par = s->ratio_n >= cfg.par_frames && s->ratio_q8 >= 8 &&
                        s->ratio_q8 <= 230 && s->quiet_own >= (uint32_t)cfg.par_px;

        /* per line flags */
        int nb = 0, nh = 0, non = 0;
        for (int y = 0; y < H; y++) {
            if (!on(L, y))
                continue;
            non++;
            int coh = sc.dsx[l][y] == sc.dom_dx[l] && sc.dsy[l][y] == sc.dom_dy[l];
            /* HUD evidence */
            if (role == WK_ROLE_AUTO) {
                if (main_fast) {
                    if (sc.dsx[l][y] == 0 && sc.dsy[l][y] == 0 &&
                        (l != mainl || sc.dom_dx[l] != 0)) {
                        if (s->still[y] < 255) s->still[y]++;
                        s->follow[y] = 0;
                    } else {
                        s->still[y] = 0;
                        if (sc.dsx[l][y] == main_dx && main_dx != 0) {
                            if (s->follow[y] < 255) s->follow[y]++;
                        }
                    }
                }
                if (s->still[y] >= cfg.hud_still)
                    s->hud[y] = 1;
                else if (s->follow[y] >= 3)
                    s->hud[y] = 0;
            } else {
                s->hud[y] = role == WK_ROLE_HUD;
            }
            if (s->hud[y]) {
                nh++;
                line_has_hud[y] = 1;
                coh = 0;
            }
            out->coherent[l][y] = (uint8_t)(coh && !s->hud[y]);
        }

        /* world coordinates */
        for (int y = 0; y < H; y++)
            out->world_x[l][y] = s->U + wk_wrapdiff(L->scroll_x[y] - L->scroll_x[ry], wrap);
        /* rows are unwrapped line to line from the reference line (lines of one
         * picture step by a few rows), not against the reference line alone: a
         * ring only as tall as the view (NES, 240 px) would otherwise fold the
         * lines more than half a ring below the reference onto the rows above
         * it.  Off lines take the row relative to the last on line.  [NES
         * adapter fix, see INTEGRATION.md] */
        {
            const int32_t hw = mh * tp;
            int32_t pr = L->row_y[ry], pw = s->V;
            for (int y = ry; y < H; y++) {
                int32_t wr = pw + wk_wrapdiff(L->row_y[y] - pr, hw);
                out->world_row[l][y] = wr;
                if (on(L, y)) { pr = L->row_y[y]; pw = wr; }
            }
            pr = L->row_y[ry]; pw = s->V;
            for (int y = ry - 1; y >= 0; y--) {
                int32_t wr = pw + wk_wrapdiff(L->row_y[y] - pr, hw);
                out->world_row[l][y] = wr;
                if (on(L, y)) { pr = L->row_y[y]; pw = wr; }
            }
        }

        /* backdrop / level decision and extents, one tile row run at a time */
        int lead_cap = -1;
        if (pf && pf->lead_tiles >= 0)
            lead_cap = pf->lead_tiles;
        Walk k{};
        k.w = w; k.L = L; k.s = s; k.mw = mw; k.tp = tp; k.M = M;
        k.lead_cap = lead_cap; k.frame = f->frame;

        int run_valid = 0;
        int32_t run_sx = 0, run_tr = -1;
        int run_back = 0;
        uint16_t run_real[2] = {0, 0};
        for (int y = 0; y < H; y++) {
            wk_line_out *lo = &out->line[y];
            if (!on(L, y))
                continue;
            if (s->hud[y]) {
                lo->real[0][l] = lo->real[1][l] = 0;
                lo->ring[0][l] = lo->ring[1][l] = 0;
                continue;
            }
            const int tr = wk_floormod(wk_floordiv(L->row_y[y], tp), mh);
            int back = 0;
            if (role == WK_ROLE_BACKDROP)
                back = 1;
            else if (role == WK_ROLE_AUTO && out->coherent[l][y])
                back = !s->streamed && !s->was_main &&
                       (par || (qualifies && (wrapped || s->row_streak[tr] >= cfg.periodic_frames)));
            if (back) {
                lo->backdrop_mask |= (uint8_t)(1u << l);
                nb++;
                lo->real[0][l] = lo->real[1][l] = (uint16_t)M;
                lo->ring[0][l] = lo->ring[1][l] = (uint16_t)M;
                if (!out->coherent[l][y]) {
                    /* forced backdrop on a split line: still the ring */
                }
                continue;
            }
            lo->level_mask |= (uint8_t)(1u << l);
            if (!out->coherent[l][y]) {
                lo->real[0][l] = lo->real[1][l] = 0;
                lo->ring[0][l] = lo->ring[1][l] = 0;
                continue;
            }
            const int32_t uy = out->world_x[l][y];
            if (run_valid && run_sx == uy && run_tr == tr && run_back == 0) {
                lo->real[0][l] = lo->ring[0][l] = run_real[0];
                lo->real[1][l] = lo->ring[1][l] = run_real[1];
                continue;
            }
            /* new run */
            k.tr = tr;
            k.tf = wk_floordiv(uy, tp);
            k.nvis = (int)(wk_floordiv(uy + W - 1, tp) - k.tf + 1);
            if (k.nvis > mw) k.nvis = mw;
            memset(sc.vis, 0, (size_t)mw);
            memset(sc.claimed, 0, (size_t)mw);
            for (int i = 0; i < k.nvis; i++)
                sc.vis[wk_floormod(k.tf + i, mw)] = 1;
            const int partL = uy - k.tf * tp;
            const int partR = (int)((k.tf + k.nvis) * tp - (uy + W));
            /* mark what is on screen now (and check old predictions) */
            for (int i = 0; i < k.nvis; i++) {
                int32_t ut = k.tf + i;
                int idx = tr * mw + wk_floormod(ut, mw);
                if (s->pred_ep[idx] == w->ep8) {
                    if (s->pred_wc[idx] == (int16_t)ut) {
                        out->hs_checked++;
                        if (s->pred_hash[idx] != L->hash[idx]) {
                            out->hs_missed++;
                            s->chain_ban = cfg.chain_ban_frames;
                        }
                    }
                    s->pred_ep[idx] = 0;
                }
                s->seen_hash[idx] = L->hash[idx];
                s->seen_wc[idx] = (int16_t)ut;
                s->seen_ep[idx] = w->ep8;
            }
            k.fresh_edge = 0;
            if (L->stamp) {
                int64_t ea = (int64_t)f->frame -
                             (int64_t)L->stamp[tr * mw + wk_floormod(k.tf, mw)];
                int64_t eb = (int64_t)f->frame -
                             (int64_t)L->stamp[tr * mw + wk_floormod(k.tf + k.nvis - 1, mw)];
                k.fresh_edge = (ea <= 2) | ((eb <= 2) << 1);
            }
            int nL, nR;
            int fe = k.fresh_edge;
            /* the side the camera moved towards gets the first pick of
             * physical slots */
            /* A lead can only be trusted on the side the camera moves towards:
             * a page or tile loader also overwrites slots BEHIND the camera,
             * and those fresh stamps are not a lead. */
            const int sR = L->stamp != NULL && s->dir > 0;
            const int sL = L->stamp != NULL && s->dir < 0;
            if (s->dir >= 0) {
                k.fresh_edge = (fe >> 1) & 1;
                nR = walk_side(k, +1, partR, sc, sR);
                k.fresh_edge = fe & 1;
                nL = walk_side(k, -1, partL, sc, sL);
            } else {
                k.fresh_edge = fe & 1;
                nL = walk_side(k, -1, partL, sc, sL);
                k.fresh_edge = (fe >> 1) & 1;
                nR = walk_side(k, +1, partR, sc, sR);
            }
            /* a ring narrower than the view (NES one-screen / horizontal
             * mirroring: 32 tiles, 33 visible with fine scroll) gives a
             * negative part: the edge column's slot is also the other edge's,
             * so nothing beyond it is ring-backed  [NES adapter fix] */
            int rl = wk_clamp(partL + tp * nL, 0, M);
            int rr = wk_clamp(partR + tp * nR, 0, M);
            if (getenv("WKDBG2") && f->frame == (uint32_t)atoi(getenv("WKDBG2")) && l == 0 && tr == 12) fprintf(stderr, "walk l=%d y=%d tr=%d nL=%d nR=%d rl=%d rr=%d dir=%d partL=%d partR=%d\n", l, y, tr, nL, nR, rl, rr, s->dir, partL, partR);
            run_valid = 1;
            run_sx = uy;
            run_tr = tr;
            run_back = 0;
            run_real[0] = (uint16_t)rl;
            run_real[1] = (uint16_t)rr;
            lo->real[0][l] = lo->ring[0][l] = (uint16_t)rl;
            lo->real[1][l] = lo->ring[1][l] = (uint16_t)rr;
        }

        /* kind / confidence */
        if (role == WK_ROLE_HUD || (non > 0 && nh * 2 > non))
            s->kind = WK_KIND_HUD;
        else if (role == WK_ROLE_BACKDROP || (non > 0 && nb * 2 > non))
            s->kind = WK_KIND_BACKDROP;
        else if (role == WK_ROLE_LEVEL || s->stream_ema > 8)
            s->kind = WK_KIND_LEVEL;
        else
            s->kind = WK_KIND_UNKNOWN;
        {
            uint32_t c = s->quiet_travel * 255u / (uint32_t)(cfg.quiet_px > 0 ? cfg.quiet_px : 1);
            s->conf = c > 255 ? 255 : (int)c;
            if (role != WK_ROLE_AUTO)
                s->conf = 255;
        }
        out->kind[l] = s->kind;
        out->conf[l] = s->conf;
    }

    /* Profile hud ranges and margin limits; line level summaries. */
    for (int y = 0; y < H; y++) {
        wk_line_out *lo = &out->line[y];
        if (pf) {
            for (int i = 0; i < pf->nhud; i++)
                if (y >= pf->hud_lo[i] && y <= pf->hud_hi[i]) {
                    line_hud[y] = 1;
                    line_has_hud[y] = 1;
                    memset(lo->real, 0, sizeof lo->real);
                    memset(lo->ring, 0, sizeof lo->ring);
                    lo->level_mask = 0;
                    lo->backdrop_mask = 0;
                }
            if (pf->margin_left >= 0 || pf->margin_right >= 0) {
                for (int l = 0; l < WK_MAX_LAYERS; l++) {
                    if (pf->margin_left >= 0) {
                        lo->real[0][l] = (uint16_t)wk_min(lo->real[0][l], pf->margin_left);
                        lo->ring[0][l] = (uint16_t)wk_min(lo->ring[0][l], pf->margin_left);
                    }
                    if (pf->margin_right >= 0) {
                        lo->real[1][l] = (uint16_t)wk_min(lo->real[1][l], pf->margin_right);
                        lo->ring[1][l] = (uint16_t)wk_min(lo->ring[1][l], pf->margin_right);
                    }
                }
            }
        }
        wk_out_refresh_line(out, y);
        if (pf) {
            if (pf->margin_left >= 0) lo->level_real[0] = (uint16_t)wk_min(lo->level_real[0], pf->margin_left);
            if (pf->margin_right >= 0) lo->level_real[1] = (uint16_t)wk_min(lo->level_real[1], pf->margin_right);
        }
        lo->hud = (uint8_t)(line_has_hud[y] && lo->level_mask == 0);
        if (lo->hud) {
            hud_total++;
            lo->level_real[0] = lo->level_real[1] = 0;
        }
        (void)line_hud;
    }
    out->hud_lines = hud_total;

    /* Line groups. */
    {
        uint32_t prev = 0;
        int g = 0;
        for (int y = 0; y < H; y++) {
            uint32_t sig = 0x1234567u;
            for (int l = 0; l < nl; l++) {
                if (!sc.valid[l])
                    continue;
                sig = wk_hash2(sig, (uint32_t)f->layer[l].scroll_x[y]);
                sig = wk_hash2(sig, (uint32_t)f->layer[l].row_y[y] - (uint32_t)y);
                sig = wk_hash2(sig, (uint32_t)(on(&f->layer[l], y)) |
                                        ((uint32_t)out->coherent[l][y] << 1) |
                                        ((uint32_t)w->ls[l].hud[y] << 2));
            }
            sig = wk_hash2(sig, out->line[y].backdrop_mask);
            if (y > 0 && sig != prev)
                g++;
            prev = sig;
            out->line[y].group = (uint8_t)g;
        }
        out->ngroups = g + 1;
    }
}
