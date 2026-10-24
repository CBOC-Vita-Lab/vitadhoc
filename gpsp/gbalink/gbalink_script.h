/* gbalink_script.h -- frame-stamped button scripts for the partner machine
 * of a GBA link bench (the blind subset of fe_autopilot's grammar: wait,
 * press, hold, repeat/endrepeat, dump, evt; '#'/';' comments).  Measurement
 * only: the desktop prototype (tools/gbamirror) and the PSP harness, where
 * fe_autopilot drives the local machine and this drives the partner. */
#ifndef GBALINK_SCRIPT_H
#define GBALINK_SCRIPT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gbalink_script
{
   uint16_t *keys;     /* GBA KEYINPUT bit order, 1 = pressed, per frame */
   uint8_t  *dump;     /* 1 = the script asked for a screen dump here    */
   char    **mark;     /* `evt` text at that frame, or NULL              */
   unsigned  n, cap;
} gbalink_script;

/* 0 on success.  `cap` frames at most. */
int  gbalink_script_load(gbalink_script *sc, const char *path, unsigned cap);
void gbalink_script_free(gbalink_script *sc);
static inline uint16_t gbalink_script_keys(const gbalink_script *sc,
                                           uint32_t frame)
{
   return frame < sc->n ? sc->keys[frame] : 0;
}

#ifdef __cplusplus
}
#endif
#endif
