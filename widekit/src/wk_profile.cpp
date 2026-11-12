/* widekit - MIT licence.  wk_profile: parser, validation, level-map reader. */

#include "wk_profile.h"
#include "wk_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct wk_profile_db {
    wk_profile *items;
    int n, cap;
};

void wk_profile_default(wk_profile *p)
{
    memset(p, 0, sizeof *p);
    p->enable = WK_ENABLE_AUTO;
    for (int i = 0; i < WK_MAX_LAYERS; i++)
        p->role[i] = WK_ROLE_AUTO;
    p->margin_left = p->margin_right = -1;
    p->lead_tiles = -1;
    p->fade_px = -1;
}

namespace {

struct Msgs {
    char *buf;
    size_t cap, len;
    int count;
};

void report(Msgs &m, int line, const char *fmt, const char *arg)
{
    m.count++;
    if (!m.buf || m.cap == 0)
        return;
    char tmp[200];
    int n = snprintf(tmp, sizeof tmp, "line %d: ", line);
    n += snprintf(tmp + n, sizeof tmp - (size_t)n, fmt, arg ? arg : "");
    if (n < 0)
        return;
    if (n > (int)sizeof tmp - 2)
        n = (int)sizeof tmp - 2;
    tmp[n++] = '\n';
    for (int i = 0; i < n && m.len + 1 < m.cap; i++)
        m.buf[m.len++] = tmp[i];
    m.buf[m.len] = 0;
}

char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

/* portable strtok_r */
char *next_tok(char **save, const char *delims)
{
    char *s = *save;
    if (!s)
        return NULL;
    while (*s && strchr(delims, *s))
        s++;
    if (!*s) {
        *save = NULL;
        return NULL;
    }
    char *t = s;
    while (*s && !strchr(delims, *s))
        s++;
    if (*s)
        *s++ = 0;
    *save = s;
    return t;
}

int ieq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
        if (!a[i])
            return 1;
    }
    return 1;
}

int parse_int(const char *s, long lo, long hi, long *out)
{
    if (!*s)
        return 0;
    char *end;
    long v = strtol(s, &end, 0);
    if (*end || v < lo || v > hi)
        return 0;
    *out = v;
    return 1;
}

int parse_role(const char *v, int *out)
{
    if (ieq(v, "auto")) *out = WK_ROLE_AUTO;
    else if (ieq(v, "backdrop")) *out = WK_ROLE_BACKDROP;
    else if (ieq(v, "level")) *out = WK_ROLE_LEVEL;
    else if (ieq(v, "hud")) *out = WK_ROLE_HUD;
    else if (ieq(v, "off")) *out = WK_ROLE_OFF;
    else return 0;
    return 1;
}

/* "0-31,208-223" or "12" */
int parse_ranges(const char *v, wk_profile *p)
{
    char tmp[96];
    if (strlen(v) >= sizeof tmp)
        return 0;
    strcpy(tmp, v);
    int n = 0;
    int lo_[WK_HUD_RANGES], hi_[WK_HUD_RANGES];
    char *save = tmp;
    for (char *t = next_tok(&save, ","); t; t = next_tok(&save, ",")) {
        if (n >= WK_HUD_RANGES)
            return 0;
        t = trim(t);
        char *dash = strchr(t, '-');
        long a, b;
        if (dash) {
            *dash = 0;
            if (!parse_int(trim(t), 0, WK_MAX_LINES - 1, &a) ||
                !parse_int(trim(dash + 1), 0, WK_MAX_LINES - 1, &b))
                return 0;
        } else {
            if (!parse_int(t, 0, WK_MAX_LINES - 1, &a))
                return 0;
            b = a;
        }
        if (b < a)
            return 0;
        lo_[n] = (int)a;
        hi_[n] = (int)b;
        n++;
    }
    if (n == 0)
        return 0;
    p->nhud = n;
    for (int i = 0; i < n; i++) {
        p->hud_lo[i] = lo_[i];
        p->hud_hi[i] = hi_[i];
    }
    return 1;
}

/* tiles=table:0=0x10,0x11|1=0x20,0x21   (no spaces) */
int parse_table(const char *v, wk_levelmap *lm)
{
    char tmp[1024];
    if (strlen(v) >= sizeof tmp)
        return 0;
    strcpy(tmp, v);
    int n = 0;
    int per = lm->meta_w * lm->meta_h;
    char *save1 = tmp;
    for (char *ent = next_tok(&save1, "|"); ent; ent = next_tok(&save1, "|")) {
        if (n >= WK_LM_TABLE)
            return 0;
        char *eq = strchr(ent, '=');
        if (!eq)
            return 0;
        *eq = 0;
        long key;
        if (!parse_int(ent, 0, 65535, &key))
            return 0;
        int i = 0;
        char *save2 = eq + 1;
        for (char *t = next_tok(&save2, ","); t; t = next_tok(&save2, ",")) {
            long tv;
            if (i >= per || !parse_int(t, 0, 0x7FFFFFFF, &tv))
                return 0;
            lm->table_tile[n][i++] = (uint32_t)tv;
        }
        if (i != per)
            return 0;
        lm->table_key[n] = (uint16_t)key;
        n++;
    }
    lm->table_n = n;
    return n > 0;
}

/* One "levelmap = k=v k=v ..." line. */
int parse_levelmap(char *v, wk_levelmap *lm, const char **why)
{
    memset(lm, 0, sizeof *lm);
    lm->bytes = 1;
    lm->cell_px = 16;
    lm->meta_w = lm->meta_h = 1;
    char tiles[1024] = "";
    int have_addr = 0, have_w = 0, have_h = 0;
    char *save = v;
    for (char *tok = next_tok(&save, " \t"); tok; tok = next_tok(&save, " \t")) {
        char *eq = strchr(tok, '=');
        if (!eq) { *why = "token without ="; return 0; }
        *eq = 0;
        const char *k = tok, *val = eq + 1;
        long x, y;
        if (ieq(k, "layer")) {
            if (!parse_int(val, 0, WK_MAX_LAYERS - 1, &x)) { *why = "layer 0..3"; return 0; }
            lm->layer = (int)x;
        } else if (ieq(k, "addr")) {
            if (!parse_int(val, 0, 0x7FFFFFFF, &x)) { *why = "bad addr"; return 0; }
            lm->addr = (uint32_t)x; have_addr = 1;
        } else if (ieq(k, "w")) {
            if (!parse_int(val, 1, 4096, &x)) { *why = "w 1..4096"; return 0; }
            lm->w = (int)x; have_w = 1;
        } else if (ieq(k, "h")) {
            if (!parse_int(val, 1, 4096, &x)) { *why = "h 1..4096"; return 0; }
            lm->h = (int)x; have_h = 1;
        } else if (ieq(k, "cell")) {
            if (!parse_int(val, 4, 64, &x)) { *why = "cell 4..64"; return 0; }
            lm->cell_px = (int)x;
        } else if (ieq(k, "bytes")) {
            if (!parse_int(val, 1, 2, &x)) { *why = "bytes 1 or 2"; return 0; }
            lm->bytes = (int)x;
        } else if (ieq(k, "endian")) {
            if (ieq(val, "little")) lm->big_endian = 0;
            else if (ieq(val, "big")) lm->big_endian = 1;
            else { *why = "endian little|big"; return 0; }
        } else if (ieq(k, "stride")) {
            if (!parse_int(val, 0, 1 << 20, &x)) { *why = "bad stride"; return 0; }
            lm->stride = (int)x;
        } else if (ieq(k, "order")) {
            if (ieq(val, "rows")) lm->column_major = 0;
            else if (ieq(val, "columns")) lm->column_major = 1;
            else { *why = "order rows|columns"; return 0; }
        } else if (ieq(k, "origin")) {
            char t2[48];
            if (strlen(val) >= sizeof t2) { *why = "bad origin"; return 0; }
            strcpy(t2, val);
            char *c = strchr(t2, ',');
            if (!c) { *why = "origin x,y"; return 0; }
            *c = 0;
            if (!parse_int(t2, -1000000, 1000000, &x) ||
                !parse_int(c + 1, -1000000, 1000000, &y)) { *why = "bad origin"; return 0; }
            lm->origin_x = (int)x;
            lm->origin_y = (int)y;
        } else if (ieq(k, "meta")) {
            int a, b;
            if (sscanf(val, "%dx%d", &a, &b) != 2 || a < 1 || b < 1 || a * b > WK_LM_META) {
                *why = "meta WxH, at most 16 tiles";
                return 0;
            }
            lm->meta_w = a;
            lm->meta_h = b;
        } else if (ieq(k, "tiles")) {
            if (strlen(val) >= sizeof tiles) { *why = "tiles too long"; return 0; }
            strcpy(tiles, val);
        } else {
            *why = "unknown levelmap key";
            return 0;
        }
    }
    if (!have_addr || !have_w || !have_h) { *why = "addr, w and h are required"; return 0; }
    if (lm->cell_px % lm->meta_w || lm->cell_px % lm->meta_h) {
        *why = "cell must be a multiple of the meta tile grid";
        return 0;
    }
    if (!tiles[0]) { *why = "tiles= required"; return 0; }
    if (!strncmp(tiles, "linear:", 7)) {
        long b, m;
        char *c = strchr(tiles + 7, ',');
        if (!c) { *why = "tiles=linear:base,mul"; return 0; }
        *c = 0;
        if (!parse_int(tiles + 7, 0, 0x7FFFFFFF, &b) || !parse_int(c + 1, 0, 4096, &m)) {
            *why = "bad linear tiles";
            return 0;
        }
        lm->map_kind = 0;
        lm->tile_base = (uint32_t)b;
        lm->tile_mul = (uint32_t)m;
    } else if (!strncmp(tiles, "table:", 6)) {
        lm->map_kind = 1;
        if (!parse_table(tiles + 6, lm)) { *why = "bad tile table"; return 0; }
    } else {
        *why = "tiles= linear:|table:";
        return 0;
    }
    return 1;
}

int db_add(wk_profile_db *db, const wk_profile &p)
{
    if (db->n == db->cap) {
        int nc = db->cap ? db->cap * 2 : 16;
        wk_profile *ni = (wk_profile *)realloc(db->items, (size_t)nc * sizeof *ni);
        if (!ni)
            return 0;
        db->items = ni;
        db->cap = nc;
    }
    db->items[db->n++] = p;
    return 1;
}

/* [crc32=XXXXXXXX title="NAME"] */
int parse_header(char *h, wk_profile *p, const char **why)
{
    wk_profile_default(p);
    char *s = h;
    while (*s) {
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;
        char *k = s;
        while (*s && *s != '=' && !isspace((unsigned char)*s)) s++;
        if (*s != '=') { *why = "header token without ="; return 0; }
        *s++ = 0;
        char *v;
        if (*s == '"') {
            v = ++s;
            while (*s && *s != '"') s++;
            if (*s != '"') { *why = "unterminated title"; return 0; }
            *s++ = 0;
        } else {
            v = s;
            while (*s && !isspace((unsigned char)*s)) s++;
            if (*s) *s++ = 0;
        }
        if (ieq(k, "crc32") || ieq(k, "crc")) {
            char *end;
            unsigned long c = strtoul(v, &end, 16);
            if (*end || !*v || strlen(v) > 8) { *why = "crc32 must be 1-8 hex digits"; return 0; }
            p->crc = (uint32_t)c;
            p->has_crc = 1;
        } else if (ieq(k, "title")) {
            if (strlen(v) >= sizeof p->title) { *why = "title too long"; return 0; }
            strcpy(p->title, v);
        } else {
            *why = "unknown header key";
            return 0;
        }
    }
    if (!p->has_crc && !p->title[0]) { *why = "section needs crc32= or title="; return 0; }
    return 1;
}

}  // namespace

wk_profile_db *wk_profile_parse(const char *text, size_t len, char *msgs,
                                size_t msgcap, int *nproblems)
{
    wk_profile_db *db = (wk_profile_db *)calloc(1, sizeof *db);
    if (!db)
        return NULL;
    Msgs m = {msgs, msgcap, 0, 0};
    if (msgs && msgcap)
        msgs[0] = 0;
    if (!text)
        len = 0;
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        free(db);
        return NULL;
    }
    memcpy(buf, text, len);
    buf[len] = 0;

    wk_profile cur;
    int in_section = 0, skip = 0;
    int lineno = 0;
    /* split on \n manually so empty lines keep their numbers */
    char *p = buf;
    while (p && *p) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = 0;
        char *line = p;
        p = eol ? eol + 1 : NULL;
        lineno++;
        /* '#' starts a comment anywhere; ';' only as the first character */
        {
            char *c = strchr(line, '#');
            if (c)
                *c = 0;
            char *t = line;
            while (*t && isspace((unsigned char)*t))
                t++;
            if (*t == ';')
                *t = 0;
        }
        line = trim(line);
        if (!*line)
            continue;
        if (*line == '[') {
            if (in_section && !skip)
                db_add(db, cur);
            in_section = 1;
            skip = 0;
            char *close = strchr(line, ']');
            const char *why = "";
            if (!close || close[1]) {
                report(m, lineno, "bad section header%s", "");
                skip = 1;
                continue;
            }
            *close = 0;
            if (!parse_header(line + 1, &cur, &why)) {
                report(m, lineno, "section skipped: %s", why);
                skip = 1;
            }
            continue;
        }
        if (!in_section) {
            report(m, lineno, "setting outside a section%s", "");
            continue;
        }
        if (skip)
            continue;
        char *eq = strchr(line, '=');
        if (!eq) {
            report(m, lineno, "expected key = value%s", "");
            continue;
        }
        *eq = 0;
        char *key = trim(line);
        char *val = trim(eq + 1);
        long x;
        int role;
        int bgn = -1;
        if ((ieq_n(key, "bg", 2) || ieq_n(key, "layer", 5)) &&
            isdigit((unsigned char)key[strlen(key) - 1]) && strlen(key) <= 6) {
            bgn = key[strlen(key) - 1] - '0';
            if (ieq_n(key, "bg", 2))
                bgn -= 1;
        }
        if (bgn >= 0) {
            if (bgn >= WK_MAX_LAYERS || !parse_role(val, &role)) {
                report(m, lineno, "bad layer role '%s'", val);
                continue;
            }
            cur.role[bgn] = role;
        } else if (ieq(key, "enable")) {
            if (ieq(val, "auto")) cur.enable = WK_ENABLE_AUTO;
            else if (ieq(val, "off")) cur.enable = WK_ENABLE_OFF;
            else if (ieq(val, "force")) cur.enable = WK_ENABLE_FORCE;
            else report(m, lineno, "enable auto|off|force, got '%s'", val);
        } else if (ieq(key, "margin")) {
            if (parse_int(val, 0, WK_MAX_MARGIN, &x)) cur.margin_left = cur.margin_right = (int)x;
            else report(m, lineno, "margin 0..128, got '%s'", val);
        } else if (ieq(key, "margin_left")) {
            if (parse_int(val, 0, WK_MAX_MARGIN, &x)) cur.margin_left = (int)x;
            else report(m, lineno, "margin_left 0..128, got '%s'", val);
        } else if (ieq(key, "margin_right")) {
            if (parse_int(val, 0, WK_MAX_MARGIN, &x)) cur.margin_right = (int)x;
            else report(m, lineno, "margin_right 0..128, got '%s'", val);
        } else if (ieq(key, "lead_tiles")) {
            if (parse_int(val, 0, 32, &x)) cur.lead_tiles = (int)x;
            else report(m, lineno, "lead_tiles 0..32, got '%s'", val);
        } else if (ieq(key, "fade_px")) {
            if (parse_int(val, 0, 64, &x)) cur.fade_px = (int)x;
            else report(m, lineno, "fade_px 0..64, got '%s'", val);
        } else if (ieq(key, "sprites")) {
            if (ieq(val, "existing")) cur.sprites_off = 0;
            else if (ieq(val, "off")) cur.sprites_off = 1;
            else report(m, lineno, "sprites existing|off, got '%s'", val);
        } else if (ieq(key, "hud_lines")) {
            if (!parse_ranges(val, &cur))
                report(m, lineno, "hud_lines like 0-31,208-223, got '%s'", val);
        } else if (ieq(key, "levelmap")) {
            const char *why = "";
            wk_levelmap lm;
            if (cur.nlevelmaps >= WK_LM_MAX) {
                report(m, lineno, "too many levelmap lines%s", "");
            } else if (!parse_levelmap(val, &lm, &why)) {
                report(m, lineno, "levelmap: %s", why);
            } else {
                cur.levelmap[cur.nlevelmaps++] = lm;
            }
        } else {
            report(m, lineno, "unknown key '%s'", key);
        }
    }
    if (in_section && !skip)
        db_add(db, cur);
    free(buf);
    if (nproblems)
        *nproblems = m.count;
    return db;
}

void wk_profile_free(wk_profile_db *db)
{
    if (!db)
        return;
    free(db->items);
    free(db);
}

int wk_profile_count(const wk_profile_db *db) { return db ? db->n : 0; }

const wk_profile *wk_profile_at(const wk_profile_db *db, int i)
{
    return (db && i >= 0 && i < db->n) ? &db->items[i] : NULL;
}

const wk_profile *wk_profile_find(const wk_profile_db *db, uint32_t crc, const char *title)
{
    if (!db)
        return NULL;
    for (int i = 0; i < db->n; i++)
        if (db->items[i].has_crc && db->items[i].crc == crc)
            return &db->items[i];
    if (title && *title)
        for (int i = 0; i < db->n; i++)
            if (!db->items[i].has_crc && db->items[i].title[0] &&
                ieq(db->items[i].title, title))
                return &db->items[i];
    return NULL;
}

int wk_levelmap_tile(const wk_levelmap *lm, const uint8_t *ram, size_t len,
                     int32_t wx, int32_t wy, uint32_t *tile)
{
    if (!lm || !ram || lm->w <= 0 || lm->h <= 0 || lm->cell_px <= 0)
        return 0;
    int32_t rx = wx - lm->origin_x, ry = wy - lm->origin_y;
    if (rx < 0 || ry < 0)
        return 0;
    int32_t cx = rx / lm->cell_px, cy = ry / lm->cell_px;
    if (cx >= lm->w || cy >= lm->h)
        return 0;
    size_t off;
    if (lm->column_major) {
        int stride = lm->stride ? lm->stride : lm->h * lm->bytes;
        off = (size_t)lm->addr + (size_t)cx * (size_t)stride + (size_t)cy * (size_t)lm->bytes;
    } else {
        int stride = lm->stride ? lm->stride : lm->w * lm->bytes;
        off = (size_t)lm->addr + (size_t)cy * (size_t)stride + (size_t)cx * (size_t)lm->bytes;
    }
    if (off + (size_t)lm->bytes > len)
        return 0;
    uint32_t v = ram[off];
    if (lm->bytes == 2)
        v = lm->big_endian ? (uint32_t)(ram[off] << 8 | ram[off + 1])
                           : (uint32_t)(ram[off + 1] << 8 | ram[off]);
    int tw = lm->cell_px / lm->meta_w, th = lm->cell_px / lm->meta_h;
    int sx = (rx % lm->cell_px) / tw, sy = (ry % lm->cell_px) / th;
    int sub = sy * lm->meta_w + sx;
    if (lm->map_kind == 0) {
        *tile = lm->tile_base + v * lm->tile_mul + (uint32_t)sub;
        return 1;
    }
    for (int i = 0; i < lm->table_n; i++)
        if (lm->table_key[i] == v) {
            *tile = lm->table_tile[i][sub];
            return 1;
        }
    return 0;
}
