/* pspcompat.c -- see pspcompat.h.
 *
 * Two bodies behind one set of pc_* functions: the PS Vita's own calls when
 * built by arm-vita-eabi-gcc (__vita__), POSIX for the host twin.  The Vita
 * body is thin on purpose -- sceIo on the Vita has the PSP's semantics and
 * even the PSP's flag values, so each function is a line or two -- and
 * native rather than newlib's POSIX layer because sceIoDread hands back each
 * entry's size and type with the name: the ROM scan needs no stat per file.
 */
#define PSPCOMPAT_IMPL
#include "pspcompat.h"

#include <stdio.h>
#include <string.h>

char g_dir_base[128] =
#ifdef __vita__
   PSPCOMPAT_ROOT_VITA;
#else
   PSPCOMPAT_ROOT_HOST;
#endif

void pspcompat_set_root(const char *root)
{
   size_t n;
   if (!root || !root[0])
      return;
   snprintf(g_dir_base, sizeof(g_dir_base), "%s", root);
   n = strlen(g_dir_base);
   /* "ux0:" alone keeps its colon; a trailing slash goes. */
   while (n > 1 && g_dir_base[n - 1] == '/')
      g_dir_base[--n] = '\0';
}

const char *pspcompat_root(void)
{
   return g_dir_base;
}

int pspcompat_path(char *out, size_t out_sz, const char *rel)
{
   int n;
   if (!out || !out_sz)
      return -1;
   n = snprintf(out, out_sz, "%s/%s", g_dir_base, rel ? rel : "");
   if (n < 0 || (size_t)n >= out_sz)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

int pspcompat_make_dirs(void)
{
   static const char *const sub[] = { "roms", "boxart", "hero", "saves",
                                      "log" };
   char path[192];
   pc_stat st;
   unsigned i;
   int rc = 0;
   /* The parent of the default root, "ux0:data", exists on nearly every
    * Vita; creating it costs nothing when it does. */
#ifdef __vita__
   pc_io_mkdir("ux0:data", 0777);
#endif
   pc_io_mkdir(g_dir_base, 0777);
   if (pc_io_getstat(g_dir_base, &st) < 0)
      rc = -1;
   for (i = 0; i < sizeof(sub) / sizeof(sub[0]); i++)
   {
      if (pspcompat_path(path, sizeof(path), sub[i]) != 0)
      {
         rc = -1;
         continue;
      }
      pc_io_mkdir(path, 0777);
      if (pc_io_getstat(path, &st) < 0)
         rc = -1;
   }
   return rc;
}

/* The left stick as d-pad bits. */
static unsigned stick_bits(unsigned lx, unsigned ly)
{
   unsigned b = 0;
   int dx = (int)lx - 128, dy = (int)ly - 128;
   if (dx < -PSPCOMPAT_STICK_DEADZONE) b |= PSP_CTRL_LEFT;
   if (dx >  PSPCOMPAT_STICK_DEADZONE) b |= PSP_CTRL_RIGHT;
   if (dy < -PSPCOMPAT_STICK_DEADZONE) b |= PSP_CTRL_UP;
   if (dy >  PSPCOMPAT_STICK_DEADZONE) b |= PSP_CTRL_DOWN;
   return b;
}

int pc_ctrl_peek(pc_ctrl *pad, int count)
{
   (void)count;
   if (!pad)
      return -1;
   memset(pad, 0, sizeof(*pad));
   pad->Buttons = pspcompat_pad();
   pad->TimeStamp = pspcompat_time_us();
   pad->Lx = pad->Ly = 128;
   return 1;
}

/* What the keyboard returned, as the UI's two faces can draw it: printable
 * ASCII.  A UTF-8 sequence becomes one '?'; leading and trailing blanks go. */
static void ime_ascii(char *out, size_t cap, const unsigned char *utf8)
{
   size_t n = 0;
   if (!out || !cap)
      return;
   while (*utf8 == ' ')
      utf8++;
   while (*utf8 && n + 1 < cap)
   {
      unsigned c = *utf8++;
      if (c >= 0x80)
      {
         while ((*utf8 & 0xC0) == 0x80)
            utf8++;
         c = '?';
      }
      else if (c < 0x20 || c == 0x7F)
         c = ' ';
      out[n++] = (char)c;
   }
   while (n && out[n - 1] == ' ')
      n--;
   out[n] = '\0';
}

#ifdef __vita__
/* ======================================================================= */
/*                                PS VITA                                   */
/* ======================================================================= */
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>
#include <psp2/display.h>
#include <psp2/rtc.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

/* The flag values and the twelve button bits are the PSP's; if a future SDK
 * ever disagrees, fail the build rather than the browser. */
typedef char pc_flags_match[(SCE_O_RDONLY == PSP_O_RDONLY &&
                             SCE_O_WRONLY == PSP_O_WRONLY &&
                             SCE_O_CREAT == PSP_O_CREAT &&
                             SCE_O_TRUNC == PSP_O_TRUNC &&
                             SCE_SEEK_SET == PSP_SEEK_SET &&
                             SCE_SEEK_END == PSP_SEEK_END) ? 1 : -1];
typedef char pc_pad_match[(SCE_CTRL_SELECT == PSP_CTRL_SELECT &&
                           SCE_CTRL_START == PSP_CTRL_START &&
                           SCE_CTRL_UP == PSP_CTRL_UP &&
                           SCE_CTRL_RIGHT == PSP_CTRL_RIGHT &&
                           SCE_CTRL_DOWN == PSP_CTRL_DOWN &&
                           SCE_CTRL_LEFT == PSP_CTRL_LEFT &&
                           SCE_CTRL_LTRIGGER == PSP_CTRL_LTRIGGER &&
                           SCE_CTRL_RTRIGGER == PSP_CTRL_RTRIGGER &&
                           SCE_CTRL_TRIANGLE == PSP_CTRL_TRIANGLE &&
                           SCE_CTRL_CIRCLE == PSP_CTRL_CIRCLE &&
                           SCE_CTRL_CROSS == PSP_CTRL_CROSS &&
                           SCE_CTRL_SQUARE == PSP_CTRL_SQUARE) ? 1 : -1];

unsigned pspcompat_time_us(void)
{
   return (unsigned)sceKernelGetProcessTimeLow();
}

int pspcompat_battery_percent(void)
{
   int p = scePowerGetBatteryLifePercent();
   return p < 0 ? -1 : p;
}

int pspcompat_battery_charging(void)
{
   return scePowerIsBatteryCharging() == SCE_TRUE;
}

void pspcompat_clock(int *hour, int *minute)
{
   SceDateTime t;
   memset(&t, 0, sizeof(t));
   if (sceRtcGetCurrentClockLocalTime(&t) < 0)
   {
      *hour = 0;
      *minute = 0;
      return;
   }
   *hour = t.hour;
   *minute = t.minute;
}

static void (*g_vblank_fn)(void);

void pspcompat_wait_vblank(void)
{
   sceDisplayWaitVblankStart();
   if (g_vblank_fn)
      g_vblank_fn();
}

unsigned pspcompat_pad(void)
{
   static int analog_on;
   SceCtrlData pad;
   if (!analog_on)
   {
      /* Digital-only is the power-on mode: the stick reads 128,128. */
      sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
      analog_on = 1;
   }
   memset(&pad, 0, sizeof(pad));
   pad.lx = pad.ly = 128;
   if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0)
      return 0;
   /* Plain PeekBufferPositive reports the Vita's L and R -- and a PS TV
    * pad's L1/R1 -- as LTRIGGER/RTRIGGER, the PSP's shoulder bits. */
   return (pad.buttons & 0xF3F9u) | stick_bits(pad.lx, pad.ly);
}

void pspcompat_host_pad(unsigned (*fn)(void)) { (void)fn; }
void pspcompat_host_advance(unsigned us)      { (void)us; }
/* On the console the hook is the main loop's "a frame went by inside a modal
 * screen" service (power tick, quit request): the browser, the wake overlay
 * and the notices all wait for vblank once per frame. */
void pspcompat_host_vblank_hook(void (*fn)(void)) { g_vblank_fn = fn; }

void pspcompat_host_touch(int (*fn)(int *, int *)) { (void)fn; }
const char PSPCOMPAT_HOST_IME_CANCEL[] = "";
void pspcompat_host_ime(const char *(*fn)(const char *, const char *, int))
{
   (void)fn;
}
void pspcompat_host_user_name(const char *name) { (void)name; }

/* The front panel reports 1920x1088 over the 960x544 screen (psp2/touch.h;
 * sceTouchGetPanelInfo confirms it on every model): half of a report is a
 * native pixel, a quarter a logical one. */
int pspcompat_touch(pc_touch *out)
{
   static int started;
   SceTouchData td;
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));
   if (!started)
   {
      sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                               SCE_TOUCH_SAMPLING_STATE_START);
      started = 1;
   }
   memset(&td, 0, sizeof(td));
   if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) < 0 || td.reportNum < 1)
      return 0;
   out->down = 1;
   out->nx = td.report[0].x / 2;
   out->ny = td.report[0].y / 2;
   if (out->nx > 959) out->nx = 959;
   if (out->ny > 543) out->ny = 543;
   out->x = out->nx / 2;
   out->y = out->ny / 2;
   return 1;
}

void pspcompat_dialogs_init(void)
{
   static int done;
   SceAppUtilInitParam ip;
   SceAppUtilBootParam bp;
   SceCommonDialogConfigParam cp;
   if (done)
      return;
   done = 1;
   memset(&ip, 0, sizeof(ip));
   memset(&bp, 0, sizeof(bp));
   /* Already initialised by the main loop: an error we do not care about. */
   (void)sceAppUtilInit(&ip, &bp);
   sceCommonDialogConfigParamInit(&cp);
   (void)sceCommonDialogSetConfigParam(&cp);
}

void pspcompat_user_name(char *out, size_t cap)
{
   SceChar8 name[SCE_SYSTEM_PARAM_USERNAME_MAXSIZE + 1];
   if (!out || !cap)
      return;
   out[0] = '\0';
   pspcompat_dialogs_init();
   memset(name, 0, sizeof(name));
   if (sceAppUtilSystemParamGetString(SCE_SYSTEM_PARAM_ID_USERNAME, name,
                                      SCE_SYSTEM_PARAM_USERNAME_MAXSIZE) >= 0)
      ime_ascii(out, cap, (const unsigned char *)name);
}

/* ---- the system keyboard (after VitaShell's ime_dialog.c, TheFloW, GPLv3:
 * the parameter block and the UTF-16 <-> UTF-8 steps) -------------------- */
static struct
{
   int       on;
   SceWChar16 title[64];
   SceWChar16 initial[PC_IME_MAX_CHARS + 1];
   /* maxTextLength characters and the terminator */
   SceWChar16 input[PC_IME_MAX_CHARS + 1];
} g_ime;

static void ime_to16(SceWChar16 *dst, size_t cap, const char *src)
{
   size_t n = 0;
   for (; src && *src && n + 1 < cap; src++)
      dst[n++] = (SceWChar16)(unsigned char)(*src < 0x7F ? *src : '?');
   dst[n] = 0;
}

int pspcompat_ime_open(const char *title, const char *initial, int max_chars)
{
   SceImeDialogParam p;
   if (g_ime.on)
      return -1;
   pspcompat_dialogs_init();
   if (max_chars < 1) max_chars = 1;
   if (max_chars > PC_IME_MAX_CHARS) max_chars = PC_IME_MAX_CHARS;
   ime_to16(g_ime.title, sizeof(g_ime.title) / sizeof(g_ime.title[0]), title);
   ime_to16(g_ime.initial, sizeof(g_ime.initial) / sizeof(g_ime.initial[0]),
            initial);
   memset(g_ime.input, 0, sizeof(g_ime.input));
   sceImeDialogParamInit(&p);
   p.supportedLanguages = 0x0001FFFF;      /* every layout the console has */
   p.languagesForced = SCE_TRUE;
   p.type = SCE_IME_TYPE_BASIC_LATIN;
   p.option = 0;
   p.title = g_ime.title;
   p.maxTextLength = (SceUInt32)max_chars;
   p.initialText = g_ime.initial;
   p.inputTextBuffer = g_ime.input;
   if (sceImeDialogInit(&p) < 0)
      return -1;
   g_ime.on = 1;
   return 0;
}

int pspcompat_ime_poll(char *out, size_t cap)
{
   SceImeDialogResult res;
   unsigned char utf8[PC_IME_MAX_CHARS * 3 + 4];
   size_t i, n = 0;
   int accepted;
   if (!g_ime.on)
      return PC_IME_IDLE;
   if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
      return PC_IME_RUNNING;
   memset(&res, 0, sizeof(res));
   sceImeDialogGetResult(&res);
   accepted = res.button == SCE_IME_DIALOG_BUTTON_ENTER;
   sceImeDialogTerm();
   g_ime.on = 0;
   if (!accepted)
      return PC_IME_CANCELLED;
   for (i = 0; g_ime.input[i] && n + 4 < sizeof(utf8); i++)
   {
      unsigned c = g_ime.input[i];
      if (c < 0x80)
         utf8[n++] = (unsigned char)c;
      else if (c < 0x800)
      {
         utf8[n++] = (unsigned char)(0xC0 | (c >> 6));
         utf8[n++] = (unsigned char)(0x80 | (c & 0x3F));
      }
      else
      {
         utf8[n++] = (unsigned char)(0xE0 | (c >> 12));
         utf8[n++] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
         utf8[n++] = (unsigned char)(0x80 | (c & 0x3F));
      }
   }
   utf8[n] = 0;
   ime_ascii(out, cap, utf8);
   return PC_IME_DONE;
}

pc_uid pc_io_open(const char *path, int flags, int mode)
{
   return (pc_uid)sceIoOpen(path, flags, mode);
}

int pc_io_close(pc_uid fd)                       { return sceIoClose(fd); }
int pc_io_read(pc_uid fd, void *buf, pc_size n)  { return sceIoRead(fd, buf, n); }
int pc_io_write(pc_uid fd, const void *buf, pc_size n)
{
   return sceIoWrite(fd, buf, n);
}

long long pc_io_lseek(pc_uid fd, long long off, int whence)
{
   return (long long)sceIoLseek(fd, (SceOff)off, whence);
}

int pc_io_lseek32(pc_uid fd, int off, int whence)
{
   return (int)sceIoLseek32(fd, off, whence);
}

int pc_io_remove(const char *path)          { return sceIoRemove(path); }
int pc_io_rename(const char *from, const char *to)
{
   return sceIoRename(from, to);
}
unsigned long long pspcompat_time_wide_us(void)
{
   return (unsigned long long)sceKernelGetProcessTimeWide();
}
void pspcompat_delay_us(unsigned us) { sceKernelDelayThread(us); }
int pc_io_mkdir(const char *path, int mode) { return sceIoMkdir(path, mode); }

/* sceIo stamps are UTC; the page shows the console's own clock. */
static void stamp_local(pc_datetime *out, const SceDateTime *utc)
{
   SceRtcTick t, lt;
   SceDateTime d = *utc;
   if (sceRtcGetTick(utc, &t) >= 0 &&
       sceRtcConvertUtcToLocalTime(&t, &lt) >= 0)
      sceRtcSetTick(&d, &lt);
   out->year = d.year; out->month = d.month; out->day = d.day;
   out->hour = d.hour; out->minute = d.minute; out->second = d.second;
   out->microsecond = d.microsecond;
}

static void stat_out(pc_stat *out, const SceIoStat *in)
{
   memset(out, 0, sizeof(*out));
   out->st_mode = (unsigned)in->st_mode;
   out->st_attr = in->st_attr;
   out->st_size = (long long)in->st_size;
   stamp_local(&out->sce_st_ctime, &in->st_ctime);
   stamp_local(&out->sce_st_atime, &in->st_atime);
   stamp_local(&out->sce_st_mtime, &in->st_mtime);
}

int pc_io_getstat(const char *path, pc_stat *st)
{
   SceIoStat s;
   int rc;
   memset(&s, 0, sizeof(s));
   rc = sceIoGetstat(path, &s);
   if (rc >= 0 && st)
      stat_out(st, &s);
   return rc;
}

pc_uid pc_io_dopen(const char *path) { return (pc_uid)sceIoDopen(path); }
int    pc_io_dclose(pc_uid d)        { return sceIoDclose(d); }

int pc_io_dread(pc_uid d, pc_dirent *ent)
{
   SceIoDirent e;
   int rc;
   memset(&e, 0, sizeof(e));
   rc = sceIoDread(d, &e);
   if (rc > 0 && ent)
   {
      /* Type and size only: the scan reads nothing else, and converting
       * three time stamps per ROM would be three RTC calls for nothing. */
      memset(ent, 0, sizeof(*ent));
      ent->d_stat.st_mode = (unsigned)e.d_stat.st_mode;
      ent->d_stat.st_attr = e.d_stat.st_attr;
      ent->d_stat.st_size = (long long)e.d_stat.st_size;
      snprintf(ent->d_name, sizeof(ent->d_name), "%s", e.d_name);
   }
   return rc;
}

#else
/* ======================================================================= */
/*                            HOST TWIN (POSIX)                             */
/* ======================================================================= */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static unsigned (*g_pad_fn)(void);
static void (*g_vblank_fn)(void);

/* ---- touch, keyboard, user name: scripted --------------------------------- */
static int (*g_touch_fn)(int *, int *);
static const char *(*g_ime_fn)(const char *, const char *, int);
static struct { int on, frames, max; char title[64], initial[64]; } g_ime;
static char g_user_name[32];
const char PSPCOMPAT_HOST_IME_CANCEL[] = "\x18";

void pspcompat_host_touch(int (*fn)(int *, int *)) { g_touch_fn = fn; }
void pspcompat_host_ime(const char *(*fn)(const char *, const char *, int))
{
   g_ime_fn = fn;
}
void pspcompat_host_user_name(const char *name)
{
   snprintf(g_user_name, sizeof(g_user_name), "%s", name ? name : "");
}

int pspcompat_touch(pc_touch *out)
{
   int nx = 0, ny = 0;
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));
   if (!g_touch_fn || !g_touch_fn(&nx, &ny))
      return 0;
   if (nx < 0) nx = 0;
   if (ny < 0) ny = 0;
   if (nx > 959) nx = 959;
   if (ny > 543) ny = 543;
   out->down = 1;
   out->nx = nx; out->ny = ny;
   out->x = nx / 2; out->y = ny / 2;
   return 1;
}

void pspcompat_dialogs_init(void) { }

int  pspcompat_battery_percent(void) { return 80; }
int  pspcompat_battery_charging(void) { return 0; }
void pspcompat_clock(int *hour, int *minute) { *hour = 13; *minute = 34; }

void pspcompat_user_name(char *out, size_t cap)
{
   if (out && cap)
      ime_ascii(out, cap, (const unsigned char *)g_user_name);
}

int pspcompat_ime_open(const char *title, const char *initial, int max_chars)
{
   if (g_ime.on || !g_ime_fn)
      return -1;
   g_ime.on = 1;
   g_ime.frames = 0;
   g_ime.max = max_chars < 1 ? 1
             : max_chars > PC_IME_MAX_CHARS ? PC_IME_MAX_CHARS : max_chars;
   snprintf(g_ime.title, sizeof(g_ime.title), "%s", title ? title : "");
   snprintf(g_ime.initial, sizeof(g_ime.initial), "%s", initial ? initial : "");
   return 0;
}

int pspcompat_ime_poll(char *out, size_t cap)
{
   const char *t;
   char clip[PC_IME_MAX_CHARS + 1];
   if (!g_ime.on)
      return PC_IME_IDLE;
   t = g_ime_fn ? g_ime_fn(g_ime.title, g_ime.initial, g_ime.frames++)
                : PSPCOMPAT_HOST_IME_CANCEL;
   if (!t)
      return PC_IME_RUNNING;
   g_ime.on = 0;
   if (t == PSPCOMPAT_HOST_IME_CANCEL)
      return PC_IME_CANCELLED;
   snprintf(clip, sizeof(clip), "%.*s", g_ime.max, t);
   ime_ascii(out, cap, (const unsigned char *)clip);
   return PC_IME_DONE;
}

#ifdef PSPCOMPAT_PLAT
/* THE WHOLE APP ON A HOST (vita/hostapp): the clock, the display's blank and
 * the pad are plat.h's -- hostcore/plat_host.c's simulated (or real) time --
 * so the UI's modal screens and the main loop live on ONE timeline and one
 * input script. */
#include "plat.h"

unsigned pspcompat_time_us(void) { return (unsigned)plat_time_us(); }
void pspcompat_host_advance(unsigned us) { plat_sleep_us(us); }

void pspcompat_wait_vblank(void)
{
   plat_wait_vblank();
   if (g_vblank_fn)
      g_vblank_fn();
}

void pspcompat_host_pad(unsigned (*fn)(void)) { g_pad_fn = fn; }
void pspcompat_host_vblank_hook(void (*fn)(void)) { g_vblank_fn = fn; }

unsigned pspcompat_pad(void)
{
   plat_pad p;
   if (g_pad_fn)
      return g_pad_fn() & 0xF3F9u;
   plat_pad_read(&p);
   return (p.buttons & 0xF3F9u) | stick_bits(p.lx, p.ly);
}
#else
static unsigned g_clock_us = 1000000u;

unsigned pspcompat_time_us(void)
{
   g_clock_us += 20u;         /* time passes: nothing may divide by zero */
   return g_clock_us;
}

void pspcompat_host_advance(unsigned us) { g_clock_us += us; }

void pspcompat_wait_vblank(void)
{
   g_clock_us += 16667u;
   if (g_vblank_fn)
      g_vblank_fn();
}

void pspcompat_host_pad(unsigned (*fn)(void)) { g_pad_fn = fn; }
void pspcompat_host_vblank_hook(void (*fn)(void)) { g_vblank_fn = fn; }

unsigned pspcompat_pad(void)
{
   (void)stick_bits;
   return g_pad_fn ? (g_pad_fn() & 0xF3F9u) : 0u;
}
#endif /* PSPCOMPAT_PLAT */

pc_uid pc_io_open(const char *path, int flags, int mode)
{
   int f = 0, fd;
   (void)mode;
   if ((flags & PSP_O_RDWR) == PSP_O_RDWR) f = O_RDWR;
   else if (flags & PSP_O_WRONLY)          f = O_WRONLY;
   else                                    f = O_RDONLY;
   if (flags & PSP_O_CREAT)  f |= O_CREAT;
   if (flags & PSP_O_TRUNC)  f |= O_TRUNC;
   if (flags & PSP_O_APPEND) f |= O_APPEND;
   fd = open(path, f, 0644);
   return fd < 0 ? -1 : fd;
}

int pc_io_close(pc_uid fd) { return close(fd); }

int pc_io_read(pc_uid fd, void *buf, pc_size n)
{
   /* sceIoRead returns what the file had: loop over short reads. */
   size_t got = 0;
   while (got < n)
   {
      ssize_t r = read(fd, (char *)buf + got, n - got);
      if (r < 0)
         return got ? (int)got : -1;
      if (r == 0)
         break;
      got += (size_t)r;
   }
   return (int)got;
}

int pc_io_write(pc_uid fd, const void *buf, pc_size n)
{
   size_t put = 0;
   while (put < n)
   {
      ssize_t r = write(fd, (const char *)buf + put, n - put);
      if (r <= 0)
         return put ? (int)put : -1;
      put += (size_t)r;
   }
   return (int)put;
}

long long pc_io_lseek(pc_uid fd, long long off, int whence)
{
   return (long long)lseek(fd, (off_t)off, whence);
}

int pc_io_lseek32(pc_uid fd, int off, int whence)
{
   return (int)lseek(fd, (off_t)off, whence);
}

int pc_io_remove(const char *path) { return unlink(path) == 0 ? 0 : -1; }
int pc_io_rename(const char *from, const char *to)
{
   return rename(from, to) == 0 ? 0 : -1;
}
/* The host's clock is pspcompat_time_us (the twin's simulated time when
 * there is one), carried into 64 bits; a delay is that clock advancing. */
unsigned long long pspcompat_time_wide_us(void)
{
   static unsigned last, hi;
   unsigned now = pspcompat_time_us();
   if (now < last)
      hi++;
   last = now;
   return ((unsigned long long)hi << 32) | now;
}
void pspcompat_delay_us(unsigned us) { pspcompat_host_advance(us); }

int pc_io_mkdir(const char *path, int mode)
{
   (void)mode;
   return mkdir(path, 0755) == 0 ? 0 : -1;
}

static void stamp_local(pc_datetime *out, time_t t)
{
   struct tm tm;
   memset(&tm, 0, sizeof(tm));
   localtime_r(&t, &tm);
   out->year = (unsigned short)(tm.tm_year + 1900);
   out->month = (unsigned short)(tm.tm_mon + 1);
   out->day = (unsigned short)tm.tm_mday;
   out->hour = (unsigned short)tm.tm_hour;
   out->minute = (unsigned short)tm.tm_min;
   out->second = (unsigned short)tm.tm_sec;
   out->microsecond = 0;
}

static void stat_out(pc_stat *out, const struct stat *in)
{
   int dir = S_ISDIR(in->st_mode);
   memset(out, 0, sizeof(*out));
   out->st_mode = dir ? 0x1000u : 0x2000u;
   out->st_attr = dir ? 0x0010u : 0x0020u;
   out->st_size = (long long)in->st_size;
   stamp_local(&out->sce_st_ctime, in->st_ctime);
   stamp_local(&out->sce_st_atime, in->st_atime);
   stamp_local(&out->sce_st_mtime, in->st_mtime);
}

int pc_io_getstat(const char *path, pc_stat *st)
{
   struct stat s;
   if (stat(path, &s) != 0)
      return -1;
   if (st)
      stat_out(st, &s);
   return 0;
}

#define PC_DIRS 8
static struct { DIR *d; char path[512]; } g_dirs[PC_DIRS];

pc_uid pc_io_dopen(const char *path)
{
   int i;
   for (i = 0; i < PC_DIRS; i++)
      if (!g_dirs[i].d)
      {
         g_dirs[i].d = opendir(path);
         if (!g_dirs[i].d)
            return -1;
         snprintf(g_dirs[i].path, sizeof(g_dirs[i].path), "%s", path);
         return i;
      }
   return -1;
}

int pc_io_dread(pc_uid d, pc_dirent *ent)
{
   struct dirent *e;
   struct stat s;
   char full[800];
   if (d < 0 || d >= PC_DIRS || !g_dirs[d].d || !ent)
      return -1;
   e = readdir(g_dirs[d].d);
   if (!e)
      return 0;
   memset(ent, 0, sizeof(*ent));
   snprintf(ent->d_name, sizeof(ent->d_name), "%s", e->d_name);
   snprintf(full, sizeof(full), "%s/%s", g_dirs[d].path, e->d_name);
   if (stat(full, &s) == 0)
      stat_out(&ent->d_stat, &s);
   return 1;
}

int pc_io_dclose(pc_uid d)
{
   if (d < 0 || d >= PC_DIRS || !g_dirs[d].d)
      return -1;
   closedir(g_dirs[d].d);
   g_dirs[d].d = NULL;
   return 0;
}

#endif /* __vita__ */
