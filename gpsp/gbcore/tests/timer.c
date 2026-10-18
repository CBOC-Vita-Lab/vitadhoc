#include "gbcore/gbcore.h"
#include "gbcore/tgbdual/gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static byte rom[32768];
static byte state[1<<20], pending_state[1<<20], bad[1<<20], after[1<<20];
static uint64_t audio_hash;
static unsigned audio_count;
static void audio(void *u,const int16_t *p,size_t n) {
  (void)u;audio_count+=(unsigned)n;
  for(size_t i=0;i<2*n;i++)audio_hash=(audio_hash^(uint16_t)p[i])*1099511628211ULL;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s DIV=%u TIMA=%u\n",__LINE__,#x,cpu_read(0xff04),cpu_read(0xff05)); exit(1); } } while (0)
static gbcore_t *boot(int mode) {
  byte sum=0;gbcore_callbacks_t cb={0};cb.audio_batch=audio;memset(rom,0,sizeof rom);
  rom[0x143]=mode?0x80:0;rom[0x146]=3;rom[0x14b]=0x33;
  for(unsigned i=0x134;i<=0x14c;i++)sum=(byte)(sum-rom[i]-1);
  rom[0x14d]=sum;
  gbcore_t *c=gbcore_create(NULL,rom,sizeof rom,mode?FE_CONSOLE_GBC:FE_CONSOLE_GB,32768,&cb);CHECK(c);
  if(mode==2){int dat[8];cpu_save_state(dat);dat[2]=1;cpu_restore_state(dat);}
  return c;
}
static void reset(void) {
  int dat[4]={0};cpu_restore_state_ex(dat);c_regs_PC=0x150;
  *cpu_get_halt()=0;seri_occer=0x7fffffff;
  struct cpu_regs *r=cpu_get_c_regs();r->I=0;r->SP=0xfffe;cpu_set_c_regs();cpu_write(0xffff,0);
  const byte legacy[8]={0};cpu_load_timer_timing(legacy);
  cpu_write(0xff07,0);cpu_write(0xff04,0);cpu_write(0xff05,0);cpu_write(0xff0f,0);
}
static void ticks(unsigned cpu_clocks,int mode) {cpu_exec((unsigned short)(cpu_clocks/(mode==2?2:1)));}
int main(int argc,char **argv) {
  const char *which=argc>1?argv[1]:"all";
  for(int mode=0;mode<3;mode++) {
    gbcore_t *c=boot(mode);
    reset();ticks(256,mode);CHECK(cpu_read(0xff05)==0); /* disabled timer */
    if(!strcmp(which,"all")||!strcmp(which,"div")) {
      reset();ticks(252,mode);CHECK(cpu_read(0xff04)==0);
      ticks(4,mode);CHECK(cpu_read(0xff04)==1);
      for(unsigned i=2;i<=300;i++){ticks(256,mode);CHECK(cpu_read(0xff04)==(byte)i);c_regs_PC=0x150;}
      cpu_write(0xff04,0xab);CHECK(cpu_read(0xff04)==0);ticks(256,mode);CHECK(cpu_read(0xff04)==1);
    }
    if(!strcmp(which,"all")||!strcmp(which,"boundary")) {
      const unsigned periods[]={1024,16,64,256};
      for(int sel=0;sel<4;sel++) {
        reset();cpu_write(0xff07,(byte)(4|sel));ticks(periods[sel]-4,mode);CHECK(cpu_read(0xff05)==0);
        ticks(4,mode);CHECK(cpu_read(0xff05)==1);
        ticks(periods[sel],mode);CHECK(cpu_read(0xff05)==2);
        cpu_write(0xff05,0x40);ticks(periods[sel],mode);CHECK(cpu_read(0xff05)==0x41);
      }
    }
    if(!strcmp(which,"all")||!strcmp(which,"multi")) {
      reset();cpu_write(0xff07,5);cpu_write(0xff05,0xfe);cpu_write(0xff06,0xfc);
      ticks(12,mode);CHECK(cpu_read(0xff05)==0xfe);
      /* A real 24-T-clock CALL crosses two 16-T-clock timer boundaries. */
      byte *r=get_rom();r[c_regs_PC]=0xcd;r[c_regs_PC+1]=0;r[c_regs_PC+2]=2;
      ticks(24,mode);CHECK(cpu_read(0xff05)==0xfc);CHECK(cpu_read(0xff0f)&4);
      cpu_write(0xff0f,0);ticks(64,mode);CHECK(cpu_read(0xff05)==0xfc);CHECK(cpu_read(0xff0f)&4);
    }
    if(!strcmp(which,"all")||!strcmp(which,"phase")) {
      const unsigned periods[]={1024,16,64,256};
      for(int sel=0;sel<4;sel++) {
        reset();ticks(periods[sel]-4,mode);cpu_write(0xff07,(byte)(4|sel));
        ticks(4,mode);CHECK(cpu_read(0xff05)==1); /* enabling preserves DIV phase */
        reset();cpu_write(0xff07,(byte)(4|sel));
        for(unsigned i=0;i<300;i++) {
          ticks(periods[sel]/2-4,mode);cpu_write(0xff04,0);
          CHECK(cpu_read(0xff05)==0);CHECK(!(cpu_read(0xff0f)&4));
        }
        ticks(periods[sel]/2,mode);cpu_write(0xff04,0);CHECK(cpu_read(0xff05)==1);
        ticks(periods[sel]-4,mode);CHECK(cpu_read(0xff05)==1);
        ticks(4,mode);CHECK(cpu_read(0xff05)==2);
      }
    }
    if(!strcmp(which,"all")||!strcmp(which,"tac")) {
      reset();cpu_write(0xff07,5);ticks(8,mode);cpu_write(0xff07,5);
      CHECK(cpu_read(0xff05)==0); /* same source has no falling edge */
      cpu_write(0xff07,4);CHECK(cpu_read(0xff05)==1); /* high bit 3 -> low bit 9 */
      reset();cpu_write(0xff07,5);ticks(8,mode);cpu_write(0xff07,1);
      CHECK(cpu_read(0xff05)==(mode==0?1:0)); /* DMG/SGB disable tick; CGB no tick */
      reset();cpu_write(0xff07,4);ticks(512,mode);cpu_write(0xff07,4);
      CHECK(cpu_read(0xff05)==0);
      cpu_write(0xff05,0xff);cpu_write(0xff06,0x55);cpu_write(0xff07,5);
      CHECK(cpu_read(0xff05)==0);CHECK(!(cpu_read(0xff0f)&4));
      ticks(4,mode);CHECK(cpu_read(0xff05)==0x55);CHECK(cpu_read(0xff0f)&4);
    }
    if(!strcmp(which,"all")||!strcmp(which,"reload")) {
      byte timing[8];
      reset();cpu_write(0xff07,6);cpu_write(0xff05,0xff);cpu_write(0xff06,0x55);
      ticks(60,mode);CHECK(cpu_read(0xff05)==0xff);CHECK(!(cpu_read(0xff0f)&4));
      ticks(4,mode);CHECK(cpu_read(0xff05)==0);CHECK(!(cpu_read(0xff0f)&4));
      cpu_save_timer_timing(timing);CHECK(timing[4]==4&&timing[5]==0);
      ticks(4,mode);CHECK(cpu_read(0xff05)==0x55);CHECK(cpu_read(0xff0f)&4);
      cpu_write(0xff05,0x77);CHECK(cpu_read(0xff05)==0x55);
      cpu_write(0xff06,0x66);CHECK(cpu_read(0xff05)==0x66);
      ticks(4,mode);cpu_write(0xff05,0x77);CHECK(cpu_read(0xff05)==0x77);
      reset();cpu_write(0xff07,6);cpu_write(0xff05,0xff);ticks(64,mode);
      cpu_write(0xff05,0x77);ticks(4,mode);CHECK(cpu_read(0xff05)==0x77);CHECK(!(cpu_read(0xff0f)&4));
      reset();cpu_write(0xff07,6);cpu_write(0xff05,0xff);ticks(64,mode);
      cpu_write(0xff06,0x66);cpu_write(0xff04,0);cpu_write(0xff07,0);
      ticks(4,mode);CHECK(cpu_read(0xff05)==0x66);CHECK(cpu_read(0xff0f)&4);
      reset();cpu_write(0xff07,6);cpu_write(0xff05,0xff);cpu_write(0xff06,0x55);ticks(64,mode);
      gdma_rest=4;ticks(4,mode);CHECK(cpu_read(0xff05)==0x55);CHECK(cpu_read(0xff0f)&4);
      reset();gdma_rest=256;ticks(256,mode);CHECK(cpu_read(0xff04)==1);
    }
    if(!strcmp(which,"all")||!strcmp(which,"reload-state")) {
      reset();cpu_write(0xff26,0x80);cpu_write(0xff12,0xf0);cpu_write(0xff24,0x77);cpu_write(0xff25,0x11);cpu_write(0xff14,0x80);
      cpu_write(0xff07,6);cpu_write(0xff05,0xff);cpu_write(0xff06,0x55);ticks(64,mode);
      byte saved_timing[8], loaded_timing[8];cpu_save_timer_timing(saved_timing);
      long n=gbcore_state_save(c,state,sizeof state);CHECK(n>0);
      long pending_n=n;memcpy(pending_state,state,(size_t)n);
      ticks(4,mode);CHECK(cpu_read(0xff05)==0x55);CHECK(cpu_read(0xff0f)&4);
      CHECK(gbcore_state_load(c,state,(size_t)n)==0);cpu_save_timer_timing(loaded_timing);CHECK(!memcmp(saved_timing,loaded_timing,8));
      uint64_t hash=gbcore_sync_hash(c); /* public restore normalizes total_clock */
      ticks(4,mode);CHECK(gbcore_state_load(c,state,(size_t)n)==0);CHECK(gbcore_sync_hash(c)==hash);
      ticks(4,mode);CHECK(cpu_read(0xff05)==0x55);CHECK(cpu_read(0xff0f)&4);
      n=gbcore_state_save(c,state,sizeof state);CHECK(n>0);cpu_save_timer_timing(saved_timing);
      ticks(4,mode);cpu_write(0xff05,0x77);CHECK(cpu_read(0xff05)==0x77);
      CHECK(gbcore_state_load(c,state,(size_t)n)==0);cpu_save_timer_timing(loaded_timing);CHECK(!memcmp(saved_timing,loaded_timing,8));
      cpu_write(0xff05,0x77);CHECK(cpu_read(0xff05)==0x55);
      cpu_write(0xff06,0x66);CHECK(cpu_read(0xff05)==0x66);
      size_t timer=40+4+(mode?0xc000:0x4000)+0x2000+0xa0+0x80+8+(mode?8:0)+
        sizeof(struct cpu_regs)+sizeof(struct gb_regs)+(mode?sizeof(struct gbc_regs)+128:0)+
        16+(mode?24:0)+2*sizeof(struct apu_stat)+0x30+(mode?1:0)+80;
      CHECK(!memcmp(state+timer,"TMR1",4));
      CHECK(gbcore_state_save(c,after,sizeof after)==n);memcpy(bad,state,(size_t)n);bad[timer+4]=5;
      CHECK(gbcore_state_load(c,bad,(size_t)n)!=0);CHECK(gbcore_state_save(c,bad,sizeof bad)==n);CHECK(!memcmp(bad,after,(size_t)n));
      memcpy(bad,state,(size_t)n);bad[timer+4]=1;bad[timer+5]=1;
      CHECK(gbcore_state_load(c,bad,(size_t)n)!=0);CHECK(gbcore_state_save(c,bad,sizeof bad)==n);CHECK(!memcmp(bad,after,(size_t)n));
      memcpy(bad,state,(size_t)n);memset(bad+timer,0,8);CHECK(gbcore_state_load(c,bad,(size_t)n)==0);
      cpu_write(0xff05,0x77);CHECK(cpu_read(0xff05)==0x77); /* legacy extension loads idle */
      gbcore_set_headless(c,1);CHECK(gbcore_state_load(c,state,(size_t)n)==0);
      cpu_write(0xff05,0x77);CHECK(cpu_read(0xff05)==0x55);
      gbcore_set_headless(c,0);
      byte *r=get_rom();r[0x200]=0xc3;r[0x201]=0;r[0x202]=2;c_regs_PC=0x200;
      n=gbcore_state_save(c,state,sizeof state);CHECK(n>0);
      uint64_t hashes[8], sounds[8];unsigned counts[8];
      for(int phase=0;phase<2;phase++) {
        const byte *saved=phase?state:pending_state;size_t length=(size_t)(phase?n:pending_n);
        gbcore_set_headless(c,0);CHECK(gbcore_state_load(c,saved,length)==0);
        for(int i=0;i<8;i++){audio_hash=1;audio_count=0;gbcore_run_frame(c,0);hashes[i]=gbcore_sync_hash(c);sounds[i]=audio_hash;counts[i]=audio_count;CHECK(audio_count>0);}
        CHECK(gbcore_state_load(c,saved,length)==0);
        for(int i=0;i<8;i++){audio_hash=1;audio_count=0;gbcore_run_frame(c,0);CHECK(gbcore_sync_hash(c)==hashes[i]);CHECK(audio_hash==sounds[i]);CHECK(audio_count==counts[i]);}
        CHECK(gbcore_state_load(c,saved,length)==0);gbcore_set_headless(c,1);
        for(int i=0;i<8;i++){gbcore_run_frame(c,0);CHECK(gbcore_sync_hash(c)==hashes[i]);}
      }
    }
    if(!strcmp(which,"all")||!strcmp(which,"halt")) {
      byte *r=get_rom();
      for(int enabled=0;enabled<2;enabled++) {
        memset(r+0x150,0,0x200);reset();cpu_write(0xff07,6);cpu_write(0xff05,0xff);cpu_write(0xff06,0x55);ticks(64,mode);
        CHECK(cpu_read(0xff05)==0);CHECK(!(cpu_read(0xff0f)&4));
        if(!enabled)cpu_write(0xff07,0);
        r[c_regs_PC]=0x76;ticks(4,mode);CHECK(cpu_read(0xff05)==0x55);CHECK(cpu_read(0xff0f)&4);
        ticks(4,mode);cpu_write(0xff05,0x77);CHECK(cpu_read(0xff05)==0x77); /* HALT expires reload window */
      }
      memset(r+0x150,0,0x200);reset();cpu_write(0xff07,6);cpu_write(0xff05,0xff);cpu_write(0xff06,0xfc);r[c_regs_PC]=0x76;
      ticks(1024,mode);CHECK(cpu_read(0xff05)==0xff);CHECK(cpu_read(0xff04)==4);CHECK(cpu_get_clock()==1024);CHECK(*cpu_get_halt());
      ticks(64,mode);CHECK(cpu_read(0xff05)==0);ticks(4,mode);CHECK(cpu_read(0xff05)==0xfc);
      memset(r+0x150,0,0x200);reset();r[c_regs_PC]=0x76;ticks(512,mode);
      byte apu_phase[36];apu_save_cpu_timing(apu_phase);
      CHECK(cpu_get_clock()==512);CHECK(cpu_read(0xff04)==2);
      CHECK((unsigned)(apu_phase[0]|(apu_phase[1]<<8))==(mode==2?256u:512u));
      for(int ime=0;ime<2;ime++) {
        memset(r+0x150,0,0x200);reset();cpu_write(0xff07,6);cpu_write(0xff05,0xff);cpu_write(0xff06,0x55);ticks(64,mode);
        cpu_write(0xffff,4);struct cpu_regs *regs=cpu_get_c_regs();regs->I=(byte)ime;cpu_set_c_regs();
        word next=(word)(c_regs_PC+1);r[c_regs_PC]=0x76;ticks(4,mode);CHECK(cpu_read(0xff0f)&4);ticks(4,mode);
        regs=cpu_get_c_regs();CHECK(!*cpu_get_halt());CHECK(cpu_get_clock()==72);
        if(ime){CHECK(c_regs_PC==0x51);CHECK(regs->SP==0xfffc);CHECK(!(cpu_read(0xff0f)&4));}
        else {CHECK(c_regs_PC==(word)(next+1));CHECK(regs->SP==0xfffe);CHECK(cpu_read(0xff0f)&4);}
      }
      memset(r+0x150,0,0x200);reset();r[c_regs_PC]=0x76;cpu_write(0xffff,8);cpu_write(0xff01,0x35);cpu_write(0xff02,0x81);
      ticks(4096,mode);CHECK(cpu_read(0xff02)&0x80);ticks(4,mode);CHECK(!(cpu_read(0xff02)&0x80));CHECK(cpu_read(0xff0f)&8);
      ticks(4,mode);CHECK(!*cpu_get_halt());CHECK(c_regs_PC==0x152);CHECK(cpu_get_c_regs()->SP==0xfffe);
    }
    gbcore_shutdown(c);printf("PASS timer %s mode=%d\n",which,mode);
  }
  return 0;
}
