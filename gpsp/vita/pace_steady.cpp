/* See pace_steady.h.  playerkit's pacer compiled into this translation unit
 * (MIT code inside the GPL program; no C++ runtime needed). */
#include "pace_steady.h"
#include "../../playerkit/src/pk_pace.cpp"

#include <new>

/* constructed on first use: no static constructor in the program */
alignas(pk::Pacer) static unsigned char g_mem[sizeof(pk::Pacer)];
static pk::Pacer *g_p;
static pk::Pacer &pacer()
{
   if (!g_p) g_p = new (g_mem) pk::Pacer();
   return *g_p;
}
#define g_steady pacer()

extern "C" void steady_config(uint32_t budget_us)
{
   pk::PaceConfig c;
   c.budget_us = budget_us ? budget_us : 16683;
   g_steady.configure(c);
   g_steady.reset(pk::PACE_FULL);
}
extern "C" void steady_enable(int on) { g_steady.set_enabled(on != 0); }
extern "C" void steady_reset(void) { g_steady.reset(pk::PACE_FULL); }
extern "C" int steady_add(uint32_t cost_us, int *changed)
{
   bool ch = false;
   pk::PaceMode m = g_steady.enabled() ? g_steady.add(cost_us, &ch) : pk::PACE_FULL;
   if (changed) *changed = ch;
   return m == pk::PACE_HALF;
}
extern "C" int steady_half(void) { return g_steady.enabled() && g_steady.mode() == pk::PACE_HALF; }
