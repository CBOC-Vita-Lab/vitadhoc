/* pspcompat.h -- the PSP SDK surface the UI uses, for the PS Vita and for
 * the Linux host twin.
 *
 * psp/ui_psp.c, osd_psp.c, config_psp.c and ctl_map.c are the PSP frontend's
 * UI.  Between them they touch a small part of the PSP SDK: sceIo* files and
 * directories, the microsecond clock, the pad, the vblank wait and the
 * D-cache writeback the GE needs before it reads a texture.  Under -DVITA
 * those files include THIS header instead of <pspkernel.h> & co., and keep
 * every call site as it is.
 *
 * The names are remapped by macro onto pc_* functions implemented in
 * pspcompat.c -- natively (sceIo, sceCtrl, sceRtc, sceDisplay) when built by
 * arm-vita-eabi-gcc, on POSIX otherwise.  Macros rather than same-named
 * functions because the Vita SDK has functions with these very names and
 * DIFFERENT shapes (sceCtrlPeekBufferPositive takes a port; SceCtrlData
 * spells its fields in lower case; SceIoStat's time is st_mtime, not
 * sce_st_mtime), so the two sets of headers must never meet in one file:
 * a file that includes pspcompat.h must not include <psp2/...> headers.
 * pspcompat.c defines PSPCOMPAT_IMPL to get the declarations without the
 * remapping.
 *
 * WHAT MAPS TO WHAT
 *   sceIoOpen/Close/Read/Write/Lseek/Lseek32/Remove/Mkdir/Getstat,
 *   sceIoDopen/Dread/Dclose      pc_io_*  (same flags, same return rules:
 *                                >= 0 good, < 0 error; Dread > 0 = an entry)
 *   SceIoStat / SceIoDirent      pc_stat / pc_dirent: st_mode 0x1000 and
 *                                st_attr 0x10 both mark a directory (the
 *                                browser tests both), st_size, sce_st_mtime
 *                                in LOCAL time
 *   sceKernelGetSystemTimeLow    pspcompat_time_us()
 *   sceCtrlPeekBufferPositive    pc_ctrl_peek(): Buttons in the PSP's bit
 *                                layout.  The Vita's SCE_CTRL_* bits ARE the
 *                                PSP's for the twelve buttons the UI knows;
 *                                the left stick is folded into the d-pad
 *                                (dead zone PSPCOMPAT_STICK_DEADZONE)
 *   sceDisplayWaitVblankStart    pspcompat_wait_vblank()
 *   sceKernelDcacheWritebackRange  vid_tex_invalidate(): "these texture
 *                                bytes changed" is exactly what the GPU
 *                                texture cache in video_vita.c must hear
 *
 * THE DATA ROOT.  The PSP derives everything from its EBOOT's folder,
 * `g_dir_base` (ms0:/PSP/GAME/GBAdhoc): roms/, boxart/, hero/, saves/, log/,
 * config.ini.  Here g_dir_base is DEFINED in pspcompat.c and is the single
 * runtime-settable prefix: "ux0:data/GBAdhoc" by default on the Vita.  Call
 * pspcompat_set_root() before anything else to move it (uma0:, a test
 * directory on the host...).  A main loop must NOT define g_dir_base itself.
 */
#ifndef PSPCOMPAT_H
#define PSPCOMPAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the data root ------------------------------------------------------- */
#define PSPCOMPAT_ROOT_VITA "ux0:data/GBAdhoc"
#define PSPCOMPAT_ROOT_HOST "vita_root"

extern char g_dir_base[128];
/* Copies `root` (no trailing slash kept) into g_dir_base. */
void        pspcompat_set_root(const char *root);
const char *pspcompat_root(void);
/* "<root>/<rel>" into out; 0 = fits, -1 = too long (out is ""). */
int         pspcompat_path(char *out, size_t out_sz, const char *rel);
/* Create the root and roms/ boxart/ hero/ saves/ log/ under it (an existing
 * directory is fine).  0 = all present afterwards. */
int         pspcompat_make_dirs(void);

/* ---- clock, vblank, pad -------------------------------------------------- */
unsigned pspcompat_time_us(void);
/* For the home screen's top bar: the battery's charge 0..100 (-1 unknown),
 * whether it is charging, and the local time.  The host twin answers 80,
 * no, 13:34, so its pictures do not change with the clock. */
int  pspcompat_battery_percent(void);
int  pspcompat_battery_charging(void);
void pspcompat_clock(int *hour, int *minute);
void     pspcompat_wait_vblank(void);
/* Buttons held now, PSP bit layout, left stick folded into the d-pad.  A
 * main loop can use this for its own menu input too. */
unsigned pspcompat_pad(void);
#define PSPCOMPAT_STICK_DEADZONE 56      /* of 128, from centre */

#define PSP_CTRL_SELECT   0x000001u
#define PSP_CTRL_START    0x000008u
#define PSP_CTRL_UP       0x000010u
#define PSP_CTRL_RIGHT    0x000020u
#define PSP_CTRL_DOWN     0x000040u
#define PSP_CTRL_LEFT     0x000080u
#define PSP_CTRL_LTRIGGER 0x000100u
#define PSP_CTRL_RTRIGGER 0x000200u
#define PSP_CTRL_TRIANGLE 0x001000u
#define PSP_CTRL_CIRCLE   0x002000u
#define PSP_CTRL_CROSS    0x004000u
#define PSP_CTRL_SQUARE   0x008000u

/* ---- files --------------------------------------------------------------- */
#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_RDWR   0x0003
#define PSP_O_APPEND 0x0100
#define PSP_O_CREAT  0x0200
#define PSP_O_TRUNC  0x0400
#define PSP_SEEK_SET 0
#define PSP_SEEK_CUR 1
#define PSP_SEEK_END 2

typedef int      pc_uid;
typedef unsigned pc_size;

typedef struct
{
   unsigned short year, month, day, hour, minute, second;
   unsigned int   microsecond;
} pc_datetime;

typedef struct
{
   unsigned    st_mode;         /* 0x1000 directory, 0x2000 file           */
   unsigned    st_attr;         /* 0x0010 directory, 0x0020 file           */
   long long   st_size;
   pc_datetime sce_st_ctime, sce_st_atime, sce_st_mtime;   /* local time   */
} pc_stat;

typedef struct
{
   pc_stat d_stat;
   char    d_name[256];
} pc_dirent;

typedef struct
{
   unsigned      TimeStamp;
   unsigned      Buttons;
   unsigned char Lx, Ly;
} pc_ctrl;

pc_uid    pc_io_open(const char *path, int flags, int mode);
int       pc_io_close(pc_uid fd);
int       pc_io_read(pc_uid fd, void *buf, pc_size n);
int       pc_io_write(pc_uid fd, const void *buf, pc_size n);
long long pc_io_lseek(pc_uid fd, long long off, int whence);
int       pc_io_lseek32(pc_uid fd, int off, int whence);
int       pc_io_remove(const char *path);
int       pc_io_rename(const char *from, const char *to);
/* sceKernelGetSystemTimeWide / sceKernelDelayThread, for the PSP code that
 * uses them (Share game data): the Vita's own, the twin's clock. */
unsigned long long pspcompat_time_wide_us(void);
void      pspcompat_delay_us(unsigned us);
typedef long long pc_off;
int       pc_io_mkdir(const char *path, int mode);
int       pc_io_getstat(const char *path, pc_stat *st);
pc_uid    pc_io_dopen(const char *path);
int       pc_io_dread(pc_uid d, pc_dirent *ent);
int       pc_io_dclose(pc_uid d);
int       pc_ctrl_peek(pc_ctrl *pad, int count);

/* Completed compatibility-call boundaries, not lower-level host syscalls.
 * Mark the main/menu owner once before creating workers. An unmarked caller
 * counts as other; later mark attempts cannot change the owner. No paths,
 * contents, allocation, logger calls, locks, or worker waits are involved.
 * Errors mean negative public results; bytes mean returned bytes, not requested
 * bytes. Counters use lock-free 32-bit atomics and wrap modulo 2^32 (except
 * max_us). A single call's duration is also modulo 2^32 microseconds.
 * A snapshot loads each scalar independently: concurrent fields need not be
 * mutually consistent. Timing uses a real clock, not the host virtual clock. */
enum
{
   PC_IO_OPEN, PC_IO_CLOSE, PC_IO_READ, PC_IO_WRITE,
   PC_IO_LSEEK, PC_IO_LSEEK32, PC_IO_REMOVE, PC_IO_RENAME,
   PC_IO_MKDIR, PC_IO_GETSTAT, PC_IO_DOPEN, PC_IO_DREAD, PC_IO_DCLOSE,
   PC_IO_OP_COUNT
};
typedef struct
{
   uint32_t calls, errors, read_bytes, write_bytes, total_us, max_us;
   uint32_t op_calls[PC_IO_OP_COUNT];
} pc_io_diag_counts;
typedef struct
{
   uint32_t owner_marked;
   pc_io_diag_counts owner, other;
} pc_io_diag;
void pspcompat_io_mark_owner(void);
void pspcompat_io_get_diag(pc_io_diag *out);

/* video_vita.h */
void vid_tex_invalidate(const void *p, size_t bytes);

/* ---- touch: the front panel ------------------------------------------------
 * One finger (the first the panel reports), where it is NOW.  Vita: sceTouch
 * on SCE_TOUCH_PORT_FRONT (sampling is started on the first call), its
 * 1920x1088 report scaled to the screen.  Host twin: whatever the test's
 * callback says.  Returns out->down.  Cheap: call it once per frame.
 *
 *   x, y    LOGICAL 480x272, the coordinates psp/ui_psp.c lays out in
 *   nx, ny  native 960x544, for a main loop that hit-tests picture
 *           rectangles (vid_pair_rects, vid_picture_rect)
 *
 * The UI's own use is vita/ui_vita_touch.h; a main loop that wants taps on
 * the GAME picture (the local link's "tap a machine to drive it") reads
 * this directly while the menu is closed. */
typedef struct
{
   int down;
   int x, y;
   int nx, ny;
} pc_touch;
int pspcompat_touch(pc_touch *out);

/* ---- text entry: the system keyboard ---------------------------------------
 * ONE text field at a time, never blocking:
 *
 *   pspcompat_ime_open(title, initial, max_chars)   0 = the keyboard is
 *        coming up; < 0 = it cannot (already open, or the system refused):
 *        the caller keeps the old text
 *   every frame after that, pspcompat_ime_poll(out, cap):
 *        PC_IME_RUNNING     still typing: keep drawing frames
 *        PC_IME_DONE        `out` holds what was entered (UTF-8 reduced to
 *                           printable ASCII: the UI's faces have no other
 *                           glyphs; anything else becomes '?'), once
 *        PC_IME_CANCELLED   closed without accepting, once
 *        PC_IME_IDLE        nothing is open
 *
 * Vita: sceImeDialog (a system common dialog: it draws itself into the
 * frame the application presents, so the caller must have
 * vid_set_common_dialog(1) in force while this returns RUNNING and keep
 * presenting frames -- psp/ui_psp.c's ui_text_* does both).  Host twin: the
 * test's callback supplies the text after a few frames.  max_chars <= 63. */
enum { PC_IME_IDLE = 0, PC_IME_RUNNING, PC_IME_DONE, PC_IME_CANCELLED };
#define PC_IME_MAX_CHARS 63
int pspcompat_ime_open(const char *title, const char *initial, int max_chars);
/* The same for a password: the field shows dots, starts empty, and what is
 * typed is not kept by this file past the poll that returns it. */
int pspcompat_ime_open_secret(const char *title, int max_chars);
int pspcompat_ime_poll(char *out, size_t cap);

/* What every system dialog needs once per process (sceAppUtilInit and
 * sceCommonDialogSetConfigParam).  Idempotent; pspcompat_ime_open and
 * pspcompat_user_name call it themselves, and a main loop that shows the ad
 * hoc NetCheck dialog can call it at boot.  A no-op on the host. */
void pspcompat_dialogs_init(void);

/* The console owner's name (the Vita's "User Name" system setting), for the
 * first run's default player name.  "" when unknown (always on the host,
 * unless a test set one). */
void pspcompat_user_name(char *out, size_t cap);

/* ---- host twin only (POSIX build) ---------------------------------------
 * The clock is VIRTUAL so a run is reproducible: it advances 16667 us per
 * vblank wait and a few microseconds per reading.  The pad is whatever the
 * test's callback says. */
void pspcompat_host_pad(unsigned (*fn)(void));
void pspcompat_host_advance(unsigned us);
/* Called at every vblank wait, after the clock moves (a test's frame hook). */
void pspcompat_host_vblank_hook(void (*fn)(void));
/* The finger: fn returns 1 while it is down and fills NATIVE 960x544
 * coordinates.  NULL = no touch panel. */
void pspcompat_host_touch(int (*fn)(int *nx, int *ny));
/* The keyboard: fn is asked once per poll with the frames elapsed since the
 * open; it returns NULL while "typing", a string to accept it, or
 * PSPCOMPAT_HOST_IME_CANCEL to close without accepting.  NULL fn = every
 * open is refused (a console with no keyboard). */
extern const char PSPCOMPAT_HOST_IME_CANCEL[];
void pspcompat_host_ime(const char *(*fn)(const char *title,
                                          const char *initial, int frames));
void pspcompat_host_user_name(const char *name);

#ifndef PSPCOMPAT_IMPL
#define SceUID                         pc_uid
#define SceSize                        pc_size
#define ScePspDateTime                 pc_datetime
#define SceIoStat                      pc_stat
#define SceIoDirent                    pc_dirent
#define SceCtrlData                    pc_ctrl
#define sceIoOpen                      pc_io_open
#define sceIoClose                     pc_io_close
#define sceIoRead                      pc_io_read
#define sceIoWrite                     pc_io_write
#define sceIoLseek                     pc_io_lseek
#define sceIoLseek32                   pc_io_lseek32
#define sceIoRemove                    pc_io_remove
#define sceIoRename                    pc_io_rename
#define SceOff                         pc_off
/* Opt-in: the Vita SDK declares these two with other types, and a few
 * files that include this header also include <psp2/kernel/threadmgr.h>
 * (render_thread.c, upscale_vita.c).  The PSP code that calls them (Share
 * game data) defines PSPCOMPAT_KERNEL_CALLS first. */
#ifdef PSPCOMPAT_KERNEL_CALLS
#define sceKernelGetSystemTimeWide     pspcompat_time_wide_us
#define sceKernelDelayThread           pspcompat_delay_us
#endif
#define sceIoMkdir                     pc_io_mkdir
#define sceIoGetstat                   pc_io_getstat
#define sceIoDopen                     pc_io_dopen
#define sceIoDread                     pc_io_dread
#define sceIoDclose                    pc_io_dclose
#define sceCtrlPeekBufferPositive      pc_ctrl_peek
#define sceKernelGetSystemTimeLow      pspcompat_time_us
#define sceDisplayWaitVblankStart      pspcompat_wait_vblank
#define sceKernelDcacheWritebackRange(p, n) \
   vid_tex_invalidate((p), (size_t)(n))
#endif /* PSPCOMPAT_IMPL */

#ifdef __cplusplus
}
#endif

#endif /* PSPCOMPAT_H */
