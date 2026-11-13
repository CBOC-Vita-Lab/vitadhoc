/* See look_glue.h.  playerkit's look governor (pk_look, MIT) compiled into
 * this translation unit.  The program links with the C driver and no C++
 * library: the string class pk_look uses is instantiated here, and the few
 * library calls it can make on an impossible length (it never builds a
 * string of more than a few hundred bytes) end the program as an assert
 * would.  The allocation operators are handoff_glue.cpp's. */
#include "look_glue.h"
#include "../../playerkit/src/pk_look.cpp"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

template class std::basic_string<char>;

namespace std {
void __throw_bad_alloc() { abort(); }
void __throw_logic_error(const char *) { abort(); }
void __throw_length_error(const char *) { abort(); }
void __throw_out_of_range_fmt(const char *, ...) { abort(); }
}  // namespace std

namespace {

/* docs/DEFAULTS-1.4.md section 5: least visible first */
const pk::LookRung k_ladder[] = {
   { "ambient frame -> art", LOOK_ROW_AMBIENT, 0.03f, 8 },
   { "xBRZ 4x -> 3x", LOOK_ROW_QUALITY, 0.12f, 15 },
   { "xBRZ 3x -> 2x", LOOK_ROW_QUALITY, 0.10f, 25 },
   { "frame blend off", LOOK_ROW_BLEND, 0.05f, 40 },
   { "xBRZ 2x -> sharp", LOOK_ROW_QUALITY, 0.10f, 55 },
   { "wide view off", LOOK_ROW_WIDE, 0.12f, 80 },
};
enum { RUNGS = sizeof(k_ladder) / sizeof(k_ladder[0]) };

alignas(pk::LookGovernor) unsigned char g_mem[sizeof(pk::LookGovernor)];
pk::LookGovernor *g_g;
char g_path[512];
int g_mhz;
unsigned g_user = ~0u;

void set_rows(unsigned user)
{
   int r;
   for (r = 0; r < LOOK_ROWS; r++)
      if (((g_user >> r) & 1u) != ((user >> r) & 1u))
         g_g->set_user_row(r, ((user >> r) & 1u) != 0);
   g_user = user;
}

}  // namespace

extern "C" void look_begin(const char *mem_path, int mhz, unsigned user_rows)
{
   FILE *f;
   if (g_g)
      look_end();
   g_g = new (g_mem) pk::LookGovernor();
   g_g->set_ladder(k_ladder, RUNGS);
   g_user = 0;
   {
      int r;
      for (r = 0; r < LOOK_ROWS; r++)
         if ((user_rows >> r) & 1u)
            g_g->set_user_row(r, true);
      g_user = user_rows;
   }
   g_mhz = mhz;
   snprintf(g_path, sizeof(g_path), "%s", mem_path ? mem_path : "");
   if (g_path[0] && (f = fopen(g_path, "rb")) != NULL)
   {
      char text[1100];
      size_t n = fread(text, 1, sizeof(text) - 1, f);
      fclose(f);
      text[n] = '\0';
      g_g->load_memory(text, n, mhz);
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
}

extern "C" int look_running(void) { return g_g != NULL; }

extern "C" void look_set_user_rows(unsigned user_rows)
{
   if (g_g && user_rows != g_user)
      set_rows(user_rows);
}

extern "C" int look_second(float emu, float draw, float filter, int refreshes,
                           int missed, int parallel, int pace_half)
{
   pk::LookSecond s;
   if (!g_g)
      return 0;
   s.emu = emu;
   s.draw = draw;
   s.filter = filter;
   s.refreshes = refreshes;
   s.missed = missed;
   s.parallel = parallel != 0;
   s.pace_half = pace_half != 0;
   return g_g->second(s) ? 1 : 0;
}

extern "C" int look_safe_point(int kind)
{
   return g_g && g_g->safe_point((pk::LookSafe)kind) ? 1 : 0;
}
extern "C" void look_reset_window(void) { if (g_g) g_g->reset_window(); }
extern "C" uint32_t look_generation(void) { return g_g ? g_g->generation() : 0; }
extern "C" uint32_t look_mask(void) { return g_g ? g_g->mask() : 0; }
extern "C" int look_steps_on_row(int row) { return g_g ? g_g->steps_on_row(row) : 0; }
extern "C" int look_rung(int i) { return g_g && g_g->applied(i); }

extern "C" int look_quality_cap(void)
{
   if (!g_g)
      return 0;
   return g_g->applied(4) ? 1 : g_g->applied(2) ? 2 : g_g->applied(1) ? 3 : 0;
}

extern "C" int look_census_line(char *buf, size_t n)
{
   return g_g && g_g->census_line(buf, n);
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
