/* vita_linkcheck.c -- a throwaway PS Vita program whose only job is to LINK.
 *
 * `make vita-linkcheck` builds the whole Vita UI (vita/ui.mk: the PSP UI
 * files -DVITA, video_vita.c, the GXM backend, libvita2d's core, pspcompat's
 * native body) with arm-vita-eabi-gcc and links it with this file into an
 * ELF, a .velf and a .self, proving every symbol resolves against VitaSDK.
 * It has never been run: it stands in for main_vita.c (which another
 * workstream owns) only as far as the linker is concerned, with the hooks of
 * vita/ui_host_hooks.h stubbed and a main() that reaches every entry point.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>

#define PSPCOMPAT_IMPL
#include "pspcompat.h"
#include "ui_psp.h"
#include "osd_psp.h"
#include "config_psp.h"
#include "video_vita.h"
#include "ui_host_hooks.h"
#include "fe_evt.h"

volatile int g_running = 1;
void osd_session_chip_refresh(void) { }
/* Every other hook of ui_host_hooks.h -- Mystery Gift, the network's state,
 * the rooms, the local link, and the cheat engine's thirteen functions --
 * is deliberately NOT defined here: the link resolves them to the weak
 * defaults in vita/ui_hooks_default.c, which is exactly how the app links
 * before link_vita.c, local_vita.c and cheats_vita.c exist. */
int fe_host_gb_palette_count(void) { return 1; }
const char *fe_host_gb_palette_name(int id) { (void)id; return "Auto"; }
void fe_host_gb_palette_set(int id) { (void)id; }
/* The core's half of the pixel-format link guard (gba_memory.c). */
const char gpsp_core_pixfmt_rgb565 = 1;

int main(void)
{
   static uint16_t frame[240 * 160], snap[256 * 256];
   char rom[512], cfg[192], roms[192];
   int slot = 0;
   fe_console_t con = FE_CONSOLE_GBA;
   ui_linking_view lv;

   pspcompat_set_root(PSPCOMPAT_ROOT_VITA);
   pspcompat_make_dirs();
   pspcompat_path(cfg, sizeof(cfg), "config.ini");
   pspcompat_path(roms, sizeof(roms), "roms");
   fe_evt_init(NULL, 0);
   pcfg_load(cfg);
   vid_init();
   vid_set_mode(g_pcfg.scale, g_pcfg.filter);
   vid_ambient_mode(g_pcfg.ambient);

   if (ui_browser(roms, rom, sizeof(rom), &slot, &con) == 0)
   {
      ui_loading_update("Reading ROM", 1, 2);
      ui_loading_finish(1);
   }
   while (g_running)
   {
      unsigned pad = pspcompat_pad();
      vid_draw_frame(frame, 240, 160, 480);
      vid_draw_frame_pair(frame, frame, 240, 160, 480, VID_PAIR_SIDE);
      if ((pad & (PSP_CTRL_START | PSP_CTRL_SELECT)) ==
          (PSP_CTRL_START | PSP_CTRL_SELECT) && !ui_active())
      {
         vid_frame_to_psp565(snap, 256, frame, 240, 160, 480);
         ui_set_backdrop(snap, 240, 160);
         ui_open();
      }
      if (ui_active())
      {
         /* The Vita's own actions, so their enumerators are referenced. */
         switch (ui_frame(pad, 0, NULL))
         {
         case UI_ACT_EXIT:           g_running = 0; break;
         case UI_ACT_NET_JOIN:       (void)ui_join_target(); break;
         case UI_ACT_NET_LOCAL:
         case UI_ACT_NET_LOCAL_STOP:
         case UI_ACT_NET_LOCAL_SWAP:
         default:                    break;
         }
      }
      {
         /* The front panel while the menu is closed (a main loop's use). */
         pc_touch t;
         if (!ui_active() && !ui_typing() && pspcompat_touch(&t))
            vid_dump_ge("ux0:data/GBAdhoc/log/touch.png");
      }
      memset(&lv, 0, sizeof(lv));
      if (pad & PSP_CTRL_LTRIGGER)
         ui_linking_draw(&lv);
      if (pad & PSP_CTRL_RTRIGGER)
         ui_wake_menu(snap, 240, 160, rom);
      osd_draw();
      pspcompat_wait_vblank();
      vid_swap();
      if (pad & PSP_CTRL_TRIANGLE)
         vid_dump_ge("ux0:data/GBAdhoc/log/frame.png");
   }
   pcfg_save();
   vid_term();
   sceKernelExitProcess(0);
   return 0;
}
