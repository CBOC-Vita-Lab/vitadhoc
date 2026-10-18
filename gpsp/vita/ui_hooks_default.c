/* ui_hooks_default.c -- "not available" answers for the hooks the Vita UI
 * added (vita/ui_host_hooks.h sections 2..6), all WEAK.
 *
 * The UI's pages are written against hooks that vita/link_vita.c,
 * vita/local_vita.c and vita/cheats_vita.c implement.  Until each of those
 * is in the build, the application links against these instead and the
 * pages say what they say on a console with no network: "Not connected to
 * Wi-Fi", an empty room list, "not for this game", "No cheats for this game
 * yet".  A real definition anywhere in the link replaces the one here; no
 * flag, no #ifdef.
 *
 * Listed in vita/ui.mk (VITA_UI_SRCS_C).
 */
#include <stddef.h>
#include <string.h>

#include "fe_console.h"
#include "ui_host_hooks.h"
#include "cheats_vita.h"
#include "config_psp.h"
#include "video_vita.h"

#define UI_WEAK __attribute__((weak))

/* ---- section 2: the look ------------------------------------------------- */
UI_WEAK void ui_host_look_changed(void)
{
   vid_set_look(pcfg_display_console() == FE_CONSOLE_GBA ? g_pcfg.color
                                                          : VID_COLOR_RAW,
                g_pcfg.blend);
}

/* ---- section 2: ROM folders beside roms/ ---------------------------------- */
UI_WEAK int         ui_host_extra_roots(void)   { return 0; }
UI_WEAK const char *ui_host_extra_root(int i)   { (void)i; return ""; }

/* ---- section 2: Mystery Gift -------------------------------------------- */
UI_WEAK int         ui_host_mgift_available(void) { return 0; }
UI_WEAK int         mgift_ui_active(void)         { return 0; }
UI_WEAK const char *mgift_ui_line1(void)          { return ""; }
UI_WEAK const char *mgift_ui_line2(void)          { return ""; }

/* ---- section 3: the network's state ---------------------------------------- */
UI_WEAK void ui_host_net_status(ui_net_status *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   out->wifi = UI_WIFI_OFF;
   out->signal_pct = -1;
}

/* ---- section 4: rooms -------------------------------------------------------- */
UI_WEAK int ui_host_rooms_begin(const char *addr)
{
   (void)addr;
   return UI_ROOMS_E_NO_WIFI;
}

UI_WEAK int ui_host_rooms_poll(ui_room *out, int max)
{
   (void)out; (void)max;
   return UI_ROOMS_E_FAILED;
}

UI_WEAK void ui_host_rooms_end(void) { }

/* ---- section 5: the local link ------------------------------------------- */
UI_WEAK void ui_host_local_state(ui_local_state *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   out->why_not = "Two players on one console is not in this build yet.";
}

UI_WEAK int         ui_host_local_roms(void)            { return 0; }
UI_WEAK const char *ui_host_local_rom_name(int i)       { (void)i; return ""; }
UI_WEAK int         ui_host_local_rom_pick(int i)       { (void)i; return -1; }

/* ---- section 6: cheats (vita/cheats_vita.h) -- the list is empty and
 * nothing can be added, so the page shows its empty state. ---------------- */
UI_WEAK int  cheats_count(void)                        { return 0; }
UI_WEAK const cheat_entry *cheats_get(int i)           { (void)i; return NULL; }
UI_WEAK int  cheats_enabled_count(void)                { return 0; }
UI_WEAK int  cheats_set_enabled(int i, int on)
{
   (void)i; (void)on;
   return CHEATS_E_RANGE;
}
UI_WEAK int  cheats_validate(const char *code, fe_console_t console)
{
   (void)code; (void)console;
   return CHEATS_E_UNSUPPORTED;
}
UI_WEAK int  cheats_add(const char *name, const char *code)
{
   (void)name; (void)code;
   return CHEATS_E_UNSUPPORTED;
}
UI_WEAK int  cheats_remove(int i)                      { (void)i; return CHEATS_E_RANGE; }
UI_WEAK int  cheats_rename(int i, const char *name)
{
   (void)i; (void)name;
   return CHEATS_E_RANGE;
}
UI_WEAK int  cheats_suspended(void)                    { return 0; }
UI_WEAK const char *cheats_db_game(void)               { return ""; }
UI_WEAK const char *cheats_error_text(int rc)
{
   (void)rc;
   return "Cheats are not in this build yet.";
}
UI_WEAK const char *cheats_kind_name(int kind)         { (void)kind; return "code"; }
UI_WEAK const char *cheats_format_hint(fe_console_t console)
{
   (void)console;
   return "hexadecimal digits";
}
UI_WEAK const char *cheats_note(int i)                 { (void)i; return ""; }
UI_WEAK int  cheats_db_match(void)                     { return 0; }
