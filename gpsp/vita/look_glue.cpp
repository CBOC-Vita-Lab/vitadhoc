/* See look_glue.h.  playerkit's look governor (pk_look, MIT) compiled into
 * this translation unit.  The program links with the C driver and no C++
 * library: the string class pk_look uses is instantiated here, and the few
 * library calls it can make on an impossible length (it never builds a
 * string of more than a few hundred bytes) end the program as an assert
 * would.  The allocation operators are handoff_glue.cpp's. */
#include "look_glue.h"
#include "../../playerkit/src/pk_look.cpp"

#include "mmpx_vita.h"
#include "upscale_vita.h"
#include "fe_util.h"          /* fe_ini_get_int: vita.ini look_bitmap */

#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

template class std::basic_string<char>;

/* pk_look's one text (look_auto_label) through the app's own catalogue (i18n.h) instead of the players' loader. */
#include "i18n.h"
#include <stdarg.h>
namespace pk {
std::string trf(const char *key, ...)
{
	char b[160];
	va_list ap;
	va_start(ap, key);
	vsnprintf(b, sizeof b, ::tr(key), ap);
	va_end(ap);
	return b;
}
}  // namespace pk

namespace std {
void __throw_bad_alloc() { abort(); }
void __throw_logic_error(const char *) { abort(); }
void __throw_length_error(const char *) { abort(); }
void __throw_out_of_range_fmt(const char *, ...) { abort(); }
}  // namespace std

/* The GBA core's I/O registers: DISPCNT is the first (video mode in bits 0-2).
 * Read once a second, from this thread, while the core writes it: a torn
 * read of one halfword cannot happen, and one stale sample costs nothing. */
extern "C" uint16_t io_registers[512];
/* upscale_vita.c: source pixels submitted / scaled since the start (not reset by upscale_stats) */
extern "C" void upscale_cumulative(unsigned *px, unsigned *scaled);

namespace {

/* ---- the quality ladder of a console ----------------------------------------
 *
 * What a quality step is made of (docs/DEFAULTS-1.4.md section 5): the tiers a
 * console's Auto walks down, from the factor it STARTS at to sharp bilinear.
 * The start is a starting point, not a promise: the governor steps down in
 * the first bad seconds, remembers where the game settled, and climbs back
 * only with measured headroom.  Where it starts is the console's CEILING,
 * chosen from output pixels (the only console evidence is Pokemon Unbound on
 * a GBA at xBRZ 3x full screen: 345,600 output pixels a frame, 59.7 emulated
 * but 44 drawn frames a second, docs/PERFORMANCE.md), with the real width of
 * the picture (the GBA's wide view is 282 wide, so its 3x is 406,080):
 *   GBA   282x160: 3x.  4x is 721k px (1.78 times 3x): hand-set only, never
 *                  a start and never a climb.  The owner's order puts a
 *                  locked 60 first, so this is the start to be corrected.
 *   GB/C  160x144: 3x until measured (4x is 369k px, but nothing has run)
 *   WS    224x144: 3x
 *   NES   424x240: 2x (wide view); 3x exceeds the 544-line panel and the
 *                  pixel budget, so 2x and the GPU's stretch is cleaner too
 * An unknown console gets 2x.
 *
 * MMPX 2x (mmpx_vita.h) sits between xBRZ 2x and sharp bilinear (the floor):
 * it makes the same-size 2x picture for about two thirds of xBRZ's work. */

struct Console {
   const char *dir;       /* the memory's folder: <root>/look/<dir>/ */
   int w, h;              /* the picture (the wide one where there is a wide view) */
   int ceil;              /* the factor Auto starts at */
   int is_gba;
};

const Console k_consoles[] = {
   { "gba", 282, 160, 3, 1 },
   { "gb", 160, 144, 3, 0 },
   { "gbc", 160, 144, 3, 0 },
   { "ws", 224, 144, 3, 0 },
   { "nes", 424, 240, 2, 0 },
};
/* a console this file does not know (never the GBA's numbers) */
const Console k_other = { "", 256, 240, 2, 0 };

/* One output pixel's cost as a share of a refresh, seeds for the governor.
 * xBRZ: a whole 3x GBA picture redrawn is 20-33 ms at 4x on the two helpers
 * (upscale_vita.c UP_HOT_HOLD, measured on a console), so about 11-19 ms at 3x
 * (345,600 px); a game's typical picture is about half new: 0.45 of a refresh
 * for 345,600 px.  MMPX: the instruction count per output pixel, relative to
 * xBRZ 2x on real frames of the GBA games (qemu-arm instruction counts and host timing, docs/DEFAULTS-1.4.md). */
const float k_xbrz_px = 0.45f / 345600.f;
const float k_mmpx_ratio = 0.65f;

pk::LookTierCosts tier_costs()
{
   pk::LookTierCosts c;
   c.xbrz_per_px = k_xbrz_px;
   c.mmpx_per_px = k_xbrz_px * k_mmpx_ratio;
   return c;
}

/* The tiers of Auto for a console, best first. */
struct Tiers
{
   pk::LookTier t[6];
   int n;
};

Tiers tiers_for(const Console &c)
{
   Tiers r;
   const int top = c.ceil < 2 ? 2 : c.ceil > 4 ? 4 : c.ceil;
   int f;
   r.n = 0;
   for (f = top; f >= 2; f--)
      r.t[r.n++] = f == 4 ? pk::LOOK_TIER_XBRZ4 : f == 3 ? pk::LOOK_TIER_XBRZ3
                                                          : pk::LOOK_TIER_XBRZ2;
   r.t[r.n++] = pk::LOOK_TIER_MMPX2;
   r.t[r.n++] = pk::LOOK_TIER_SHARP;
   return r;
}

const char *tier_name(pk::LookTier t)
{
   return t == pk::LOOK_TIER_XBRZ4 ? "xBRZ 4x" : t == pk::LOOK_TIER_XBRZ3 ? "xBRZ 3x"
        : t == pk::LOOK_TIER_XBRZ2 ? "xBRZ 2x" : t == pk::LOOK_TIER_MMPX2 ? "MMPX 2x"
        : "sharp";
}

/* docs/DEFAULTS-1.4.md section 5: least visible first.  The ladder is built
 * for the console: ambient, then the quality steps (the visible ones after
 * the frame blend), the frame blend, the wide view (consoles that have one).
 * g_id[] maps the fixed rung numbers the app asks about (look_rung) to the
 * ladder's own index (-1 where this console has none). */
enum { ID_AMBIENT = 0, ID_Q1, ID_Q2, ID_BLEND, ID_Q3, ID_WIDE, IDS };

pk::LookRung g_ladder[pk::LOOK_MAX_RUNGS];
char g_names[pk::LOOK_MAX_RUNGS][32];
int g_n;
int g_id[IDS];
int g_qrung[pk::LOOK_MAX_RUNGS];     /* the ladder indexes of the quality rungs, in order */
int g_nq;
Tiers g_tiers;
const Console *g_con;

void add_rung(const char *name, int row, float saving, int vis, int id)
{
   pk::LookRung &r = g_ladder[g_n];
   snprintf(g_names[g_n], sizeof(g_names[g_n]), "%s", name);
   r.name = g_names[g_n];
   r.row = row;
   r.est_saving = saving < 0.01f ? 0.01f : saving > 0.95f ? 0.95f : saving;
   r.visibility = vis;
   if (id >= 0)
      g_id[id] = g_n;
   g_n++;
}

void build_ladder(const Console &c)
{
   const pk::LookTierCosts cost = tier_costs();
   int i;
   g_con = &c;
   g_tiers = tiers_for(c);
   g_n = g_nq = 0;
   for (i = 0; i < IDS; i++)
      g_id[i] = -1;
   add_rung("ambient frame -> art", LOOK_ROW_AMBIENT, 0.03f, 8, ID_AMBIENT);
   /* the quality steps: three or four of them, each step's saving is the
    * difference of its two tiers' output pixel costs.  Visibility 15, 25,
    * 35 before the frame blend (40), the last, to sharp, after it (55). */
   for (i = 0; i + 1 < g_tiers.n; i++)
   {
      const pk::LookTier a = g_tiers.t[i], b = g_tiers.t[i + 1];
      const int last = i + 2 == g_tiers.n;
      char nm[32];
      const float sv = pk::look_tier_cost(a, c.w, c.h, cost) -
                       pk::look_tier_cost(b, c.w, c.h, cost);
      const int vis = last ? 55 : i == 0 && g_tiers.n >= 5 ? 15 : i + 2 >= g_tiers.n - 1 ? 35 : 25;
      if (last)
         add_rung("", LOOK_ROW_BLEND, 0.05f, 40, ID_BLEND);     /* the blend sits before the last step */
      snprintf(nm, sizeof(nm), "%s -> %s", tier_name(a), tier_name(b));
      g_qrung[g_nq++] = g_n;
      add_rung(nm, LOOK_ROW_QUALITY, sv, vis, i == 0 ? ID_Q1 : i == 1 ? ID_Q2 : last ? ID_Q3 : -1);
   }
   /* No wide view rung: the owner's order (docs/DEFAULTS-1.4.md): a locked 60
    * first, wide view / screen fill stays on, then the best look that fits;
    * so the scaler is what the governor gives up first and gets back last. */
}

/* The console of a game, from its memory path: <root>/look/<dir>/<xx>/<name>.look */
const Console &console_of(const char *path)
{
   const char *p = path ? strstr(path, "/look/") : NULL;
   size_t i;
   if (!p)
      return k_other;
   p += 6;
   for (i = 0; i < sizeof(k_consoles) / sizeof(k_consoles[0]); i++)
   {
      const size_t n = strlen(k_consoles[i].dir);
      if (strncmp(p, k_consoles[i].dir, n) == 0 && p[n] == '/')
         return k_consoles[i];
   }
   return k_other;
}

alignas(pk::LookGovernor) unsigned char g_mem[sizeof(pk::LookGovernor)];
pk::LookGovernor *g_g;
char g_path[512];
int g_mhz;
unsigned g_user = ~0u;
int g_reassert;                    /* the ceiling is to be given to the upscaler again (look_generation) */

/* software 3D: the picture is drawn whole in a bitmap mode every frame */
enum { BM_WINDOW = 10, BM_NEED = 7 };
unsigned char g_bm[BM_WINDOW];
int g_bm_n, g_bm_head, g_bm_done;
int g_bm_on;                       /* the check runs (vita.ini look_bitmap, and no memory of the game) */
unsigned g_cum_px, g_cum_sc;       /* upscale_cumulative at the last second */

void set_rows(unsigned user)
{
   int r;
   for (r = 0; r < LOOK_ROWS; r++)
      if (((g_user >> r) & 1u) != ((user >> r) & 1u))
         g_g->set_user_row(r, ((user >> r) & 1u) != 0);
   g_user = user;
}

/* The quality tier now in force: the number of quality steps applied. */
int tier_index()
{
   int i, k = 0;
   if (!g_g)
      return 0;
   for (i = 0; i < g_nq; i++)
      if (g_g->applied(g_qrung[i]))
         k++;
   return k < g_tiers.n ? k : g_tiers.n - 1;
}

}  // namespace

extern "C" void look_begin(const char *mem_path, int mhz, unsigned user_rows)
{
   FILE *f;
   if (g_g)
      look_end();
   build_ladder(console_of(mem_path));
   g_g = new (g_mem) pk::LookGovernor();
   g_g->set_ladder(g_ladder, g_n);
   g_user = 0;
   {
      int r;
      for (r = 0; r < LOOK_ROWS; r++)
         if ((user_rows >> r) & 1u)
            g_g->set_user_row(r, true);
      g_user = user_rows;
   }
   g_mhz = mhz;
   g_bm_n = g_bm_head = g_bm_done = 0;
   g_bm_on = 1;
   upscale_cumulative(&g_cum_px, &g_cum_sc);
   {
      /* vita.ini look_bitmap = 0 turns the bitmap-mode check off (the A/B switch) */
      const char *r = strstr(mem_path ? mem_path : "", "/look/");
      if (r)
      {
         char ini[512];
         snprintf(ini, sizeof(ini), "%.*s/vita.ini", (int)(r - mem_path), mem_path);
         if (!fe_ini_get_int(ini, "look_bitmap", 1))
            g_bm_on = 0;
      }
   }
   memset(g_bm, 0, sizeof(g_bm));
   g_reassert = 1;
   snprintf(g_path, sizeof(g_path), "%s", mem_path ? mem_path : "");
   if (g_path[0] && (f = fopen(g_path, "rb")) != NULL)
   {
      char text[1100];
      size_t n = fread(text, 1, sizeof(text) - 1, f);
      fclose(f);
      text[n] = '\0';
      /* a game with a memory of its look has been through this already */
      if (g_g->load_memory(text, n, mhz))
         g_bm_on = 0;
   }
   g_g->start();
}

extern "C" int look_save(void)
{
   if (!g_g || !g_path[0])
      return 0;
   switch (g_g->memory_action())
   {
   case pk::LOOK_MEM_WRITE:
   {
      char tmp[sizeof(g_path) + 8];
      const std::string t = g_g->serialize(g_mhz);
      FILE *f;
      int ok;
      char dir[sizeof(g_path)];
      char *s;
      /* <root>/look/<console>/<xx>/: made as needed, one level at a time */
      snprintf(dir, sizeof(dir), "%s", g_path);
      for (s = strchr(dir + 1, '/'); s; s = strchr(s + 1, '/'))
      {
         *s = '\0';
         (void)mkdir(dir, 0777);
         *s = '/';
      }
      snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
      if ((f = fopen(tmp, "wb")) == NULL)
         return 0;
      ok = fwrite(t.data(), 1, t.size(), f) == t.size();
      ok &= fclose(f) == 0;
      if (!ok)
      {
         remove(tmp);
         return 0;
      }
      remove(g_path);
      if (rename(tmp, g_path) != 0)
      {
         remove(tmp);
         return 0;
      }
      return 1;
   }
   case pk::LOOK_MEM_DELETE:
      remove(g_path);
      return 2;
   default:
      return 0;
   }
}

extern "C" void look_end(void)
{
   if (!g_g)
      return;
   g_g->~LookGovernor();
   g_g = NULL;
   g_path[0] = '\0';
   g_reassert = 0;
   mmpx_set_auto(0);
}

extern "C" int look_running(void) { return g_g != NULL; }

extern "C" void look_set_user_rows(unsigned user_rows)
{
   if (g_g && user_rows != g_user)
      set_rows(user_rows);
}

/* One sample of the picture's mode: is the game drawing a bitmap mode (3, 4,
 * 5) whole every frame?  Software 3D (Need for Speed, Driver, SSX) does: no
 * part of the picture stays the same from one frame to the next, so the
 * upscaler's savings (only what changed, scrolls) are all gone and a whole
 * xBRZ picture has to be made inside every refresh.  A tile game that shows
 * a bitmap title or a fade is no such game: it takes 7 of 10 seconds. */
static int bitmap_game(void)
{
   const unsigned dispcnt = io_registers[0];
   const unsigned mode = dispcnt & 7u;
   unsigned px, sc, dpx, dsc;
   upscale_cumulative(&px, &sc);
   dpx = px - g_cum_px;
   dsc = sc - g_cum_sc;
   g_cum_px = px;
   g_cum_sc = sc;
   if (dispcnt & 0x80u)
      return 0;                       /* forced blank says nothing */
   /* a second counts when the upscaler really had to rescale most of the
    * picture: a bitmap-mode title or fade that leaves most of it alone is not
    * software 3D (no scaler running at all says nothing either way) */
   if (!dpx)
      return 0;
   g_bm[g_bm_head] = mode >= 3 && mode <= 5 && (unsigned long long)dsc * 10u >= (unsigned long long)dpx * 7u;
   g_bm_head = (g_bm_head + 1) % BM_WINDOW;
   if (g_bm_n < BM_WINDOW)
      g_bm_n++;
   if (g_bm_n < BM_WINDOW)
      return 0;
   {
      int i, n = 0;
      for (i = 0; i < BM_WINDOW; i++)
         n += g_bm[i];
      return n >= BM_NEED;
   }
}

extern "C" int look_second(float emu, float draw, float filter, int refreshes,
                           int missed, int parallel, int pace_half)
{
   pk::LookSecond s;
   int changed;
   if (!g_g)
      return 0;
   s.emu = emu;
   s.draw = draw;
   s.filter = filter;
   s.refreshes = refreshes;
   s.missed = missed;
   s.parallel = parallel != 0;
   s.pace_half = pace_half != 0;
   changed = g_g->second(s) ? 1 : 0;
   /* a GBA game that redraws a bitmap mode whole: start it (and every later
    * start, the governor remembers it) at sharp bilinear; the climb gives MMPX 2x
    * back only with measured headroom */
   if (g_bm_on && (g_con && g_con->is_gba) && !g_bm_done && refreshes >= pk::LOOK_MIN_REFRESHES &&
       bitmap_game())
   {
      uint32_t q = 0;
      int i;
      g_bm_done = 1;
      for (i = 0; i < g_nq; i++)          /* the lowest tier: nothing holds 60 on a whole picture every frame until measured */
         q |= 1u << g_qrung[i];
      if (g_g->force_rungs(q, "bitmap mode: the picture is made whole every frame"))
         changed = 1;
   }
   return changed;
}

extern "C" int look_safe_point(int kind)
{
   return g_g && g_g->safe_point((pk::LookSafe)kind) ? 1 : 0;
}
extern "C" void look_reset_window(void) { if (g_g) g_g->reset_window(); }

/* The quality tier in force as the upscaler's limit: the largest xBRZ factor
 * (0 = no limit), 1 = sharp bilinear; MMPX 2x is a 2 with the MMPX flag.  A
 * side effect, on purpose: the flag follows the tier wherever the app asks. */
extern "C" int look_quality_cap(void)
{
   pk::LookTier t;
   int cap;
   if (!g_g)
   {
      mmpx_set_auto(0);
      return 0;
   }
   t = g_tiers.t[tier_index()];
   cap = t == pk::LOOK_TIER_XBRZ4 ? 4 : t == pk::LOOK_TIER_XBRZ3 ? 3
       : t == pk::LOOK_TIER_XBRZ2 || t == pk::LOOK_TIER_MMPX2 ? 2 : 1;
   mmpx_set_auto(t == pk::LOOK_TIER_MMPX2 && !(g_user & (1u << LOOK_ROW_QUALITY)));
   return cap;
}

extern "C" const char *look_tier_name(void)
{
   const pk::LookTier t = g_g ? g_tiers.t[tier_index()] : pk::LOOK_TIER_SHARP;
   if (!g_g)
      return "";
   return t == pk::LOOK_TIER_SHARP ? "Sharp" : tier_name(t);
}

/* The app sets the upscaler's cap to 0 right after look_begin and looks at
 * this next: a game's first look is its ceiling, which is no change of the
 * applied rungs, so it is given here once (the app's own call can be
 * upscale_look_cap(look_quality_cap()) and then this does nothing). */
extern "C" uint32_t look_generation(void)
{
   if (g_reassert && g_g)
   {
      g_reassert = 0;
      upscale_look_cap(look_quality_cap());
   }
   return g_g ? g_g->generation() : 0;
}
extern "C" uint32_t look_mask(void) { return g_g ? g_g->mask() : 0; }
extern "C" int look_steps_on_row(int row) { return g_g ? g_g->steps_on_row(row) : 0; }
extern "C" int look_rung(int i)
{
   return g_g && i >= 0 && i < IDS && g_id[i] >= 0 && g_g->applied(g_id[i]);
}

extern "C" int look_census_line(char *buf, size_t n)
{
   size_t l;
   if (!(g_g && g_g->census_line(buf, n)))
      return 0;
   /* what the bitmap-mode check saw (GBA): the video mode now, bitmap seconds of the last 10 */
   l = strlen(buf);
   if ((g_con && g_con->is_gba) && l + 40 < n)
   {
      int i, k = 0;
      for (i = 0; i < g_bm_n; i++)
         k += g_bm[i];
      snprintf(buf + l, n - l, " mode=%u bm=%d/%d", (unsigned)(io_registers[0] & 7u), k, g_bm_n);
   }
   return 1;
}

extern "C" void look_summary(char *buf, size_t n)
{
   if (!g_g || !n)
   {
      if (n)
         buf[0] = '\0';
      return;
   }
   snprintf(buf, n, "%s", g_g->summary().c_str());
   /* one line */
   for (char *p = buf; *p; p++)
      if (*p == '\n')
         *p = ' ';
}
