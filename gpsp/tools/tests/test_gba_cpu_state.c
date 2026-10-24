#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../common.h"
#include "../../savestate.c"
static u8 st[4096];
static void put32(u8 *p, u32 x)
{ p[0]=x; p[1]=x>>8; p[2]=x>>16; p[3]=x>>24; }
static void make_state(u32 mode)
{
  u32 n;
  memset(reg, 0, sizeof(u32) * 64);
  reg[CPU_MODE] = mode; reg[REG_SP] = 0x03007f00; reg[REG_PC] = 0x08000000;
  reg[REG_CPSR] = 0x10;
  n = cpu_write_savestate(st + 4) + 5;
  st[n - 1] = 0; put32(st, n);
  assert(bson_validate_document(st, n, 0, false));
}
int main(void)
{
  static const u32 modes[] = { MODE_USER, MODE_SYSTEM, MODE_IRQ, MODE_FIQ,
       MODE_SUPERVISOR, MODE_ABORT, MODE_UNDEFINED };
  static const u32 bad[] = { 1, 7, 15, 0xffffffffu, MODE_INVALID };
  unsigned i;
  for (i = 0; i < sizeof(modes)/sizeof(modes[0]); i++) {
    make_state(modes[i]); assert(cpu_check_savestate(st));
    assert(cpu_read_savestate(st)); set_cpu_mode(MODE_IRQ);
  }
  for (i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
    make_state(bad[i]);
#ifdef EXPECT_UNSAFE
    printf("baseline CPU mode=%08x accepted=%d\n", bad[i], cpu_check_savestate(st));
#else
    assert(!cpu_check_savestate(st));
#endif
  }
  make_state(MODE_USER);
  {
    u32 saved[REG_ARCH_COUNT];
    const u8 *doc = bson_find_key(st, "cpu");
    u8 *regs = (u8 *)bson_find_key(doc, "regs");
    assert(bson_read_int32_array(doc, "regs", saved, REG_ARCH_COUNT));
    /* Each serialized array item is type + 2-character key + NUL + u32. */
    put32(regs + 4 + CPU_HALT_STATE * 8 + 4, CPU_DMA + 1);
#ifndef EXPECT_UNSAFE
    assert(!cpu_check_savestate(st));
#endif
  }
  puts("CPU state: all legal modes + bank switch, bad modes/halt rejected PASS");
  return 0;
}
