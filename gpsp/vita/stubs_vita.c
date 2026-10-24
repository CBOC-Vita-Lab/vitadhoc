/* stubs_vita.c -- the app without its link layer.
 *
 * main_vita.c calls the two link files (app_vita.h: part A, link_vita.c, the
 * network; part B, local_vita.c, both machines on this console) at every
 * point of the session where they have something to do.  This file is what
 * those calls reach in a build that has neither, or only one: a do-nothing
 * definition of each function, WEAK, so the moment a real file is on the
 * link line (app.mk: link.mk) its definitions win and nothing here or in
 * main_vita.c changes.  Weak one function at a time: a file that implements
 * only part of its part overrides only that.
 *
 * A stub answers the way "the feature is absent" should look to the player:
 * no session, nothing locked -- and an action that would start something
 * says so, in the place the menu looks for the outcome (the view's error
 * for Host and for the local link, whose pages stay open; a toast for Join,
 * whose page has closed).
 *
 * NOT here: the UI's hooks of ui_host_hooks.h sections 2-5 (Mystery Gift,
 * ui_host_net_status, ui_host_rooms_*, ui_host_local_state).  Their weak
 * defaults are ui_hooks_default.c's, and only there.  Cheats are real
 * (cheats_vita.c, cheats.mk).
 */
#include <stddef.h>
#include <stdint.h>

#include "app_vita.h"
#include "osd_psp.h"
#include "ui_psp.h"

#define STUB __attribute__((weak))

/* ---- part A: the network (link_vita.c) ------------------------------------ */

/* Host was asked for and there is nothing to host with: the waiting page
 * reads this from the view and puts it on its plate. */
static const char *g_net_error;

STUB void link_boot(void)                          { }
STUB void link_shutdown(void)                      { }
STUB void link_host_config(fe_host_config *cfg)    { (void)cfg; }
STUB void link_game_begin(void)                    { g_net_error = NULL; }
STUB void link_game_end(void)                      { }

STUB void link_action(int is_host)
{
   if (is_host)
      g_net_error = "Playing over the network is not in this build yet.";
   else
      osd_toast("Playing over the network is not in this build yet");
}

STUB void link_disconnect(void)                    { g_net_error = NULL; }
STUB void link_frame(void)                         { }
STUB int  link_draw(void)                          { return 0; }
STUB void link_idle_pump(void)                     { }
STUB void link_power(int resumed)                  { (void)resumed; }
STUB int  link_session_up(void)                    { return 0; }
STUB int  link_net_busy(void)                      { return 0; }
STUB int  link_locks_states(void)                  { return 0; }
STUB int  link_owns_picture(void)                  { return 0; }
STUB const char *link_session_info(void)           { return NULL; }

STUB void link_ui_view(ui_link_view *v)
{
   /* No session: state stays UI_LINK_OFF, as main_vita.c zeroed it. */
   v->error = g_net_error;
}

/* ---- part B: the local link (local_vita.c) --------------------------------- */

static const char *g_local_error;

STUB void link_local_game_begin(void)              { g_local_error = NULL; }
STUB void link_local_game_end(void)                { }

STUB void link_local_start(void)
{
   g_local_error = "Two players on one console is not in this build yet.";
}

STUB void link_local_stop(void)                    { g_local_error = NULL; }
STUB int  link_local_active(void)                  { return 0; }
STUB void link_local_frame(void)                   { }

STUB void link_local_pads(uint32_t keys1, uint32_t keys2, int have2)
{
   (void)keys1; (void)keys2; (void)have2;
}

STUB void link_local_present(const uint16_t *pix, unsigned w, unsigned h,
                             size_t pitch_bytes)
{
   (void)pix; (void)w; (void)h; (void)pitch_bytes;
}

STUB void link_local_next_view(void)               { }
STUB int  link_local_tap(int x, int y)             { (void)x; (void)y; return 0; }
STUB void link_local_state_saved(const char *f)    { (void)f; }
STUB void link_local_state_loaded(const char *f)   { (void)f; }
STUB int  link_local_audio(const int16_t *lr, size_t n)
{
   (void)lr; (void)n;
   return 0;
}
STUB void link_local_io(void)                      { }
STUB void link_local_power(int resumed)            { (void)resumed; }

STUB void link_local_ui_view(ui_link_view *v)
{
   if (g_local_error)
      v->error = g_local_error;
}
