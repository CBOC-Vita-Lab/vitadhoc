/* A stand-in for instance B's core: a few initialised and zero-initialised
 * globals of different sizes, built with whatever sanitizer flags the test
 * gives it, then partially linked with vita/gba_instance_b.ld and renamed
 * gbab_* exactly as corebuild.mk does the real one.  Between instrumented
 * globals the compiler leaves poisoned redzones. */
#include <stdint.h>

uint32_t core_init_small = 0x11223344u;
uint8_t  core_init_tab[100] = { 1, 2, 3, 4, 5 };
const char *core_init_ptr = "text";
uint16_t core_init_odd[7] = { 9, 8, 7, 6, 5, 4, 3 };
double   core_init_d = 2.5;

uint32_t core_zero_small;
uint8_t  core_zero_tab[1000];
uint16_t core_zero_odd[13];
uint64_t core_zero_q;
static uint8_t core_static_zero[333];

uint32_t core_sum(void)
{
   uint32_t i, s = core_init_small + core_zero_small + core_zero_q
                   + core_init_ptr[0] + (uint32_t)core_init_d;
   for (i = 0; i < sizeof(core_init_tab); i++) s += core_init_tab[i];
   for (i = 0; i < sizeof(core_zero_tab); i++) s += core_zero_tab[i];
   for (i = 0; i < 7; i++) s += core_init_odd[i];
   for (i = 0; i < 13; i++) s += core_zero_odd[i];
   for (i = 0; i < sizeof(core_static_zero); i++) s += core_static_zero[i];
   return s;
}

void core_scribble(void)
{
   uint32_t i;
   core_init_small = 0xdeadbeefu;
   for (i = 0; i < sizeof(core_init_tab); i++) core_init_tab[i] ^= 0xa5;
   core_init_ptr = "other";
   for (i = 0; i < 7; i++) core_init_odd[i] = 0xffff;
   core_init_d = -1.0;
   core_zero_small = 7; core_zero_q = ~0ull;
   for (i = 0; i < sizeof(core_zero_tab); i++) core_zero_tab[i] = 0x5a;
   for (i = 0; i < 13; i++) core_zero_odd[i] = 0x1234;
   for (i = 0; i < sizeof(core_static_zero); i++) core_static_zero[i] = 0xee;
}
