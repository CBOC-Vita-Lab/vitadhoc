/* osd_psp.h — overlay/OSD framework (plan §8): toast queue (top-center),
 * persistent session chip (bottom-right) and FF chip (top-right).
 * osd_draw() renders one overlay pass; call once per presented frame,
 * after the game blit and before vid_swap(). All main-thread. */
#ifndef OSD_PSP_H
#define OSD_PSP_H

#ifdef __cplusplus
extern "C" {
#endif

void osd_toast(const char *fmt, ...);
void osd_chip_session(const char *text);  /* NULL/"" hides the chip */
void osd_chip_ff(const char *text);       /* NULL/"" hides the chip */
void osd_chip_fps(const char *text);      /* top-left; NULL/"" hides */
void osd_draw(void);
/* Forget every toast and chip: the in-process return to the game list
 * starts the next game with the OSD a fresh boot has. */
void osd_reset(void);
#ifdef VITA
/* Where a toast sits.  Over the game it is the PSP's place, a line below
 * the top edge.  The Vita also shows toasts while a menu or the game list
 * is up (the PSP queues them for the next game), and there that place is
 * the wordmark: psp/ui_psp.c moves them to the screen's very edge in the
 * menus and above the footer in the game list. */
enum { OSD_TOAST_GAME = 0, OSD_TOAST_MENU, OSD_TOAST_LIST };
void osd_toast_place(int where);
#endif

#ifdef __cplusplus
}
#endif

#endif /* OSD_PSP_H */
