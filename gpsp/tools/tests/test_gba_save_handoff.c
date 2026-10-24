#define main battery_scaffold_main
#include "test_battery_persistence.c"
#undef main

static int stuck, yields;
static void wake_writer(void) {}
static void yield_writer(void)
{
   yields++;
   if (!stuck)
   {
#ifndef EXPECT_UNSAFE
      sram_io_busy = 0;
#endif
      fe_host_sram_service_io();
   }
}
static const fe_host_io writer = { wake_writer, yield_writer };
static void setup(const char *path, int full)
{
   unsigned i;
   host_io = NULL; gba_sram_hold = 0; gb_family = 0;
   reset_faults(); seed(path, FE_SRAM_SIZE);
   sram_load();
   memset(live_save, 0x42, sizeof(live_save)); backup_write_gen++;
   sram_scan_req = 0; sram_full_req = full;
   if (!full) assert(sram_scan(0) == 1);
   else for (i=0;i<SRAM_BLOCKS;i++) sram_blk_state[i]=SBLK_CLEAN;
}
int main(int argc, char **argv)
{
   char dir[]="/tmp/gbadhoc-handoff-XXXXXX", path[256];
   int threaded, full;
   (void)argc; (void)argv;
   assert(mkdtemp(dir)); snprintf(path,sizeof(path),"%s/game.sav",dir);
   host.save_path=save_path=path;
   for(threaded=0;threaded<2;threaded++) for(full=0;full<2;full++)
   {
      setup(path,full); host_io=threaded?&writer:NULL; stuck=yields=0;
#ifdef EXPECT_UNSAFE
      gba_sram_hold=1;
#else
      assert(gba_sram_hold_begin()==0);
      assert(!sram_pending() && !sram_full_req && !sram_io_busy);
#endif
      memset(live_save,0x99,sizeof(live_save)); backup_write_gen++;
      fe_host_sram_service_io();
#ifdef EXPECT_UNSAFE
      assert(matches(path,NULL,FE_SRAM_SIZE,0x99));
      printf("BASELINE unauthorized session bytes threaded=%d full=%d\n",threaded,full);
#else
      assert(matches(path,NULL,FE_SRAM_SIZE,0x42));
      /* Abort restores pre-link RAM; a validated end supplies a new image.
       * Both begin from an empty writer queue under the same held policy. */
      memset(live_save,0x42,sizeof(live_save)); gba_sram_hold=0;
      assert(fe_host_sram_flush(1)>=0);
      assert(matches(path,NULL,FE_SRAM_SIZE,0x42));
      memset(live_save,0x63,sizeof(live_save));
      assert(fe_host_sram_flush(1)>=0);
      assert(matches(path,NULL,FE_SRAM_SIZE,0x63));
      printf("PASS held/abort/commit threaded=%d full=%d\n",threaded,full);
#endif
      host_io=NULL; sram_shutdown();
   }
#ifndef EXPECT_UNSAFE
   setup(path,0); host_io=&writer; stuck=1; sram_io_busy=1;
   assert(gba_sram_hold_begin()<0 && !gba_sram_hold);
   assert(matches(path,NULL,FE_SRAM_SIZE,0x31));
   stuck=0; yields=0;
   assert(gba_sram_hold_begin()==0 && yields && !sram_io_busy);
   host_io=NULL; gba_sram_hold=0; sram_shutdown();
   setup(path,1); short_write=1;
   assert(gba_sram_hold_begin()<0 && !gba_sram_hold);
   reset_faults(); assert(gba_sram_hold_begin()==0);
   gba_sram_hold=0; sram_shutdown();
   setup(path,0); host_io=&writer; short_write=1;
   assert(gba_sram_hold_begin()<0 && !gba_sram_hold);
   assert(live_save[0]==0x42);
   reset_faults(); assert(gba_sram_hold_begin()==0);
   host_io=NULL; gba_sram_hold=0; sram_shutdown();
   puts("PASS busy writer timeout/retry and I/O failure/retry");
#endif
   unlink(path); assert(rmdir(dir)==0);
   return 0;
}
