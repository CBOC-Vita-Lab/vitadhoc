/* Steady pacing for the app's own consoles (GBA, GB, GBC, NES, WonderSwan):
 * playerkit's pk_pace decision (playerkit/src/pk_pace.h, MIT), the same one
 * the SNES player uses.  The game always runs at its true rate; when the
 * drawn frames cannot keep up, every second frame is drawn, on an exact
 * cadence, instead of irregular drops; every frame again only after about
 * 3 s with room to spare.  Settings > Gameplay > Steady pacing (on by
 * default). */
#ifndef GBADHOC_PACE_STEADY_H
#define GBADHOC_PACE_STEADY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void steady_config(uint32_t budget_us);   /* one refresh */
void steady_enable(int on);
void steady_reset(void);                 /* after the menu, a load, a pause */
/* one drawn frame's cost (its start to its picture handed over): returns
 * 1 while every second frame is to be drawn; *changed set on a change */
int steady_add(uint32_t cost_us, int *changed);
int steady_half(void);
#ifdef __cplusplus
}
#endif
#endif
