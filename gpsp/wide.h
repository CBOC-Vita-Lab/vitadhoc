/* wide.h -- the wide picture's switch and profiles (wide.c, video.h). */
#ifndef GBADHOC_WIDE_H
#define GBADHOC_WIDE_H

#ifdef __cplusplus
extern "C" {
#endif

/* (EMERALD: the profile of the five GBA Pokemon games' shared engine) */
enum { WIDE_PROFILE_NONE = 0, WIDE_PROFILE_EMERALD, WIDE_PROFILE_TEST,
       WIDE_PROFILE_AUTO };

/* The front end's switch:
 *   WIDE_OFF       the console's 240 pixels;
 *   WIDE_KNOWN     wide in a game that has a profile, 240 otherwise;
 *   WIDE_ALL       and in any other game: rotated/scaled backgrounds go on
 *                  past the edge by themselves, the rest of a margin is
 *                  black;
 *   WIDE_AUTO      and, in a game without a profile, whatever the game has
 *                  already drawn of the places beside the screen (wide.c,
 *                  "Any game"); what it has not is left empty;
 *   WIDE_TEST      (host builds) a profile that hands the renderer video
 *                  memory's own map rows: the strips must come out as the
 *                  renderer draws them unaided.
 * While it is wide the core hands over a picture 240 + 2 * wide_margin_px()
 * pixels wide. */
enum { WIDE_OFF = 0, WIDE_KNOWN, WIDE_ALL, WIDE_AUTO, WIDE_TEST };
void wide_set(int mode);
int  wide_margin_px(void);
/* What a margin shows where the game has nothing for it (video.h's
 * WIDE_FILL_*: 0 bars, 1 the picture's edge continued softly, 2 the picture
 * stretched). */
void wide_set_fill(int fill);

/* The profile of the loaded game: WIDE_PROFILE_NONE means only what goes on
 * by itself is shown in the margins (rotated/scaled backgrounds). */
int  wide_profile_id(void);
/* Tests (vita.ini wide + 32): 0 = no profile is used, every game is run by
 * the automatic mode -- a game with a profile then judges it. */
void wide_set_profiles(int on);
const char *wide_profile_name(void);   /* "Pokemon Emerald", "automatic", "" */

/* The core's own calls: a game was loaded; vertical blank begins; a state
 * was loaded. */
void wide_game_loaded(void);
void wide_vblank(void);
void wide_state_loaded(void);

/* Since the last call. */
typedef struct wide_stats_t
{
   unsigned latched;      /* frames a profile took its snapshot            */
   unsigned refused;      /* ...of which the picture did not bear it out   */
   unsigned held;         /* ...where the snapshot before did (a slow frame) */
   unsigned checked;      /* map entries on screen compared with it        */
   unsigned mismatched;   /* ...that differed                              */
   unsigned worst;        /* most in one frame                             */
   /* WIDE_AUTO: margin tiles (sampled) that were known / were not; scene
    * changes found; and, where a test has a profile to compare with, how
    * many of the known ones it agrees with / does not. */
   unsigned auto_known, auto_unknown, cuts, auto_right, auto_wrong;
   /* ...looks ahead taken (the scout), and map entries they added. */
   unsigned scouts, scout_learned;
} wide_stats_t;
void wide_stats(wide_stats_t *out);

/* THE SCOUT (wide.c): the app's second machine, a copy of the first, runs
 * a second ahead with LEFT or RIGHT held, and what it shows of the world is
 * added to what the first remembers.  frontend-common/fe_gbadual.c drives
 * it; each machine is this code, so each has all of these:
 *   on the machine that is played -- wanted: -1 left, 1 right, 0 no;
 *     export: its trackers and caches for the copy to carry on from; epoch:
 *     changes when its scene does (a look ahead under way is then void);
 *     report: how a look ahead went;
 *   on the copy -- begin / end; status: 1 when its future left the scene,
 *     with how far its picture has moved (signed) and what it has added. */
typedef struct wide_scout_bg
{
   unsigned char  on;
   unsigned short cnt, h, v;
   unsigned       wt, ht;
   int            cam_x, cam_y, start_x, start_y;
   void          *cache;
} wide_scout_bg;
typedef struct wide_scout_seed
{
   wide_scout_bg bg[4];
   unsigned      main_bg;        /* the layer whose scroll is "the camera" */
} wide_scout_seed;

int      wide_scout_wanted(void);
int      wide_scout_vdir(void);  /* the picture moves up (-1) / down (1)   */
int      wide_scout_hdir(void);  /* ...left (-1) / right (1)                */
unsigned wide_scout_gap(int side);   /* what that side lacks on screen */
int      wide_scout_cam(void);   /* that layer's unwrapped x, in pixels    */
int      wide_scout_cam_y(void);
int      wide_scout_moved_y(void);   /* (the copy) as status's moved_px    */
unsigned wide_scout_epoch(void);
void     wide_scout_export(wide_scout_seed *seed);
void     wide_scout_report(int dir, int moved_px, unsigned learned);
void     wide_scout_begin(const wide_scout_seed *seed);
void     wide_scout_end(void);
int      wide_scout_status(int *moved_px, unsigned *learned, unsigned *frames);

/* A test (video.h's wide_selftest): on, and its counts since the last call. */
void wide_selftest_set(int on);
void wide_selftest_get(unsigned *pixels, unsigned *bad);

#ifdef __cplusplus
}
#endif

#endif
