#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../common.h"
#include "../../main.h"
extern timer_type timer[4];
#include "../../savestate.c"
static u8 st[4096];
static void put32(u8 *p, u32 x)
{ p[0]=x; p[1]=x>>8; p[2]=x>>16; p[3]=x>>24; }
u32 reg[64];
u32 serial_get_irq_cycles(void) { return 0; }
u32 gbp_get_state(void) { return 0; }
void gbp_set_state(u32 v) { (void)v; }
void serial_set_irq_cycles(u32 v) { (void)v; }
int main(void)
{
  u32 n;
  unsigned i;
  init_main();
  n = main_write_savestate(st + 4) + 5;
  st[n - 1] = 0; put32(st, n);
  assert(bson_validate_document(st, n, 0, false));
  assert(main_check_savestate(st));
  assert(main_read_savestate(st));
  for (i = 0; i < 4; i++) {
    char key[2] = { '0' + i, 0 };
    const u8 *doc = bson_find_key(bson_find_key(st, "timers"), key);
    u8 *reload = (u8 *)bson_find_key(doc, "reload");
    put32(reload, 0);
#ifdef EXPECT_UNSAFE
    printf("baseline timer%u reload=0 accepted=%d\n", i, main_check_savestate(st));
#else
    assert(!main_check_savestate(st));
    put32(reload, 0x10001); assert(!main_check_savestate(st));
    put32(reload, 1); assert(main_check_savestate(st));
    assert(main_read_savestate(st)); assert(timer[i].reload==1);
#endif
    put32(reload, 0x10000); assert(main_check_savestate(st));
    assert(main_read_savestate(st)); assert(timer[i].reload==0x10000);
  }
  puts("Timer state: actual writer roundtrip, reload boundaries checked PASS");
  return 0;
}
