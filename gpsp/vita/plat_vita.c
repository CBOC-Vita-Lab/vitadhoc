/* plat_vita.c -- plat.h for the PS Vita: the Sony calls the app itself makes
 * (the memory ones are heap_vita.c / jit_vita.c's, the renderer's
 * vback_vita2d.c's, the UI's files and pad pspcompat.c's).
 *
 * Nothing here could be run before the first hardware test: it is compiled
 * and linked against vitasdk, and each call is used the way the vitasdk
 * samples and VitaShell use it.  The comments say where behaviour on a real
 * console is assumed rather than known.
 */
#include "plat.h"
#include "jit_vita.h"
#include "fe_evt.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/rtc.h>

/* The emulation thread is the main thread.  The dynarec's block translator
 * recurses (a block's exits are translated eagerly), a few hundred bytes a
 * level; the PSP runs it on 256 KiB and measures its depth.  1 MiB here.
 * vita-elf-create reads this symbol into the process parameters. */
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

/* Thread priorities: lower is more urgent, the default user priority is
 * 0x10000100 (160).  Sound must never wait for the emulator; the worker only
 * runs when nothing else wants a core. */
#define PRIO_AUDIO   0x50
#define PRIO_WORKER  0xB0

/* ---- lifetime ------------------------------------------------------------- */

static volatile unsigned g_power_events;
static volatile unsigned g_power_info;
static int g_power_cb = -1;

/* Runs on the main thread, inside sceDisplayWaitVblankStartCB (callbacks are
 * only delivered to a thread that waits in a *CB call).  It only records:
 * the main loop does the work (plat_power_events). */
static int power_callback(int notify_id, int notify_count, int info, void *user)
{
   (void)notify_id; (void)notify_count; (void)user;
   g_power_info = (unsigned)info;
   if (info & (SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_SYSTEM_SUSPEND))
      g_power_events |= PLAT_POWER_SUSPEND;
   /* An ordinary application is not told BEFORE it is suspended
    * (SCE_POWER_CB_VALID_MASK_NON_SYSTEM has neither suspend bit): the
    * resume is the only notice.  So a resume reports both, and the main
    * loop does its suspend work (end a link session and say why, take the
    * snapshot the wake screen shows) and then its resume work, in the order
    * the twin tests them.  The battery save does not depend on this: it is
    * written within seconds of the game changing it. */
   if (info & (SCE_POWER_CB_APP_RESUME | SCE_POWER_CB_SYSTEM_RESUME))
      g_power_events |= PLAT_POWER_SUSPEND | PLAT_POWER_RESUME;
   return 0;
}

int plat_init(int argc, char **argv)
{
   (void)argc; (void)argv;

   /* Full speed: 444 MHz CPU, 222 MHz bus and GPU, 166 MHz crossbar -- what
    * every emulator on the system asks for.  The defaults (333/111) are a
    * power-saving choice for the LiveArea. */
   scePowerSetArmClockFrequency(444);
   scePowerSetBusClockFrequency(222);
   scePowerSetGpuClockFrequency(222);
   scePowerSetGpuXbarClockFrequency(166);

   /* Analog mode, or the left stick reads as centred. */
   sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

   g_power_cb = sceKernelCreateCallback("gbadhoc_power", 0, power_callback, NULL);
   if (g_power_cb >= 0)
      scePowerRegisterCallback(g_power_cb);

   return 0;
}

void plat_exit(int code)
{
   sceKernelExitProcess(code);
   for (;;)
      sceKernelDelayThread(1000000);      /* not reached */
}

const char *plat_data_root(void)
{
   return "ux0:data/GBAdhoc";
}

const char *plat_app_dir(void)
{
   return "app0:";
}

/* The twin's hooks: the console has a real clock and a real screen. */
void plat_frame_ran(void)    { }
void plat_present_hook(void) { }

int plat_quit_requested(void)
{
   return 0;
}

void plat_console(const char *line)
{
   sceClibPrintf("%s", line);
}

static void report_line(const char *fmt, ...)
{
   char buf[256];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   fe_evt("%s", buf);
   sceClibPrintf("GBAdhoc: %s\n", buf);
   (void)buf;
}

void plat_memory_report(void)
{
   gbadhoc_mem_report(report_line);
}

size_t plat_heap_free(void)
{
   /* heap_vita.c's block less what sbrk has handed to malloc, plus what
    * malloc has free inside that. */
   struct mallinfo mi = mallinfo();
   uint32_t used = gbadhoc_heap_used();
   size_t rest = gbadhoc_mem.heap_bytes > used
               ? (size_t)(gbadhoc_mem.heap_bytes - used) : 0;
   return rest + (size_t)mi.fordblks;
}

/* ---- time, display clock, power ------------------------------------------- */

uint64_t plat_time_us(void)
{
   return sceKernelGetProcessTimeWide();
}

void plat_sleep_us(uint32_t us)
{
   sceKernelDelayThread(us);
}

/* ---- vm_probe (jit_vita.h) -------------------------------------------- */

static struct { int mode; const char *path; } g_vmp;

static void vmp_say(const char *fmt, ...)
{
   char line[200];
   va_list ap;
   FILE *f;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   f = fopen(g_vmp.path, "a");
   if (f)
   {
      fputs(line, f);
      fputc('\n', f);
      fclose(f);                  /* on the card before the step is tried */
   }
}

static int vmp_step(int step, const char *what)
{
   int value = -9999, rc;
   vmp_say("step %d begins: %s", step, what);
   rc = gpsp_vita_jit_probe(step, &value);
   vmp_say("step %d done: rc=%d value=%d (0x%08x)", step, rc, value,
           (unsigned)value);
   return rc;
}

static int vmp_thread(SceSize n, void *p)
{
   (void)n; (void)p;
   if (g_vmp.mode & 2)
      vmp_step(1, "the second thread runs code the first wrote (expects 41)");
   if (g_vmp.mode & 1)
      vmp_step(2, "the second thread calls sceKernelOpenVMDomain");
   vmp_step(3, "the second thread writes code, publishes and runs it (expects 43)");
   return sceKernelExitDeleteThread(0);
}

void plat_vm_probe(int mode, const char *report_path)
{
   SceUID th;
   int st = 0;
   g_vmp.mode = mode;
   g_vmp.path = report_path;
   vmp_say("vm_probe mode=%d (a line 'finished' ends a run that did not crash)",
           mode);
   if (vmp_step(0, "the first thread writes code, publishes and runs it (expects 41)") < 0)
   {
      vmp_say("no page in the VM block: finished");
      return;
   }
   th = sceKernelCreateThread("gbadhoc_vmprobe", vmp_thread, 0x10000100,
                              0x10000, 0, 0, NULL);
   if (th < 0 || sceKernelStartThread(th, 0, NULL) < 0)
   {
      vmp_say("no second thread (0x%08x): finished", (unsigned)th);
      return;
   }
   sceKernelWaitThreadEnd(th, &st, NULL);
   vmp_step(4, "the first thread writes and runs again (expects 44)");
   vmp_say("finished");
}

void plat_thread_yield(void)
{
   sceKernelDelayThread(100);
}

time_t plat_wallclock(void)
{
   /* The console's clock as the player sees it, encoded as seconds since
    * 1970 with no timezone applied -- the convention of gba_memory.c's
    * gpsp_wallclock and fe_host_config.wallclock. */
   SceDateTime dt;
   time_t t = 0;
   if (sceRtcGetCurrentClockLocalTime(&dt) < 0 || sceRtcGetTime_t(&dt, &t) < 0)
      return time(NULL);
   return t;
}

/* The pacer counts refreshes with a 32-bit target and compares by signed
 * difference (main_vita.c), so this must count on past 65535: the system's
 * counter is taken as 16 bits wide (that is what Vita3K implements; on the
 * console it is at least that) and extended here.  Called at least once a
 * frame, from the main thread only, so one wrap is never missed.  Without
 * this the game would stop for good after 65536 refreshes, 18 minutes. */
unsigned plat_vcount(void)
{
   static unsigned hi, last;
   unsigned v = (unsigned)sceDisplayGetVcount() & 0xFFFFu;
   if (v < last)
      hi += 0x10000u;
   last = v;
   return hi | v;
}

void plat_wait_vblank(void)
{
   sceDisplayWaitVblankStartCB();
}

unsigned plat_arm_mhz(void)
{
   int mhz = scePowerGetArmClockFrequency();
   return mhz > 0 ? (unsigned)mhz : 0;
}

/* SUBS + BNE: one iteration a cycle on the Cortex-A9 (the branch predicted
 * taken).  The best of three runs, against an interruption. */
unsigned plat_arm_mhz_measured(void)
{
   const unsigned n = 2000000u;
   uint64_t best = ~0ull;
   int k;
   for (k = 0; k < 3; k++)
   {
      unsigned i = n;
      const uint64_t t0 = sceKernelGetProcessTimeWide();
      __asm__ volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(i) : : "cc");
      {
         const uint64_t dt = sceKernelGetProcessTimeWide() - t0;
         if (dt && dt < best)
            best = dt;
      }
   }
   return best != ~0ull ? (unsigned)(n / best) : 0;
}

unsigned plat_refresh_mhz(void)
{
   float fps = 0.0f;
   unsigned mhz;
   if (sceDisplayGetRefreshRate(&fps) < 0)
      return 60000;
   mhz = (unsigned)(fps * 1000.0f + 0.5f);
   /* An LCD model reports 60.00, an OLED 59.94.  Anything else is not a
    * number to build the audio rate on. */
   return (mhz >= 59000 && mhz <= 61000) ? mhz : 60000;
}

void plat_power_tick(void)
{
   sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
}

unsigned plat_power_info(void)
{
   return g_power_info;
}

unsigned plat_power_events(void)
{
   unsigned e;
   /* The power callback is only delivered to a thread inside a ...CB wait.
    * The game loop's pacer waits that way (plat_wait_vblank); the UI's modal
    * screens wait with the plain call, so ask for pending callbacks here. */
   sceKernelCheckCallback();
   e = g_power_events;
   if (e)
      g_power_events &= ~e;
   return e;
}

/* ---- audio out -------------------------------------------------------------- */

static int g_port = -1;

int plat_audio_open(unsigned rate, unsigned grain)
{
   int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
   /* MAIN when the rate is its one rate, 48 kHz (which is what audio_vita.c
    * asks for): the system never mutes it.  The BGM port takes any standard
    * rate but is silenced while the owner's own music plays in the
    * background.  (vitaQuake opens MAIN at 48 kHz too.) */
   g_port = -1;
   if (rate == 48000)
      g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, (int)grain,
                                   48000, SCE_AUDIO_OUT_MODE_STEREO);
   if (g_port < 0)
      g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, (int)grain,
                                   (int)rate, SCE_AUDIO_OUT_MODE_STEREO);
   if (g_port < 0)
      return g_port;
   sceAudioOutSetVolume(g_port, SCE_AUDIO_VOLUME_FLAG_L_CH |
                                SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
   return 0;
}

int plat_audio_write(const int16_t *stereo)
{
   if (g_port < 0)
      return -1;
   /* Blocks until the port has taken the grain: this call is the audio
    * thread's clock. */
   return sceAudioOutOutput(g_port, stereo);
}

void plat_audio_close(void)
{
   if (g_port >= 0)
      sceAudioOutReleasePort(g_port);
   g_port = -1;
}

static SceUID g_audio_thid = -1;
static int (*g_audio_step)(void);
static volatile int g_audio_stop;

static int audio_entry(SceSize args, void *argp)
{
   (void)args; (void)argp;
   while (!g_audio_stop && g_audio_step() == 0)
      ;
   return 0;
}

int plat_audio_thread_start(int (*step)(void))
{
   g_audio_step = step;
   g_audio_stop = 0;
   g_audio_thid = sceKernelCreateThread("gbadhoc_audio", audio_entry,
                                        PRIO_AUDIO, 0x10000, 0, 0, NULL);
   if (g_audio_thid < 0)
      return -1;
   if (sceKernelStartThread(g_audio_thid, 0, NULL) < 0)
   {
      sceKernelDeleteThread(g_audio_thid);
      g_audio_thid = -1;
      return -1;
   }
   plat_thread_watch("audio", g_audio_thid);
   return 0;
}

void plat_audio_thread_stop(void)
{
   SceUInt tmo = 2000000;
   if (g_audio_thid < 0)
      return;
   g_audio_stop = 1;
   /* The thread is at most one grain (11 ms) from noticing; if the port
    * call itself is stuck, leave it behind rather than hang the exit. */
   if (sceKernelWaitThreadEnd(g_audio_thid, NULL, &tmo) >= 0)
      sceKernelDeleteThread(g_audio_thid);
   g_audio_thid = -1;
}

/* ---- where the threads run ------------------------------------------------ */

#define WATCH_MAX 8
static struct
{
   const char *tag;
   SceUID id;
   uint64_t run;
   unsigned pre, irq, moved;
} g_watch[WATCH_MAX];
static int g_nwatch;

void plat_thread_watch(const char *tag, int thid)
{
   int i;
   if (!thid)
      thid = sceKernelGetThreadId();
   if (thid <= 0)
      return;
   for (i = 0; i < g_nwatch; i++)
      if (g_watch[i].id == thid)
      {
         g_watch[i].tag = tag;
         return;
      }
   if (g_nwatch >= WATCH_MAX)
      return;
   memset(&g_watch[g_nwatch], 0, sizeof(g_watch[0]));
   g_watch[g_nwatch].tag = tag;
   g_watch[g_nwatch].id = thid;
   g_nwatch++;
}

void plat_thread_report(char *buf, size_t cap)
{
   size_t n = 0;
   int i;
   if (!buf || !cap)
      return;
   buf[0] = '\0';
   for (i = 0; i < g_nwatch; i++)
   {
      SceKernelThreadInfo info;
      uint64_t run;
      memset(&info, 0, sizeof(info));
      info.size = sizeof(info);
      if (sceKernelGetThreadInfo(g_watch[i].id, &info) < 0)
         continue;            /* gone (its game ended) */
      run = (uint64_t)info.runClocks;
      n += (size_t)snprintf(buf + n, n < cap ? cap - n : 0,
                            "%s%s=c%d/%x/%u/%u/%u/%u", n ? " " : "",
                            g_watch[i].tag, (int)info.lastExecutedCpuId,
                            (unsigned)info.currentCpuAffinityMask >> 16,
                            (unsigned)(run - g_watch[i].run),
                            info.threadPreemptCount - g_watch[i].pre,
                            info.intrPreemptCount - g_watch[i].irq,
                            (unsigned)info.changeCpuCount - g_watch[i].moved);
      g_watch[i].run = run;
      g_watch[i].pre = info.threadPreemptCount;
      g_watch[i].irq = info.intrPreemptCount;
      g_watch[i].moved = (unsigned)info.changeCpuCount;
      if (n >= cap)
         break;
   }
}

int plat_thread_pin(int core)
{
   if (core < 0 || core > 2)
      return 0;
   return sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),
                                               SCE_KERNEL_CPU_MASK_USER_0
                                               << core) < 0 ? -1 : 0;
}

/* ---- a background thread ------------------------------------------------- */

#define PRIO_BG      0xB8      /* below the worker: whatever is left over */

static SceUID g_bg_thid = -1;
static void (*g_bg_fn)(void *);
static void *g_bg_arg;

static int bg_entry(SceSize args, void *argp)
{
   (void)args; (void)argp;
   g_bg_fn(g_bg_arg);
   return 0;
}

int plat_bg_start(void (*fn)(void *), void *arg)
{
   if (g_bg_thid >= 0)
      return -1;
   g_bg_fn = fn;
   g_bg_arg = arg;
   /* 64 KiB: sockets, a few paths, the CRC's reads come from the heap */
   g_bg_thid = sceKernelCreateThread("gbadhoc_bg", bg_entry, PRIO_BG,
                                     0x10000, 0, 0, NULL);
   if (g_bg_thid < 0 || sceKernelStartThread(g_bg_thid, 0, NULL) < 0)
   {
      if (g_bg_thid >= 0)
         sceKernelDeleteThread(g_bg_thid);
      g_bg_thid = -1;
      return -1;
   }
   plat_thread_watch("bg", g_bg_thid);
   return 0;
}

int plat_bg_join(uint32_t timeout_us)
{
   SceUInt tmo = timeout_us;
   if (g_bg_thid < 0)
      return 1;
   if (sceKernelWaitThreadEnd(g_bg_thid, NULL, &tmo) < 0)
      return 0;
   sceKernelDeleteThread(g_bg_thid);
   g_bg_thid = -1;
   return 1;
}

void plat_thread_sleep_us(uint32_t us)
{
   sceKernelDelayThread(us);
}

/* ---- the worker ------------------------------------------------------------- */

static SceUID g_wk_thid = -1, g_wk_sema = -1;
static void (*g_wk_step)(void);
static volatile int g_wk_run;
static uint32_t g_wk_period;

static int worker_entry(SceSize args, void *argp)
{
   (void)args; (void)argp;
   while (g_wk_run)
   {
      SceUInt tmo = g_wk_period;
      sceKernelWaitSema(g_wk_sema, 1, &tmo);   /* a wake, or the period */
      g_wk_step();
   }
   g_wk_step();                                /* the last pass */
   return 0;
}

int plat_worker_start(void (*step)(void), uint32_t period_us)
{
   g_wk_step = step;
   g_wk_period = period_us;
   /* Counting, with a ceiling: a burst of wakes collapses into a few. */
   g_wk_sema = sceKernelCreateSema("gbadhoc_io", 0, 0, 64, NULL);
   if (g_wk_sema < 0)
      return -1;
   g_wk_run = 1;
   g_wk_thid = sceKernelCreateThread("gbadhoc_io", worker_entry, PRIO_WORKER,
                                     0x10000, 0, 0, NULL);
   if (g_wk_thid < 0 || sceKernelStartThread(g_wk_thid, 0, NULL) < 0)
   {
      if (g_wk_thid >= 0)
         sceKernelDeleteThread(g_wk_thid);
      sceKernelDeleteSema(g_wk_sema);
      g_wk_thid = g_wk_sema = -1;
      g_wk_run = 0;
      return -1;
   }
   plat_thread_watch("io", g_wk_thid);
   return 0;
}

void plat_worker_wake(void)
{
   if (g_wk_sema >= 0)
      sceKernelSignalSema(g_wk_sema, 1);   /* an error = already at the cap */
}

void plat_worker_stop(void)
{
   SceUInt tmo = 5000000;       /* the last pass may write a whole save */
   if (g_wk_thid < 0)
      return;
   g_wk_run = 0;
   sceKernelSignalSema(g_wk_sema, 1);
   if (sceKernelWaitThreadEnd(g_wk_thid, NULL, &tmo) >= 0)
      sceKernelDeleteThread(g_wk_thid);
   sceKernelDeleteSema(g_wk_sema);
   g_wk_thid = g_wk_sema = -1;
}

/* ---- the pad ---------------------------------------------------------------- */

void plat_pad_read(plat_pad *pad)
{
   SceCtrlData d;
   memset(&d, 0, sizeof(d));
   /* Peek, not Read: Read blocks until the next sample (a vblank). */
   if (sceCtrlPeekBufferPositive(0, &d, 1) < 0)
   {
      pad->buttons = 0;
      pad->lx = pad->ly = pad->rx = pad->ry = 128;
      return;
   }
   pad->buttons = d.buttons;     /* PLAT_BTN_* are the SCE_CTRL_* values */
   pad->lx = d.lx;
   pad->ly = d.ly;
   pad->rx = d.rx;
   pad->ry = d.ry;
}

/* Player 2's port, or 0 when there is none (plat.h has the rule).  From the
 * SDK's description and the controller plugins' conventions: not run. */
static int pad2_port(void)
{
   static int port, age;
   if (age-- <= 0)
   {
      SceCtrlPortInfo info;
      age = 60;                  /* a controller appears within a second */
      port = 0;
      memset(&info, 0, sizeof(info));
      if (sceCtrlGetControllerPortInfo(&info) >= 0)
      {
         if (info.port[2] != SCE_CTRL_TYPE_UNPAIRED)
            port = 2;
         else if (info.port[1] != SCE_CTRL_TYPE_UNPAIRED &&
                  !sceKernelIsPSVitaTV())
            port = 1;
      }
   }
   return port;
}

int plat_pad_ports(void)
{
   return pad2_port() ? 2 : 1;
}

int plat_pad_read2(plat_pad *pad)
{
   SceCtrlData d;
   int port = pad2_port();
   if (!port)
      return 0;
   memset(&d, 0, sizeof(d));
   d.lx = d.ly = d.rx = d.ry = 128;
   if (sceCtrlPeekBufferPositive2(port, &d, 1) < 0)
      return 0;
   pad->buttons = d.buttons;
   pad->lx = d.lx;
   pad->ly = d.ly;
   pad->rx = d.rx;
   pad->ry = d.ry;
   return 1;
}

/* The twin's script (plat.h): never called on the console. */
int plat_script_touch(int *nx, int *ny)
{
   (void)nx; (void)ny;
   return 0;
}

const char *plat_script_ime(const char *title, const char *initial, int frames)
{
   (void)title; (void)initial; (void)frames;
   return NULL;
}
