/* The look governor (1.4): playerkit's pk_look (MIT; playerkit/INTEGRATION.md
 * section 8, docs/DEFAULTS-1.4.md).  Every game starts with the best look;
 * when THIS game cannot hold a locked 60 with it, the least visible
 * sacrifice is made first (the ladder below), remembered for the game, and
 * given back when there is room.  Only rows on Auto / at their default are
 * governed; a row set by hand is never touched.
 *
 * The ladder (least visible first):
 *   0 ambient glow: blurred game frame -> still art      (LOOK_ROW_AMBIENT)
 *   1 xBRZ 4x -> 3x                                      (LOOK_ROW_QUALITY)
 *   2 xBRZ 3x -> 2x                                      (LOOK_ROW_QUALITY)
 *   3 frame blend off                                     (LOOK_ROW_BLEND)
 *   4 xBRZ 2x -> sharp bilinear                           (LOOK_ROW_QUALITY)
 *   5 wide view off (GBA, NES)                            (LOOK_ROW_WIDE)
 * Memory: <root>/look/<console>/<xx>/<name>.look (playerkit/FORMATS.md). */
#ifndef GBADHOC_LOOK_GLUE_H
#define GBADHOC_LOOK_GLUE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { LOOK_ROW_AMBIENT = 0, LOOK_ROW_QUALITY, LOOK_ROW_BLEND, LOOK_ROW_WIDE,
       LOOK_ROWS };
enum { LOOK_AT_START = 0, LOOK_AT_SCENE, LOOK_AT_BLANK, LOOK_AT_MENU };

/* A game starts: its memory file (may not exist), the CPU clock, the rows
 * set by hand (bit = 1 << LOOK_ROW_*).  The remembered look is applied:
 * look_generation() changes. */
void look_begin(const char *mem_path, int mhz, unsigned user_rows);
/* The game ends (and the menu opens): the memory is written or deleted as
 * the governor says (a safe write: <path>.tmp, then renamed).  Returns
 * the action taken (0 none, 1 written, 2 deleted). */
int  look_save(void);
void look_end(void);
int  look_running(void);
void look_set_user_rows(unsigned user_rows);
/* One second of play; costs are fractions of a refresh.  1 = the look
 * changed (apply it). */
int  look_second(float emu, float draw, float filter, int refreshes,
                 int missed, int parallel, int pace_half);
int  look_safe_point(int kind);
void look_reset_window(void);
uint32_t look_generation(void);
uint32_t look_mask(void);
int  look_steps_on_row(int row);
/* the governed values: the largest xBRZ factor (0 = no limit, 1 = none) */
int  look_quality_cap(void);
int  look_rung(int i);              /* rung i applied */
/* "look: ..." once a second (after look_second); 1 = written */
int  look_census_line(char *buf, size_t n);
/* the end-of-game summary line */
void look_summary(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
#endif
