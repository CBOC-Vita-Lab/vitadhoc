/* link_snes.h -- forming a two-player SNES link in the app, for snes.bin to play (2.0, package B3).
 *
 * SNES games are not run by this program (snes.bin is another one: Snes9x's licence), and sockets do not survive the
 * change from one program to the other.  So the app forms the link exactly as it does for a NES game -- the same
 * rooms, the same room code, the punch or the relay, the same "Two players" kind and "pad v1" compat -- but with no game
 * running: link_vita.c stops where the session would begin, and what snes.bin needs to make the same link again on the
 * SAME local port comes out here, to be written into launch/snes.ini as link.* keys (playerkit/FORMATS.md).
 *
 *   Same Wi-Fi   the host's snes.bin listens on the same port; the guest's joins the host's address.
 *   Online       a direct path: both punch again with the same key; a relayed one: the app lets go of its TURN
 *                allocation WITHOUT releasing it and snes.bin adopts it (lan_transport_relay_adopt), no new Allocate.
 *   Nearby       (ad hoc) is not offered for SNES in 2.0: the system's connection does not outlive the program change.
 *
 * Plain C.  Used by main_vita.c's SNES launch (a modal loop) and implemented at the end of link_vita.c. */
#ifndef LINK_SNES_H
#define LINK_SNES_H

#ifdef __cplusplus
extern "C" {
#endif

/* The launch file's link.* values (text, ready to write). */
typedef struct link_snes_link
{
   char     role[8];                 /* "host" | "guest" */
   char     mode[8];                 /* "lan" | "punch" | "relay" */
   unsigned local_port;
   char     peer[32], peer_lan[32];  /* "ip:port" */
   char     punch_key[20];
   char     relay_server[24], relay_ports[8], relay_user[170], relay_pass[170], relay_peer[32], relay_addr[32];
   unsigned delay;                   /* the host's input delay, in frames */
   char     compat[12];              /* 8 hex digits: the "pad v1" word both consoles must share */
   char     nick[20];
   unsigned wait_s;                  /* how long snes.bin waits for the other console to arrive */
} link_snes_link;

enum { LSN_IDLE = 0, LSN_WORKING, LSN_READY, LSN_FAILED, LSN_CANCELLED };

/* Start forming the link for the SNES game `rom_path` (`title`: its name for the room).  is_host 0: the room to
 * join is *ui_join_target() (a listed room, a typed address or an Online code), as for any game.  0 = begun;
 * else a UI_FAIL_* code (nothing was started: say it). */
int  link_snes_begin(int is_host, const char *rom_path, const char *title);
/* Once per frame while it works; `pad` is the controller's edges (O cancels).  LSN_*. */
int  link_snes_frame(unsigned pad);
/* LSN_READY: fill the values for the launch file and let the network go (the relay allocation is kept for snes.bin).
 * 0 = filled. */
int  link_snes_take(link_snes_link *out);
/* The status page (the LINKING screen), once per frame while LSN_WORKING / LSN_FAILED; the caller presents it. */
void link_snes_draw(void);
/* Give everything back (a cancel, a failure, the end of the modal). */
void link_snes_end(void);
/* LSN_FAILED: the UI_FAIL_* code (its sentences: ui_link_fail_text). */
int  link_snes_fail_code(void);
/* LSN_CANCELLED: 1 when the end was already told in a toast (a host's code expired), so the caller says nothing more. */
int  link_snes_end_said(void);
/* The game a SNES library file is, as a room names it: 8 upper-case hex digits (0 = unreadable). */
int  link_snes_game_id(const char *rom_path, char out[9]);
/* The way a session ended, from the report snes.bin left (launch/session.txt: link_result, link_error,
 * link_error_name, link_frames) and the launch file (link.role, link.mode): the toast the app says, or a UI_FAIL_*
 * code for a failure that has its own two lines (the joiner's, as link_end does).  Returns the UI code or 0. */
int  link_snes_ended(int was_host, int online, const char *result, int error, const char *name, int frames,
                     char *toast, unsigned toast_cap);

#ifdef __cplusplus
}
#endif

#endif /* LINK_SNES_H */
