/* share_psp.c -- see share_psp.h and docs/SAVE-SHARE.md. */
#include "share_psp.h"

#include <stdio.h>
#include <string.h>

#ifdef VITA
/* THE VITA: the files and the build identity below are the PSP's, through
 * pspcompat; the radio (share_net_*, share_now_ms, the MAC in
 * share_ident) is vita/share_vita.c, on transport_lan. */
#define PSPCOMPAT_KERNEL_CALLS   /* Share game data's clock and sleeps */
#include "pspcompat.h"
/* savestate.h's sizes are in the PSP SDK's types (psptypes.h). */
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
#else
#include <pspkernel.h>
#include <pspiofilemgr.h>
#endif

#include "libretro.h"
#include "savestate.h"
#ifndef VITA
#include "transport_adhoc.h"
#endif
#include "fe_evt.h"

#ifndef VITA
void pwr_hold(const char *why);              /* main_psp.c */
void pwr_release(void);
#endif

/* The bulk lane's ring: the guest drains it every frame and the sharer
 * sends at most SHARE_BURST datagrams a frame, so 32 slots of 1.4 KiB
 * (46 KiB, allocated by the transport at open and freed at its term) is
 * four frames of slack. */
#define SHARE_BULK_SLOTS 32

#ifndef VITA
static int  g_up;
#endif
static char g_build_override[FE_SHARE_BUILD_CAP];
static unsigned g_fail_after;

#ifndef VITA
unsigned share_now_ms(void)
{
   return (unsigned)(sceKernelGetSystemTimeWide() / 1000);
}

int share_net_up(int host, const char *group)
{
   int rc;
   if (g_up)
      return 0;
   pwr_hold("share_net");
   rc = host ? adhoc_transport_init(group, 0)
             : adhoc_transport_init_join(group, 0, 15000u);
   if (rc == ADHOC_OK &&
       adhoc_transport_bulk_open(SHARE_BULK_SLOTS, FE_SHARE_MAGIC) != 0)
   {
      adhoc_transport_term();
      rc = ADHOC_ERR_NP_START;             /* "could not start": memory */
   }
   pwr_release();
   fe_evt("share_net_up host=%d group=%s rc=%d stage=%s", host, group, rc,
          adhoc_transport_stage());
   g_up = rc == ADHOC_OK;
   return rc;
}

void share_net_down(void)
{
   adhoc_stats st;
   if (!g_up)
      return;
   adhoc_transport_get_stats(&st);
   fe_evt("share_net_down bulk_tx=%u bulk_txfail=%u bulk_rx=%u bulk_drop=%u "
          "wouldblock=%u", st.bulk_tx, st.bulk_txfail, st.bulk_rx,
          st.bulk_drop, st.tx_would_block);
   pwr_hold("share_net");
   adhoc_transport_bulk_close();
   adhoc_transport_term();
   pwr_release();
   g_up = 0;
}

int share_net_is_up(void)
{
   return g_up;
}

int share_net_connected(void)
{
   return g_up && adhoc_transport_connected();
}

void share_net_send(void *user, const uint8_t mac[6], const void *buf,
                    size_t len)
{
   (void)user;
   if (g_up)
      adhoc_transport_bulk_send(mac, buf, len);
}

int share_net_recv(void *buf, size_t cap)
{
   return g_up ? adhoc_transport_bulk_recv(buf, cap) : 0;
}
#endif /* !VITA */

void share_build_override(const char *build)
{
   snprintf(g_build_override, sizeof(g_build_override), "%s",
            build ? build : "");
}

/* fe_share_state_check knows the GBA state's size and magic by value (it is
 * built without the core's headers): they must be the core's. */
typedef char share_gba_state_size_is_the_cores[
   FE_SHARE_GBA_STATE_SIZE == GBA_STATE_MEM_SIZE ? 1 : -1];
typedef char share_gba_state_magic_is_the_cores[
   FE_SHARE_GBA_STATE_MAGIC == GBA_STATE_MAGIC ? 1 : -1];

void share_ident(fe_share_ident *out)
{
   struct retro_system_info info;
   memset(out, 0, sizeof(*out));
   memset(&info, 0, sizeof(info));
   retro_get_system_info(&info);
   snprintf(out->build, sizeof(out->build), "%s",
            g_build_override[0] ? g_build_override :
            info.library_version ? info.library_version : "unknown");
   /* The state formats.  GBA: the savestate's own version word.  GB/GBC:
    * the TGB Dual state has no version field of its own, so its format is
    * named by a number bumped here whenever fe_host's GB state layout
    * changes -- and by the build string, which FE_SHARE_STATES_NEED_SAME_
    * BUILD compares as well. */
   out->state_fmt[0] = GBA_STATE_VERSION;
   out->state_fmt[1] = 1;
   out->state_fmt[2] = 1;
#ifdef VITA
   share_net_local_mac(out->mac);
#else
   if (g_up)
   {
      nd_transport t;
      adhoc_transport_iface(&t);
      t.local_addr(t.ctx, out->mac);
   }
#endif
}

/* ---- the stick ------------------------------------------------------------ */

static int fs_exists(void *u, const char *path)
{
   SceIoStat st;
   (void)u;
   return sceIoGetstat(path, &st) >= 0;
}

static int fs_rename(void *u, const char *from, const char *to)
{
   int rc;
   SceIoStat st;
   (void)u;
   rc = sceIoRename(from, to);
   FE_EVT_ONLY(rc);
   /* Judged by the result, not the return code (PPSSPP's and the
    * firmware's differ on a rename; state_delete has the same rule). */
   if (sceIoGetstat(from, &st) >= 0 || sceIoGetstat(to, &st) < 0)
   {
      fe_evt("share_rename FAILED rc=0x%08X from=%s", (unsigned)rc, from);
      return -1;
   }
   return 0;
}

static int fs_remove(void *u, const char *path)
{
   (void)u;
   return sceIoRemove(path) < 0 ? -1 : 0;
}

const fe_share_fs share_fs = { NULL, fs_exists, fs_rename, fs_remove };

void share_fail_write_after(unsigned bytes)
{
   g_fail_after = bytes;
}

int share_file_create(share_file *f, const char *path)
{
   f->written = 0;
   f->len = 0;
   f->fail_after = g_fail_after;
   f->fd = sceIoOpen(path, PSP_O_RDWR | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   return f->fd >= 0 ? 0 : -1;
}

void share_file_close(share_file *f)
{
   if (f->fd >= 0)
      sceIoClose(f->fd);
   f->fd = -1;
}

int share_file_write(void *user, uint32_t off, const void *buf, uint32_t len)
{
   share_file *f = (share_file *)user;
   if (f->fd < 0)
      return -1;
   f->written += len;
   if (f->fail_after && f->written > f->fail_after)
      return -1;
   /* A chunk past the end (an earlier one was lost and is still to come):
    * the gap is written as zeros first, rather than trusting a seek past
    * the end of a FAT file to extend it. */
   if (off > f->len)
   {
      static const unsigned char zero[256];
      if (sceIoLseek32(f->fd, (int)f->len, PSP_SEEK_SET) != (int)f->len)
         return -1;
      while (f->len < off)
      {
         unsigned n = off - f->len < sizeof(zero) ? off - f->len
                                                  : (unsigned)sizeof(zero);
         if (sceIoWrite(f->fd, zero, n) != (int)n)
            return -1;
         f->len += n;
      }
   }
   if (sceIoLseek32(f->fd, (int)off, PSP_SEEK_SET) != (int)off)
      return -1;
   if (sceIoWrite(f->fd, buf, len) != (int)len)
      return -1;
   if (off + len > f->len)
      f->len = off + len;
   return 0;
}

int share_file_read(void *user, uint32_t off, void *buf, uint32_t len)
{
   share_file *f = (share_file *)user;
   if (f->fd < 0 || sceIoLseek32(f->fd, (int)off, PSP_SEEK_SET) != (int)off)
      return -1;
   return sceIoRead(f->fd, buf, len);
}
