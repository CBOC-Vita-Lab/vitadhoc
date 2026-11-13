/* widekit - MIT licence.  wk_map: see ../include/wk_map.h. */

#include "wk_map.h"
#include "wk_layers.h"
#include "wk_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct Entry {
    uint64_t key;     /* room << 32 | local; 0 = unused */
    uint32_t code;
    uint16_t h16, gfx;
    uint8_t conf;
    uint8_t scout;    /* 0: seen in the live picture; else the scout generation that recorded it (not yet seen live) */
    uint8_t pad[2];
};

struct Room {
    uint32_t id;       /* 0 = free slot */
    uint32_t last_used;
    uint32_t ncells;
};

const int kMaxVis = 8192;

struct Vis {
    uint32_t local;
    uint32_t code;
    uint16_t h16, gfx;
};

inline uint32_t local_key(int layer, int32_t col, int32_t row, int *ok)
{
    if (col < -32768 || col > 32767 || row < -8192 || row > 8191) {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return ((uint32_t)layer << 30) | ((uint32_t)(col + 32768) << 14) |
           (uint32_t)(row + 8192);
}

inline uint16_t fold16(uint32_t v) { return (uint16_t)((v ^ (v >> 16)) & 0xFFFF); }

}  // namespace

struct wk_map {
    wk_map_cfg cfg;
    uint32_t game_crc;
    Entry *pool;
    uint32_t npool;
    uint32_t *tab;
    uint32_t tabmask;
    Room *rooms;
    uint32_t next_room, clock;
    /* scene state */
    uint32_t epoch_seen;
    int have_epoch;
    uint32_t cur_room;
    int bound, fresh;
    int since_event, tries, last_try;
    int miss;
    wk_map_stats st;
    Vis *vis;
    uint8_t scout_gen;         /* scouted cells of other generations are never served */
};

void wk_map_cfg_default(wk_map_cfg *c)
{
    c->max_cells = 65536;
    c->max_rooms = 64;
    c->settle_frames = 2;
    c->rematch_tries = 4;
    c->rematch_gap = 4;
    c->match_pct = 85;
    c->found_pct = 40;
    c->verify_pct = 90;
    c->revoke_frames = 3;
    c->min_conf = 1;
    c->file_cap = 262144;
    c->min_room_cells = 48;
}

wk_map *wk_map_create(const wk_map_cfg *cfg, uint32_t game_crc)
{
    wk_map *m = (wk_map *)calloc(1, sizeof *m);
    if (!m)
        return NULL;
    if (cfg)
        m->cfg = *cfg;
    else
        wk_map_cfg_default(&m->cfg);
    if (m->cfg.max_cells < 256) m->cfg.max_cells = 256;
    if (m->cfg.max_rooms < 2) m->cfg.max_rooms = 2;
    m->game_crc = game_crc;
    uint32_t tsz = 1;
    while (tsz < (uint32_t)m->cfg.max_cells * 2u)
        tsz <<= 1;
    m->tabmask = tsz - 1;
    m->pool = (Entry *)calloc((size_t)m->cfg.max_cells, sizeof(Entry));
    m->tab = (uint32_t *)calloc(tsz, 4);
    m->rooms = (Room *)calloc((size_t)m->cfg.max_rooms, sizeof(Room));
    m->vis = (Vis *)malloc(sizeof(Vis) * kMaxVis);
    if (!m->pool || !m->tab || !m->rooms || !m->vis) {
        wk_map_destroy(m);
        return NULL;
    }
    m->next_room = 1;
    m->clock = 1;
    m->scout_gen = 1;
    return m;
}

void wk_map_destroy(wk_map *m)
{
    if (!m)
        return;
    free(m->pool);
    free(m->tab);
    free(m->rooms);
    free(m->vis);
    free(m);
}

void wk_map_clear(wk_map *m)
{
    memset(m->pool, 0, sizeof(Entry) * (size_t)m->cfg.max_cells);
    memset(m->tab, 0, sizeof(uint32_t) * (size_t)(m->tabmask + 1));
    memset(m->rooms, 0, sizeof(Room) * (size_t)m->cfg.max_rooms);
    m->npool = 0;
    m->next_room = 1;
    m->clock = 1;
    m->cur_room = 0;
    m->bound = m->fresh = 0;
    m->have_epoch = 0;
    memset(&m->st, 0, sizeof m->st);
}

/* ----- hash table --------------------------------------------------------- */

static inline uint32_t slot_of(const wk_map *m, uint64_t key)
{
    return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & m->tabmask;
}

static Entry *find(const wk_map *m, uint64_t key)
{
    uint32_t s = slot_of(m, key);
    for (;;) {
        uint32_t v = m->tab[s];
        if (!v)
            return NULL;
        Entry *e = &m->pool[v - 1];
        if (e->key == key)
            return e;
        s = (s + 1) & m->tabmask;
    }
}

static void rebuild(wk_map *m)
{
    memset(m->tab, 0, sizeof(uint32_t) * (size_t)(m->tabmask + 1));
    for (uint32_t i = 0; i < m->npool; i++) {
        uint32_t s = slot_of(m, m->pool[i].key);
        while (m->tab[s])
            s = (s + 1) & m->tabmask;
        m->tab[s] = i + 1;
    }
}

static Room *room_by_id(const wk_map *m, uint32_t id)
{
    for (int i = 0; i < m->cfg.max_rooms; i++)
        if (m->rooms[i].id == id)
            return &m->rooms[i];
    return NULL;
}

static void drop_room(wk_map *m, Room *r)
{
    uint32_t id = r->id;
    uint32_t w = 0;
    for (uint32_t i = 0; i < m->npool; i++)
        if ((uint32_t)(m->pool[i].key >> 32) != id)
            m->pool[w++] = m->pool[i];
    memset(m->pool + w, 0, sizeof(Entry) * (m->npool - w));
    m->npool = w;
    memset(r, 0, sizeof *r);
    rebuild(m);
    if (m->cur_room == id) {
        m->cur_room = 0;
        m->bound = 0;
    }
}

/* Evict the least recently used room that is not the current one. */
static int evict_lru(wk_map *m)
{
    Room *best = NULL;
    for (int i = 0; i < m->cfg.max_rooms; i++) {
        Room *r = &m->rooms[i];
        if (!r->id || r->id == m->cur_room)
            continue;
        if (!best || r->last_used < best->last_used)
            best = r;
    }
    if (!best)
        return 0;
    drop_room(m, best);
    return 1;
}

static int count_rooms(const wk_map *m)
{
    int n = 0;
    for (int i = 0; i < m->cfg.max_rooms; i++)
        n += m->rooms[i].id != 0;
    return n;
}

static Room *new_room(wk_map *m)
{
    for (;;) {
        for (int i = 0; i < m->cfg.max_rooms; i++)
            if (!m->rooms[i].id) {
                Room *r = &m->rooms[i];
                r->id = m->next_room++;
                r->last_used = m->clock++;
                r->ncells = 0;
                return r;
            }
        if (!evict_lru(m))
            return NULL;
    }
}

static Entry *insert(wk_map *m, uint64_t key)
{
    while (m->npool >= (uint32_t)m->cfg.max_cells)
        if (!evict_lru(m))
            return NULL;
    Entry *e = &m->pool[m->npool++];
    memset(e, 0, sizeof *e);
    e->key = key;
    uint32_t s = slot_of(m, key);
    while (m->tab[s])
        s = (s + 1) & m->tabmask;
    m->tab[s] = m->npool;
    Room *r = room_by_id(m, (uint32_t)(key >> 32));
    if (r)
        r->ncells++;
    return e;
}

/* ----- geometry ------------------------------------------------------------ */

void wk_line_geom(const wk_frame *f, const wk_out *out, int layer, int y, wk_geom *g)
{
    const wk_layer *L = &f->layer[layer];
    int tp = L->tile_px;
    int32_t wx = out->world_x[layer][y];
    g->tp = tp;
    g->tile_first = wk_floordiv(wx, tp);
    int32_t last = wk_floordiv(wx + f->view_w - 1, tp);
    g->nvis = (int)(last - g->tile_first + 1);
    g->part[0] = (int)(wx - g->tile_first * tp);
    g->part[1] = (int)((last + 1) * tp - (wx + f->view_w));
}

static int observed(const wk_frame *f, const wk_out *out, int l)
{
    if (l >= f->nlayers || !f->layer[l].active || !f->layer[l].hash)
        return 0;
    return out->kind[l] == WK_KIND_LEVEL || out->kind[l] == WK_KIND_UNKNOWN;
}

/* Gather the on-screen cells of every observed layer. */
static int gather(wk_map *m, const wk_frame *f, const wk_out *out)
{
    int n = 0;
    for (int l = 0; l < f->nlayers; l++) {
        if (!observed(f, out, l))
            continue;
        const wk_layer *L = &f->layer[l];
        int mw = L->map_w, mh = L->map_h, tp = L->tile_px;
        uint16_t g16 = fold16(L->gfx_id);
        int32_t prev_wx = 0, prev_row = 0;
        int have = 0;
        for (int y = 0; y < out->nlines; y++) {
            if (!out->coherent[l][y])
                continue;
            int32_t wx = out->world_x[l][y];
            int32_t wr = wk_floordiv(out->world_row[l][y], tp);
            if (have && wx == prev_wx && wr == prev_row)
                continue;
            have = 1;
            prev_wx = wx;
            prev_row = wr;
            int32_t tf = wk_floordiv(wx, tp);
            int nv = (int)(wk_floordiv(wx + f->view_w - 1, tp) - tf + 1);
            int rr = wk_floormod(wr, mh);
            for (int i = 0; i < nv && n < kMaxVis; i++) {
                int ok;
                uint32_t lk = local_key(l, tf + i, wr, &ok);
                if (!ok)
                    continue;
                int idx = rr * mw + wk_floormod(tf + i, mw);
                if (L->code && L->code[idx] == WK_CODE_NOREC)
                    continue;
                m->vis[n].local = lk;
                m->vis[n].h16 = fold16(L->hash[idx]);
                m->vis[n].code = L->code ? L->code[idx] : L->hash[idx];
                m->vis[n].gfx = g16;
                n++;
            }
        }
    }
    return n;
}

static void overlap(const wk_map *m, uint32_t room, int n, int *found, int *matched)
{
    int fo = 0, ma = 0;
    for (int i = 0; i < n; i++) {
        const Entry *e = find(m, ((uint64_t)room << 32) | m->vis[i].local);
        if (!e || e->gfx != m->vis[i].gfx || e->scout)
            continue;
        fo++;
        if (e->h16 == m->vis[i].h16)
            ma++;
    }
    *found = fo;
    *matched = ma;
}

void wk_map_update(wk_map *m, const wk_frame *f, const wk_out *out)
{
    m->st.served_tiles = 0;
    if (!m->have_epoch || out->epoch != m->epoch_seen) {
        m->have_epoch = 1;
        m->epoch_seen = out->epoch;
        m->cur_room = 0;
        m->bound = m->fresh = 0;
        m->since_event = 0;
        m->tries = 0;
        m->last_try = -1000;
        m->miss = 0;
    }
    if (out->scene_event || f->brightness <= 2) {
        m->since_event = 0;
        m->cur_room = 0;
        m->bound = m->fresh = 0;
        m->tries = 0;
        m->last_try = -1000;
        return;
    }
    m->since_event++;
    int n = gather(m, f, out);
    if (n == 0)
        return;

    if (!m->bound) {
        if (m->since_event < m->cfg.settle_frames)
            return;
        if (m->since_event - m->last_try < m->cfg.rematch_gap && m->tries > 0)
            return;
        /* try to recognise a known room at this world position */
        uint32_t best = 0;
        int best_m = 0;
        for (int i = 0; i < m->cfg.max_rooms; i++) {
            const Room *r = &m->rooms[i];
            if (!r->id)
                continue;
            int fo, ma;
            overlap(m, r->id, n, &fo, &ma);
            if (fo * 100 >= n * m->cfg.found_pct && fo >= 24 &&
                ma * 100 >= fo * m->cfg.match_pct && ma > best_m) {
                best = r->id;
                best_m = ma;
            }
        }
        m->tries++;
        m->last_try = m->since_event;
        if (best) {
            m->cur_room = best;
            m->bound = 1;
            m->fresh = 0;
            m->miss = 0;
            m->st.binds++;
            room_by_id(m, best)->last_used = m->clock++;
        } else if (m->tries >= m->cfg.rematch_tries || count_rooms(m) == 0) {
            Room *r = new_room(m);
            if (!r)
                return;
            m->cur_room = r->id;
            m->bound = 1;
            m->fresh = 1;
            m->miss = 0;
            m->st.new_rooms++;
        } else {
            return;
        }
    }

    int found, matched;
    overlap(m, m->cur_room, n, &found, &matched);
    m->st.found = found;
    m->st.matched = matched;
    if (found >= 24) {
        if (matched * 100 < found * m->cfg.verify_pct)
            m->miss++;
        else
            m->miss = 0;
    }
    if (m->miss >= m->cfg.revoke_frames) {
        /* the picture no longer matches what we stored: forget the binding,
         * try again (and open a new room if nothing matches) */
        m->st.revokes++;
        m->bound = 0;
        m->fresh = 0;
        m->cur_room = 0;
        m->tries = 0;
        m->last_try = -1000;
        m->miss = 0;
        m->since_event = m->cfg.settle_frames;
        return;
    }
    /* a frame that does not match the bound room is not recorded into it: if the
     * picture really is another room the binding is revoked a few frames later and
     * the stored cells must still be the old room's */
    if (found >= 24 && m->miss > 0)
        return;
    /* record */
    uint64_t rbase = (uint64_t)m->cur_room << 32;
    for (int i = 0; i < n; i++) {
        uint64_t key = rbase | m->vis[i].local;
        Entry *e = find(m, key);
        if (e) {
            if (e->h16 == m->vis[i].h16 && e->gfx == m->vis[i].gfx) {
                if (e->scout) {
                    /* hindsight: a scouted cell now seen live, the same */
                    if (e->scout == m->scout_gen) m->st.scout_confirmed++;
                    e->scout = 0;
                }
                if (e->conf < 255) e->conf++;
            } else {
                if (e->scout && e->scout == m->scout_gen) {
                    /* hindsight: the scout's future was not this one; nothing it
                     * recorded and the live picture has not shown yet is served */
                    m->st.scout_contradicted++;
                    m->scout_gen = (uint8_t)(m->scout_gen == 255 ? 1 : m->scout_gen + 1);
                }
                e->scout = 0;
                e->code = m->vis[i].code;
                e->h16 = m->vis[i].h16;
                e->gfx = m->vis[i].gfx;
                e->conf = 1;
            }
        } else {
            e = insert(m, key);
            if (!e)
                break;
            e->code = m->vis[i].code;
            e->h16 = m->vis[i].h16;
            e->gfx = m->vis[i].gfx;
            e->conf = 1;
        }
    }
    Room *r = room_by_id(m, m->cur_room);
    if (r)
        r->last_used = m->clock;
}

static int cell_at(const wk_map *m, const wk_frame *f, const wk_out *out, int layer,
                   int y, int side, int index, wk_cell *cell)
{
    if (!m->bound || !out->coherent[layer][y])
        return 0;
    const wk_layer *L = &f->layer[layer];
    wk_geom g;
    wk_line_geom(f, out, layer, y, &g);
    int32_t col = side == WK_LEFT ? g.tile_first - index : g.tile_first + g.nvis - 1 + index;
    int32_t row = wk_floordiv(out->world_row[layer][y], g.tp);
    int ok;
    uint32_t lk = local_key(layer, col, row, &ok);
    if (!ok)
        return 0;
    const Entry *e = find(m, ((uint64_t)m->cur_room << 32) | lk);
    if (!e || e->conf < m->cfg.min_conf || e->gfx != fold16(L->gfx_id) ||
        (e->scout && e->scout != m->scout_gen))
        return 0;
    if (cell) {
        cell->code = e->code;
        cell->gfx = e->gfx;
        cell->conf = e->conf;
        cell->scout = e->scout ? 1 : 0;
    }
    return 1;
}

int wk_map_margin_cell(const wk_map *m, const wk_frame *f, const wk_out *out, int layer,
                       int y, int side, int index, wk_cell *cell)
{
    if (layer < 0 || layer >= f->nlayers || y < 0 || y >= out->nlines)
        return 0;
    return cell_at(m, f, out, layer, y, side, index, cell);
}

void wk_map_record_scout(wk_map *m, const wk_frame *f, const wk_out *out)
{
    if (!m->bound || !m->cur_room || out->scene_event || out->epoch != m->epoch_seen)
        return;
    int n = gather(m, f, out);
    uint64_t rbase = (uint64_t)m->cur_room << 32;
    for (int i = 0; i < n; i++) {
        uint64_t key = rbase | m->vis[i].local;
        if (find(m, key))
            continue;               /* what the live picture showed (or an earlier scout frame) stays */
        Entry *e = insert(m, key);
        if (!e)
            break;
        e->code = m->vis[i].code;
        e->h16 = m->vis[i].h16;
        e->gfx = m->vis[i].gfx;
        e->conf = 1;
        e->scout = m->scout_gen;
        m->st.scout_cells++;
    }
}

void wk_map_serve(wk_map *m, const wk_frame *f, wk_out *out)
{
    if (!m->bound)
        return;
    const int M = out->margin;
    for (int l = 0; l < f->nlayers; l++) {
        if (!observed(f, out, l))
            continue;
        int32_t prev_wx = 0, prev_row = 0;
        uint16_t memo[2] = {0, 0};
        int have = 0;
        for (int y = 0; y < out->nlines; y++) {
            if (!out->coherent[l][y] || (out->line[y].backdrop_mask >> l) & 1)
                continue;
            int32_t wx = out->world_x[l][y];
            int32_t wr = out->world_row[l][y];
            if (have && wx == prev_wx && wr == prev_row) {
                /* same tile row and scroll as the previous served line */
                for (int s = 0; s < 2; s++)
                    if (memo[s] > out->line[y].real[s][l])
                        wk_out_raise(out, l, y, s, memo[s]);
                continue;
            }
            have = 1;
            prev_wx = wx;
            prev_row = wr;
            wk_geom g;
            wk_line_geom(f, out, l, y, &g);
            for (int s = 0; s < 2; s++) {
                int ring = out->line[y].ring[s][l];
                int part = g.part[s];
                int n = ring >= part ? (ring - part) / g.tp : 0;
                int real = out->line[y].real[s][l];
                if (real < M) {
                    int n0 = n;
                    while (part + g.tp * n < M &&
                           cell_at(m, f, out, l, y, s, n + 1, NULL))
                        n++;
                    if (n > n0) {
                        int px = wk_min(M, part + g.tp * n);
                        if (px > real) {
                            wk_out_raise(out, l, y, s, px);
                            m->st.served_tiles += (uint32_t)(n - n0);
                        }
                    }
                }
                memo[s] = out->line[y].real[s][l];
            }
        }
    }
}

void wk_map_stats_get(const wk_map *m, wk_map_stats *s)
{
    *s = m->st;
    s->cells = (int)m->npool;
    int nr = 0;
    for (int i = 0; i < m->cfg.max_rooms; i++)
        nr += m->rooms[i].id != 0;
    s->rooms = nr;
    s->cur_room = (int)m->cur_room;
    s->bound = m->bound;
    s->fresh = m->fresh;
}

/* ----- persistence ---------------------------------------------------------- */

static const char kMagic[4] = {'W', 'K', 'M', '1'};
const size_t kHdr = 40;

static size_t put_var(uint8_t *p, uint64_t v)
{
    size_t n = 0;
    while (v >= 0x80) {
        p[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    p[n++] = (uint8_t)v;
    return n;
}

static int get_var(const uint8_t *&p, const uint8_t *end, uint64_t *v)
{
    uint64_t r = 0;
    int shift = 0;
    while (p < end && shift <= 63) {
        uint8_t b = *p++;
        r |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *v = r;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

size_t wk_map_serialize_max(const wk_map *m) { return (size_t)m->cfg.file_cap + kHdr + 64; }

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = ((const uint32_t *)a)[0], y = ((const uint32_t *)b)[0];
    return x < y ? -1 : (x > y);
}

struct Pair { uint32_t local; uint32_t idx; };

static int cmp_pair(const void *a, const void *b)
{
    return cmp_u32(a, b);
}

size_t wk_map_serialize(const wk_map *m, uint8_t *buf, size_t cap)
{
    if (cap < kHdr + 16)
        return 0;
    /* rooms, most recently used first */
    int order[1024];
    int nr = 0;
    for (int i = 0; i < m->cfg.max_rooms && nr < 1024; i++)
        if (m->rooms[i].id && (int)m->rooms[i].ncells >= m->cfg.min_room_cells)
            order[nr++] = i;
    for (int a = 1; a < nr; a++) {
        int v = order[a], b = a - 1;
        while (b >= 0 && m->rooms[order[b]].last_used < m->rooms[v].last_used) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = v;
    }
    size_t limit = (size_t)m->cfg.file_cap;
    if (limit + kHdr > cap)
        limit = cap - kHdr;
    uint8_t *payload = buf + kHdr;
    size_t plen = 0;
    uint32_t wrooms = 0, wcells = 0;
    Pair *pairs = (Pair *)malloc(sizeof(Pair) * (size_t)(m->npool ? m->npool : 1));
    uint8_t *scratch = (uint8_t *)malloc((size_t)m->npool * 14 + 64);
    if (!pairs || !scratch) {
        free(pairs);
        free(scratch);
        return 0;
    }
    for (int oi = 0; oi < nr; oi++) {
        const Room *r = &m->rooms[order[oi]];
        uint32_t np = 0;
        for (uint32_t i = 0; i < m->npool; i++)
            if ((uint32_t)(m->pool[i].key >> 32) == r->id && !m->pool[i].scout) {
                pairs[np].local = (uint32_t)m->pool[i].key;
                pairs[np].idx = i;
                np++;
            }
        qsort(pairs, np, sizeof(Pair), cmp_pair);
        size_t n = 0;
        n += put_var(scratch + n, r->id);
        n += put_var(scratch + n, r->last_used);
        n += put_var(scratch + n, np);
        uint32_t prev = 0;
        for (uint32_t i = 0; i < np; i++) {
            const Entry &e = m->pool[pairs[i].idx];
            n += put_var(scratch + n, (uint64_t)(pairs[i].local - prev));
            prev = pairs[i].local;
            n += put_var(scratch + n, e.code);
            scratch[n++] = (uint8_t)e.h16;
            scratch[n++] = (uint8_t)(e.h16 >> 8);
            n += put_var(scratch + n, e.gfx);
            scratch[n++] = e.conf;
        }
        if (plen + n > limit)
            continue;           /* evicted: older rooms may still fit */
        memcpy(payload + plen, scratch, n);
        plen += n;
        wrooms++;
        wcells += np;
    }
    free(pairs);
    free(scratch);
    memset(buf, 0, kHdr);
    memcpy(buf, kMagic, 4);
    buf[4] = 1; buf[5] = 0;                 /* version */
    buf[6] = (uint8_t)kHdr; buf[7] = 0;     /* header size */
    put32(buf + 8, m->game_crc);
    put32(buf + 12, wrooms);
    put32(buf + 16, wcells);
    put32(buf + 20, (uint32_t)plen);
    put32(buf + 24, wk_crc32(0, payload, plen));
    put32(buf + 28, 0);
    put32(buf + 32, 0);
    put32(buf + 36, wk_crc32(0, buf, 36));
    return kHdr + plen;
}

static int parse_payload(wk_map *m, const uint8_t *p, const uint8_t *end, uint32_t nrooms,
                         uint32_t ncells, uint32_t *max_id, uint32_t *max_used)
{
    uint32_t total = 0;
    for (uint32_t r = 0; r < nrooms; r++) {
        uint64_t id, used, np;
        if (!get_var(p, end, &id) || !get_var(p, end, &used) || !get_var(p, end, &np))
            return 0;
        if (id == 0 || id > 0x7FFFFFFF || used > 0x7FFFFFFF || total + np > ncells ||
            room_by_id(m, (uint32_t)id))
            return 0;
        Room *room = NULL;
        for (int i = 0; i < m->cfg.max_rooms; i++)
            if (!m->rooms[i].id) {
                room = &m->rooms[i];
                break;
            }
        if (!room)
            return 0;
        room->id = (uint32_t)id;
        room->last_used = (uint32_t)used;
        room->ncells = 0;
        uint32_t local = 0;
        for (uint64_t i = 0; i < np; i++) {
            uint64_t d, code, gfx;
            if (!get_var(p, end, &d) || !get_var(p, end, &code) || end - p < 2)
                return 0;
            uint16_t h16 = (uint16_t)(p[0] | p[1] << 8);
            p += 2;
            if (!get_var(p, end, &gfx) || p >= end || gfx > 0xFFFF || code > 0xFFFFFFFFull)
                return 0;
            uint8_t conf = *p++;
            local += (uint32_t)d;
            Entry *e = insert(m, ((uint64_t)id << 32) | local);
            if (!e)
                return 0;
            e->code = (uint32_t)code;
            e->h16 = h16;
            e->gfx = (uint16_t)gfx;
            e->conf = conf ? conf : 1;
        }
        total += (uint32_t)np;
        if ((uint32_t)id > *max_id) *max_id = (uint32_t)id;
        if ((uint32_t)used > *max_used) *max_used = (uint32_t)used;
    }
    return p == end && total == ncells;
}

int wk_map_deserialize(wk_map *m, const uint8_t *buf, size_t len)
{
    wk_map_clear(m);
    if (len < kHdr || memcmp(buf, kMagic, 4))
        return -1;
    if (buf[4] != 1 || buf[5] != 0 || buf[6] != kHdr || buf[7] != 0)
        return -2;
    if (get32(buf + 36) != wk_crc32(0, buf, 36))
        return -3;
    if (get32(buf + 8) != m->game_crc)
        return -4;
    uint32_t nrooms = get32(buf + 12), ncells = get32(buf + 16), plen = get32(buf + 20);
    if (plen != len - kHdr || nrooms > (uint32_t)m->cfg.max_rooms ||
        ncells > (uint32_t)m->cfg.max_cells)
        return -5;
    if (get32(buf + 24) != wk_crc32(0, buf + kHdr, plen))
        return -6;
    uint32_t max_id = 0, max_used = 0;
    if (!parse_payload(m, buf + kHdr, buf + len, nrooms, ncells, &max_id, &max_used)) {
        wk_map_clear(m);
        return -7;
    }
    m->next_room = max_id + 1;
    m->clock = max_used + 1;
    return 0;
}

void wk_map_path(const char *dir, uint32_t game_crc, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s%08X.wkm", dir ? dir : "",
             (dir && *dir && dir[strlen(dir) - 1] != '/' && dir[strlen(dir) - 1] != ':') ? "/" : "",
             (unsigned)game_crc);
}

int wk_map_save(const wk_map *m, const char *path)
{
    size_t cap = wk_map_serialize_max(m);
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf)
        return -1;
    size_t n = wk_map_serialize(m, buf, cap);
    if (!n) {
        free(buf);
        return -1;
    }
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
        free(buf);
        return -1;
    }
    int rc = 0;
    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        free(buf);
        return -2;
    }
    if (fwrite(buf, 1, n, fp) != n)
        rc = -2;
    if (fflush(fp) != 0)
        rc = -2;
    if (fclose(fp) != 0)
        rc = -2;
    if (rc == 0) {
        /* read back: size and CRC of the whole file must be what we meant */
        fp = fopen(tmp, "rb");
        uint8_t *chk = (uint8_t *)malloc(n + 1);
        if (!fp || !chk) {
            rc = -3;
        } else {
            size_t got = fread(chk, 1, n + 1, fp);
            if (got != n || wk_crc32(0, chk, got) != wk_crc32(0, buf, n) ||
                memcmp(chk, buf, n))
                rc = -3;
        }
        if (fp) fclose(fp);
        free(chk);
    }
    if (rc == 0 && rename(tmp, path) != 0) {
        /* some platforms refuse to rename over an existing file */
        remove(path);
        if (rename(tmp, path) != 0)
            rc = -4;
    }
    if (rc != 0)
        remove(tmp);
    free(buf);
    return rc;
}

int wk_map_load(wk_map *m, const char *path)
{
    wk_map_clear(m);
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -10;
    size_t cap = wk_map_serialize_max(m) + 1;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        fclose(fp);
        return -11;
    }
    size_t n = fread(buf, 1, cap, fp);
    fclose(fp);
    int rc = n >= cap ? -12 : wk_map_deserialize(m, buf, n);
    free(buf);
    return rc;
}
