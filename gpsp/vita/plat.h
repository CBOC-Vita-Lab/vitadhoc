/* plat.h -- the machine underneath GBAdhoc's Vita frontend.
 *
 * Everything that is a Sony system call and belongs to the app itself sits
 * behind this header, in plat_vita.c (the renderer's are vback_vita2d.c's,
 * the UI's file and pad calls pspcompat.c's, the network's net_vita.c's).
 * main_vita.c and audio_vita.c are plain C against it, so the SAME files
 * build for Linux against hostcore/plat_host.c -- headless: the composed
 * picture goes to a PNG, the audio port to a WAV, the pad comes from a
 * script -- and are tested there, x86-64 with the x86 dynarec and armhf
 * under qemu-arm with the ARM dynarec the Vita uses.
 *
 * Files and directories need no entries here: vitasdk's newlib gives fopen,
 * mkdir and opendir on "ux0:" paths.
 */
#ifndef GBADHOC_PLAT_H
#define GBADHOC_PLAT_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- lifetime ----------------------------------------------------------- */

/* Clocks to full speed, pad sampling mode, power callback.  0 = ok. */
int  plat_init(int argc, char **argv);
/* Leave the process (sceKernelExitProcess).  Does not return. */
void plat_exit(int code);
/* Where the app keeps its files: "ux0:data/GBAdhoc" on a Vita. */
const char *plat_data_root(void);
/* Where the package's own read-only files are: "app0:" on a Vita (the
 * bundled cheat database). */
const char *plat_app_dir(void);
/* What a launcher asked this start to play: everything after "param=" in
 * the launch URI (psgm:play?titleid=GBAD00001&param=ux0:...), taken as it
 * is -- not decoded, so a name with '%', '&' or '+' arrives unchanged
 * (GitHub issue #3).  0 = there is one, copied into out; -2 = there is one
 * too long for out; -1 = none. */
int  plat_launch_param(char *out, size_t cap);
/* Start a separate program of this package ("snes.bin": snes/, the SNES
 * emulator, a different executable because Snes9x's licence is not
 * GPL-compatible) with one argument, a game's path.  The process is
 * replaced; the program starts this one again when it ends.  Returns only
 * when it could not be started (the error code); the twin never starts one
 * and returns -1. */
int  plat_exec_program(const char *program, const char *arg);
/* Nonzero when the platform wants the main loop to end (the host driver's
 * frame budget or script; never on a Vita, where the player quits). */
int  plat_quit_requested(void);
/* A line for the kernel console as well as the log (sceClibPrintf): what
 * survives if the log file itself cannot be opened. */
void plat_console(const char *line);
/* Log (fe_evt) where the program, the translation caches and the heap sit
 * and how far apart they are.  The ARM dynarec calls its stubs with a 24-bit
 * branch, which reaches 32 MiB: this line is how a hardware run says whether
 * that holds (vita/jit_vita.c). */
void plat_memory_report(void);
/* Bytes the C heap can still hand out: what is left of the heap block plus
 * what malloc holds free.  (size_t)-1 = no figure (a Linux process: as much
 * as anyone asks for).  rewind_vita.c sizes its buffer by it, so a heap that
 * came up smaller than asked (heap_vita.c halves its request until the
 * kernel agrees) costs the rewind its length, not the link its second
 * cartridge. */
size_t plat_heap_free(void);

/* ---- time, the display's clock, power ------------------------------------ */

uint64_t plat_time_us(void);              /* monotonic */
void     plat_sleep_us(uint32_t us);
time_t   plat_wallclock(void);            /* seconds since 1970, local RTC */
/* Vertical blanks since boot, and a wait for the next one.  The pacer
 * targets an absolute count (main_vita.c pace_frame), so a late frame uses
 * up its budget instead of buying a whole extra blank. */
unsigned plat_vcount(void);
void     plat_wait_vblank(void);
/* The panel's refresh rate in millihertz (60000; an OLED Vita reports
 * 59940), for the log and the resampler's starting point. */
unsigned plat_refresh_mhz(void);
/* The processor's clock in MHz as the system reports it to the app (what
 * plat_init asked for: 444; an overclocking plugin may run it faster and
 * still answer 444), and as measured: a loop of a known length timed
 * (about 4 ms; a core may be shared, so it is a lower bound).  The twin:
 * 0 and 0.  For the log. */
unsigned plat_arm_mhz(void);
unsigned plat_arm_mhz_measured(void);
/* Ask for the CPU clock (500 or 444 MHz; the bus is set to 222, the GPU to
 * 222 and the crossbar to 166).  0 or more: the system took it; negative:
 * refused (no overclock plugin).  The twin: refused for 500 when
 * GBADHOC_FAKE_REFUSE500 is set. */
int      plat_cpu_set_mhz(int mhz);
/* Where an overclock plugin keeps its files, and our title id; "" = none
 * (the twin: folders under GBADHOC_FAKE_UR0, for the tests). */
const char *plat_tai_dir(void);            /* ur0:tai                      */
const char *plat_psvshell_dir(void);       /* ur0:data/PSVshell/profiles   */
const char *plat_title_id(void);           /* GBAD00001                    */
/* Keep the console awake: once per emulated frame (not in the menus, where
 * an idle console should dim and sleep as the owner set it to). */
void     plat_power_tick(void);
/* Power events since the last call, PLAT_POWER_* bits.  (On the Vita this is
 * also where pending callbacks are delivered: the UI's modal screens wait
 * for the blank without the callback variant of the call.) */
#define PLAT_POWER_SUSPEND 1u   /* the system is about to suspend the app */
#define PLAT_POWER_RESUME  2u   /* ...and has brought it back            */
unsigned plat_power_events(void);
/* The raw notice of the last power callback (SCE_POWER_CB_* bits on the
 * Vita; 0 before any), for the log. */
unsigned plat_power_info(void);

/* ---- the twin's two hooks (empty on a Vita) -------------------------------- */

/* One emulated frame was run.  The twin's simulated clock moves by the cost
 * it gives a frame (--work-us), which is what makes fast forward mean
 * something on a clock that otherwise only moves when the app waits. */
void plat_frame_ran(void);
/* The composed picture is complete and about to be presented: called before
 * every vid_swap of the game loop, and once per refresh inside the UI's
 * modal screens (after their wait for the blank).  The twin writes its
 * --shot screenshots here. */
void plat_present_hook(void);

/* ---- audio out -------------------------------------------------------- */

/* A stereo signed 16-bit port at `rate` Hz taking `grain` frames per write.
 * plat_audio_write blocks until the port has room, which is what paces the
 * audio thread.  A negative return is an error the caller may answer by
 * closing and opening again (a port lost across a suspend). */
int  plat_audio_open(unsigned rate, unsigned grain);
int  plat_audio_write(const int16_t *stereo);
/* Only a successful checked close retires all submitted PCM storage. It
 * may block while draining; call it on the consumer while one exists. */
int  plat_audio_close_checked(void);
void plat_audio_close(void);
/* Run step() over and over on the audio thread until it returns nonzero or
 * plat_audio_thread_stop() is called. Start refuses an unretired consumer.
 * The host driver has no thread: it calls step() from its simulated clock,
 * one call per grain of simulated time, so a run is reproducible. */
int  plat_audio_thread_start(int (*step)(void));
/* 1: no consumer remains; 0: still owned, so do not close/reuse its storage.
 * Native waits at most two seconds. Retry after a timeout or delete error.
 * Lifecycle calls are serialized on the controlling/emulation thread. */
int  plat_audio_thread_stop_checked(void);
void plat_audio_thread_stop(void);

/* vita.ini vm_probe (jit_vita.h): asks the console whether a second thread
 * may run and write translated code, and writes what happened, step by
 * step, to `report_path` -- each step is announced there before it is
 * tried, so that a crash names it.  mode: 1 = the second thread opens the
 * VM domain first; 2 = it does not; 3 = it first runs code without having
 * opened it, then opens it.  Nothing on the twin. */
void plat_vm_probe(int mode, const char *report_path);

/* Let another thread of this process run: a real pause of a tenth of a
 * millisecond or so.  Not plat_sleep_us: on the twin that moves the
 * SIMULATED clock, and a frame thread waiting out the worker thread's
 * critical section must not age the frame it is in (rewind_vita.c). */
void plat_thread_yield(void);

/* ---- where the threads run (the log's vita_threads line) ----------------- */

/* Watch thread `thid` (0 = the calling one) under `tag` (a literal). */
void plat_thread_watch(const char *tag, int thid);
/* One line: per watched thread, the core it last ran on, its affinity mask,
 * the microseconds it ran and the times it was preempted (by threads / by
 * interrupts) and moved between cores since the last call.  "" on a twin. */
void plat_thread_report(char *buf, size_t cap);
/* Keep the calling thread on core `core` (0-2 of the application's three);
 * -1 = leave it where the system put it.  0 = done. */
int  plat_thread_pin(int core);

/* ---- a background thread (the game list's box art, artfetch.h) ---------- */

/* One low-priority thread running fn(arg) to its end; 0 = started (at most
 * one at a time). */
int  plat_bg_start(void (*fn)(void *), void *arg);
/* Wait up to timeout_us (real time) for it to end: 1 = it did and is gone,
 * 0 = it is still running (it is left to finish by itself). */
int  plat_bg_join(uint32_t timeout_us);
/* A second one, for the network's requests (ra_http.h: RetroAchievements),
 * with the stack a TLS handshake needs.  Same contract. */
int  plat_net_start(void (*fn)(void *), void *arg);
int  plat_net_join(uint32_t timeout_us);
/* Sleep in REAL time, on a thread other than the frame thread (on the twin
 * plat_sleep_us moves the simulated clock). */
void plat_thread_sleep_us(uint32_t us);

/* ---- a background worker (the event log and the .sav writer) ------------- */

/* A low-priority thread: step() runs after every plat_worker_wake() and at
 * least every `period_us`.  0 = started; nonzero = no thread, and the caller
 * stays synchronous.  plat_worker_stop() gives step() a last call on the
 * worker and waits for it. */
int  plat_worker_start(void (*step)(void), uint32_t period_us);
void plat_worker_wake(void);
void plat_worker_stop(void);
/* 1 = joined, 0 = still owns its storage after a bounded wait. Never start
 * a replacement consumer or drain its data on main after a 0 result. */
int  plat_worker_stop_checked(void);

/* ---- the motion sensor (SceMotion) ----------------------------------------
 *
 * For the sensor cartridges (frontend-common/fe_sensor.h): a reading in that
 * header's terms -- right_g / down_g = how much of gravity points to the
 * screen's right / bottom edge (0 level, 1 that edge straight down),
 * rate_z = rad/s about the axis through the screen, positive anticlockwise
 * as the player looks at it.  The twin's come from its script (motion=). */
typedef struct plat_motion
{
   float right_g, down_g, rate_z;
} plat_motion;
/* Start (1) or stop (0) sampling; sampling costs battery, so only while a
 * sensor game runs.  0 = done; -1 = this console has no sensor (a PS TV). */
int  plat_motion_enable(int on);
/* This moment's reading.  0 = read; -1 = none (not sampling, or no sensor). */
int  plat_motion_read(plat_motion *m);

/* ---- the pad ------------------------------------------------------------- */

/* Button bits: the Vita's own (SCE_CTRL_*), so plat_vita.c copies the word. */
#define PLAT_BTN_SELECT    0x0001u
#define PLAT_BTN_START     0x0008u
#define PLAT_BTN_UP        0x0010u
#define PLAT_BTN_RIGHT     0x0020u
#define PLAT_BTN_DOWN      0x0040u
#define PLAT_BTN_LEFT      0x0080u
#define PLAT_BTN_L         0x0100u   /* the Vita's L (SCE_CTRL_LTRIGGER)  */
#define PLAT_BTN_R         0x0200u   /* the Vita's R (SCE_CTRL_RTRIGGER)  */
#define PLAT_BTN_L1        0x0400u   /* a paired DualShock's L1 (PS TV)   */
#define PLAT_BTN_R1        0x0800u
#define PLAT_BTN_TRIANGLE  0x1000u
#define PLAT_BTN_CIRCLE    0x2000u
#define PLAT_BTN_CROSS     0x4000u
#define PLAT_BTN_SQUARE    0x8000u

typedef struct plat_pad
{
   uint32_t buttons;       /* PLAT_BTN_* */
   uint8_t  lx, ly;        /* left stick, 0..255, 128 = centre */
   uint8_t  rx, ry;        /* right stick, likewise */
} plat_pad;

void plat_pad_read(plat_pad *pad);
/* A AND B BUTTONS: 1 = plat_pad_read / plat_pad_read2 hand CROSS and CIRCLE
 * to the app exchanged (the physical Circle comes back as PLAT_BTN_CROSS and
 * the other way round), so the app's whole input -- menus, game buttons,
 * shortcuts -- follows one setting.  0 (the start) = as pressed. */
void plat_pad_face_swap(int on);
/* How many players have a pad of their own: 1, or 2 when a second
 * controller can drive the partner's machine of the local link.  Cheap
 * (the answer is refreshed about three times a second, so a pad plugged in
 * or switched off is noticed within a moment).
 *
 * WHICH PORT IS PLAYER 2 (plat_vita.c).  sceCtrl's port 0 is the console's
 * own controls -- on a PS TV, merged with the first paired controller, so
 * player 1 is always port 0.  Paired controllers sit on ports 1..4 in order
 * (SceCtrlPortInfo.port[]: DualShock 3, DualShock 4, a plugin's pad); on a
 * PS TV port 1 is the first pad again, so the SECOND pad is port 2.  Player
 * 2 is the first of ports 2, 3, 4 that is paired (a PS TV with two pads; a
 * handheld with a plugin that pairs two); else, on a HANDHELD only, port 1
 * when one is reported there (one external pad through a plugin: the
 * console's own controls are then player 1's alone).  "Paired" is enough:
 * a pad that lies untouched is still player 2.  The second pad is read with
 * the extended call (L1 and L2 both come back as PLAT_BTN_L, R1 and R2 as
 * PLAT_BTN_R; the sticks' clicks are dropped). */
int  plat_pad_ports(void);
/* The second controller: 1 = there is one and *pad is its state. */
int  plat_pad_read2(plat_pad *pad);

/* Every finger on the front panel, native 960x544 (the Vita's touch panel
 * reports up to six; the twin's script names as many as it likes).  Returns
 * how many were written to out[0..max-1].  For the touch pad of player 2
 * (vita/local_vita.c); pspcompat_touch is the UI's one finger. */
typedef struct plat_touch_pt { int x, y; } plat_touch_pt;
int  plat_touch_points(plat_touch_pt *out, int max);

/* ---- the twin's script: a finger and a keyboard ----------------------------
 * The Vita's touch panel and system keyboard are pspcompat.c's (the UI's).
 * On the twin pspcompat asks callbacks for them; these are the callbacks,
 * answered from plat_host.c's script (main_vita.c registers them with
 * pspcompat_host_touch / pspcompat_host_ime, which do nothing on a Vita,
 * where these two are never called). */
int         plat_script_touch(int *nx, int *ny);
const char *plat_script_ime(const char *title, const char *initial,
                            int frames);

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_PLAT_H */
