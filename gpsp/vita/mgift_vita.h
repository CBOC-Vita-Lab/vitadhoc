/* mgift_vita.h -- Mystery Gift on the Vita (vita/mgift_vita.c). */
#ifndef MGIFT_VITA_H
#define MGIFT_VITA_H

#ifdef __cplusplus
extern "C" {
#endif

/* The running GBA cartridge's 4-character header code at boot ("BPEE" for
 * Emerald), or NULL when no GBA game runs: the menu offers Mystery Gift
 * only for a GBA game. */
void mgift_vita_game(const char *code4);
/* vita.ini mgift_port: the UDP port listened on (tests; default 21064). */
void mgift_vita_set_port(unsigned port);
/* The menu's Mystery Gift row, or the shortcut: start / stop listening.
 * quiet = no toast (leaving the game). */
void mgift_vita_start(void);
void mgift_vita_stop(int quiet);
/* Once per frame while a GBA game runs: the phone's parcel, the cart. */
void mgift_vita_frame(void);
int  mgift_vita_active(void);

#ifdef __cplusplus
}
#endif

#endif /* MGIFT_VITA_H */
