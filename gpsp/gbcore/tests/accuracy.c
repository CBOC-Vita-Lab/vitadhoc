/* Regression oracles for review F9/F10. Original synthetic cartridge only. */
#include "../gbcore.h"
#include "../tgbdual/gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__func__,__LINE__,#x); exit(1); } } while (0)
static byte rom[32768], state[1<<20];
static unsigned audio_count;
static uint64_t audio_hash;
static word pixel;
static void video(void *u, const gbcore_video_frame_t *f) { (void)u; pixel=f->pixels[0]; }
static void audio(void *u,const int16_t *p,size_t n) {
  (void)u; audio_count+=(unsigned)n;
  for(size_t i=0;i<n*2;i++) audio_hash=(audio_hash^(uint16_t)p[i])*1099511628211ULL;
}
static gbcore_t *boot(int color) {
  gbcore_callbacks_t cb={0}; byte sum=0;
  memset(rom,0,sizeof rom); rom[0x100]=0xc3;rom[0x101]=0x50;rom[0x102]=1;
  memcpy(rom+0x134,"ACCURACY",8); rom[0x143]=color?0x80:0;
  rom[0x146]=3;rom[0x14b]=0x33;
  for(unsigned i=0x134;i<=0x14c;i++)sum=(byte)(sum-rom[i]-1);
  rom[0x14d]=sum; rom[0x150]=0xc3;rom[0x151]=0x50;rom[0x152]=1;
  cb.video=video;cb.audio_batch=audio;
  gbcore_set_palette(GBCORE_PALETTE_AUTO);
  gbcore_t *c=gbcore_create(NULL,rom,sizeof rom,color?FE_CONSOLE_GBC:FE_CONSOLE_GB,32768,&cb);
  CHECK(c); return c;
}
static void tick(unsigned n) { while(n) {unsigned v=n>8192?8192:n;cpu_exec((unsigned short)v);n-=v;} }
static unsigned status(void) { return cpu_read(0xff26)&15; }
static void power(void) { cpu_write(0xff26,0);cpu_write(0xff04,0);cpu_write(0xff26,0x80); }
static void trigger(int ch,int length,int env) {
  const word len[]={0xff11,0xff16,0xff1b,0xff20},vol[]={0xff12,0xff17,0xff1c,0xff21},trig[]={0xff14,0xff19,0xff1e,0xff23};
  if(ch==2)cpu_write(0xff1a,0x80);
  cpu_write(len[ch],(byte)((ch==2?256:64)-length));
  cpu_write(vol[ch],(byte)env);cpu_write(trig[ch],0xc0);
}
static void send_packet(const byte p[16]) {
  cpu_write(0xff00,0);cpu_write(0xff00,0x30);
  for(int i=0;i<16;i++)for(int b=0;b<8;b++){cpu_write(0xff00,(p[i]&(1<<b))?0x10:0x20);cpu_write(0xff00,0x30);}
  cpu_write(0xff00,0x20);cpu_write(0xff00,0x30);
}
static void packet(byte mode) {
  byte p[16]={0};p[0]=(0x17<<3)|1;p[1]=mode;send_packet(p);
}
static void masks(void) {
  gbcore_t *c=boot(0);
  const word colors[]={0x7fff,0x001f,0x03e0,0x7c00,0x1234};
  for(unsigned i=0;i<sizeof colors/sizeof *colors;i++) {
    sgb_set_color(0,colors[i]);packet(3);gbcore_run_frame(c,0);
    CHECK(pixel==CONVERT_COLOR15(colors[i]));
    CHECK(vframe[VFRAME_SIZE/2-1]==pixel);
    packet(2);gbcore_run_frame(c,0);CHECK(pixel==0);
  }
  packet(3);gbcore_run_frame(c,0);
  long n=gbcore_state_save(c,state,sizeof state);CHECK(n>0);
  packet(2);gbcore_run_frame(c,0);CHECK(pixel==0);
  CHECK(gbcore_state_load(c,state,(size_t)n)==0);gbcore_run_frame(c,0);
  CHECK(pixel==CONVERT_COLOR15(0x1234));
  gbcore_set_headless(c,1);packet(3);gbcore_run_frame(c,0);
  gbcore_set_headless(c,0);gbcore_run_frame(c,0);CHECK(pixel==CONVERT_COLOR15(0x1234));
  gbcore_shutdown(c); puts("PASS SGB MASK_EN colors, black, guard span, state and headless");
}
static void invalid_packets(int check) {
  gbcore_t *c=boot(0);byte p[16]={0};
  for(int i=0;i<10;i++){send_packet(p);if(check){CHECK(packets_received==0);CHECK(sgb_state==0);}}
  packet(3);CHECK(sgb_mask==3);CHECK(packets_received==0);
  gbcore_shutdown(c);puts("PASS SGB zero packet count rejected and valid command recovers");
}
static void attributes(void) {
  gbcore_t *c=boot(0);byte p[16]={0};p[0]=(7<<3)|1;
  p[3]=1;p[6]=0xff;
  for(int vertical=0;vertical<2;vertical++) {
    memset(sgb_ATF,2,sizeof sgb_ATF);p[5]=(byte)vertical;send_packet(p);
    CHECK(sgb_ATF[0]==3);
    for(int i=1;i<360;i++)CHECK(sgb_ATF[i]==2);
  }
  memset(sgb_ATF,2,sizeof sgb_ATF);p[3]=5;p[5]=0;p[6]=0xff;p[7]=0xff;send_packet(p);
  for(int i=0;i<360;i++)CHECK(sgb_ATF[i]==(i<5?3:2));
  gbcore_shutdown(c);puts("PASS SGB ATTR_CHR exact character count in both directions");
}
static void channels(int color,int fast) {
  gbcore_t *c=boot(color);
  if(fast){int dat[8];cpu_save_state(dat);dat[2]=1;cpu_restore_state(dat);}
  for(int ch=0;ch<4;ch++) {
    power();trigger(ch,1,ch==2?0:0xf0);
    CHECK(status()==(1u<<ch)); /* NR32 mute is independent of active status */
    tick(4096);CHECK(status()==(1u<<ch));
    tick(4096);CHECK(status()==0);
  }
  power();trigger(0,64,0x08);CHECK(status()==1); /* zero volume, DAC enabled */
  cpu_write(0xff14,0x80);tick(65536);CHECK(status()==1);
  cpu_write(0xff12,0);CHECK(status()==0);cpu_write(0xff14,0x80);CHECK(status()==0);
  power();trigger(1,64,0xf0);cpu_write(0xff1c,0x20);CHECK(status()==2);
  cpu_write(0xff17,0);CHECK(status()==0);
  power();cpu_write(0xff1a,0x80);cpu_write(0xff1b,0xff);CHECK(status()==0);
  cpu_write(0xff1e,0x80);CHECK(status()==4);cpu_write(0xff1a,0);CHECK(status()==0);
  power();trigger(3,64,0x08);CHECK(status()==8);cpu_write(0xff21,0);CHECK(status()==0);
  power();trigger(0,64,0xf0);cpu_write(0xff26,0);CHECK(cpu_read(0xff26)==0x70);
  cpu_write(0xff12,0xf0);cpu_write(0xff14,0x80);CHECK(status()==0);
  cpu_write(0xff30,0x5a);CHECK(cpu_read(0xff30)==0x5a);
  power();trigger(0,1,0xf0);tick(4096);cpu_write(0xff04,0);CHECK(status()==0);
  power();cpu_write(0xff10,0x11);cpu_write(0xff12,0xf0);cpu_write(0xff13,0xff);cpu_write(0xff14,0x87);CHECK(status()==0);
  power();trigger(0,2,0xf0);tick(8192);CHECK(apu_get_stat_cpu()->sq1_len==1);
  cpu_write(0xff14,0xc0);CHECK(apu_get_stat_cpu()->sq1_len==1); /* trigger preserves remaining length */
  power();trigger(1,64,0x19);cpu_write(0xff19,0x80);
  tick(65536);CHECK(apu_get_stat_cpu()->sq2_vol==2);CHECK(status()==2);
  power();cpu_write(0xff10,0x11);cpu_write(0xff12,0xf0);cpu_write(0xff13,0);cpu_write(0xff14,0x82);
  tick(24576);CHECK(apu_get_stat_cpu()->sq1_freq==768);CHECK(status()==1);
  tick(32768);CHECK(apu_get_stat_cpu()->sq1_freq==1152);CHECK(status()==1);
  tick(32768);CHECK(apu_get_stat_cpu()->sq1_freq==1728);CHECK(status()==0);
  gbcore_shutdown(c);printf("PASS NR52/length/DAC/power/DIV/envelope/sweep mode=%s speed=%d\n",color?"CGB":"SGB",fast+1);
}
static void restore(int color) {
  gbcore_t *c=boot(color);long n;
  power();trigger(0,40,0xa2);cpu_write(0xff24,0x77);cpu_write(0xff25,0x11);
  gbcore_run_frame(c,0);n=gbcore_state_save(c,state,sizeof state);CHECK(n>0);
  uint64_t hash[12],sound[12];unsigned count[12];byte st[12];
  for(int i=0;i<12;i++){audio_hash=1;audio_count=0;gbcore_run_frame(c,0);hash[i]=gbcore_sync_hash(c);sound[i]=audio_hash;count[i]=audio_count;st[i]=status();}
  CHECK(gbcore_state_load(c,state,(size_t)n)==0);
  for(int i=0;i<12;i++){audio_hash=1;audio_count=0;gbcore_run_frame(c,0);CHECK(status()==st[i]);CHECK(gbcore_sync_hash(c)==hash[i]);CHECK(audio_hash==sound[i]);CHECK(audio_count==count[i]);}
  CHECK(gbcore_state_load(c,state,(size_t)n)==0);gbcore_set_headless(c,1);
  for(int i=0;i<12;i++){gbcore_run_frame(c,0);CHECK(status()==st[i]);CHECK(gbcore_sync_hash(c)==hash[i]);}
  gbcore_set_headless(c,0);gbcore_run_frame(c,0);CHECK(status()==0);
  gbcore_shutdown(c);printf("PASS state replay: sync, audio samples/hash, headless parity mode=%s\n",color?"CGB":"SGB");
}
static void malformed(int color) {
  static byte bad[1<<20], after[1<<20];
  gbcore_t *c=boot(color);
  gbcore_run_frame(c,0);
  long n=gbcore_state_save(c,state,sizeof state);CHECK(n>0);
  size_t bank=40+4+(color?0xc000:0x4000)+0x2000+0xa0+0x80;
  size_t apu=bank+8+(color?8:0)+sizeof(struct cpu_regs)+sizeof(struct gb_regs)+
             (color?sizeof(struct gbc_regs)+128:0)+16+(color?24:0);
  size_t offsets[6]={bank,bank+4,apu+offsetof(struct apu_stat,sq1_type),apu+offsetof(struct apu_stat,sq1_sw_shift),bank+8,bank+12};
  for(unsigned i=0;i<(color?6u:4u);i++) {
    memcpy(bad,state,(size_t)n);int invalid=10000;memcpy(bad+offsets[i],&invalid,sizeof invalid);
    CHECK(gbcore_state_load(c,bad,(size_t)n)!=0);
    CHECK(gbcore_state_save(c,after,sizeof after)==n);
    CHECK(!memcmp(after,state,(size_t)n));
  }
  CHECK(gbcore_state_load(c,state,(size_t)n)==0);
  /* Legacy state extension was all zero. It remains loadable. */
  size_t reserve=apu+2*sizeof(struct apu_stat)+0x30+(color?1:0);
  memcpy(bad,state,(size_t)n);memset(bad+reserve,0,256);memset(bad+n-12,0,12);
  CHECK(gbcore_state_load(c,bad,(size_t)n)==0);
  gbcore_run_frame(c,0);gbcore_shutdown(c);
  printf("PASS malformed bank/APU rejection is atomic; legacy extension loads mode=%s\n",color?"CGB":"SGB");
}
int main(int argc,char **argv) {
  if(argc>1&&!strcmp(argv[1],"mask")) {masks();return 0;}
  if(argc>1&&!strcmp(argv[1],"apu")) {channels(0,0);return 0;}
  if(argc>1&&!strcmp(argv[1],"packet-overrun")) {invalid_packets(0);return 0;}
  if(argc>1&&!strcmp(argv[1],"packet")) {invalid_packets(1);return 0;}
  if(argc>1&&!strcmp(argv[1],"attr")) {attributes();return 0;}
  if(argc>1&&!strcmp(argv[1],"state")) {malformed(1);return 0;}
  masks();invalid_packets(1);attributes();channels(0,0);channels(1,0);channels(1,1);restore(0);restore(1);malformed(0);malformed(1);
  puts("PASS GB accuracy regression suite");return 0;
}
