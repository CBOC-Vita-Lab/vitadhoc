/* Bounded local menu work. CPU pixels only; the UI owns every GPU object. */
#ifndef GBADHOC_MENU_LOAD_H
#define GBADHOC_MENU_LOAD_H

#include <stddef.h>
#include <stdint.h>

#define ML_PATH 512
#define ML_STEM 256
#define ML_NEIGHBORS 6
#define ML_META_SLOTS 8
#define ML_IMAGE_SLOTS 2
#define ML_SLOTS (ML_META_SLOTS + ML_IMAGE_SLOTS)
#define ML_TILE_EDGE 160
#define ML_IMAGE_MAX_W 960
#define ML_IMAGE_MAX_H 544

enum { ML_META = 1, ML_TILE = 2, ML_STILL = 4, ML_STATE0 = 8 };
/* ML_ERROR: a failure that may pass (I/O, memory): retried soon.
 * ML_INVALID: the file is there but its content can never be decoded
 * (bad header, corrupt or oversized image): retried only after a long
 * wait or when new artwork arrives. */
enum { ML_OK = 1, ML_ABSENT = -1, ML_ERROR = -2, ML_CANCELLED = -3,
       ML_INVALID = -4 };
enum { ML_UNKNOWN = -1, ML_MISSING = 0, ML_PRESENT = 1, ML_OLD_ONLY = 2 };
enum { ML_SOURCE_NORMAL = 0, ML_SOURCE_HERO = 1 };

typedef struct menu_load_job
{
   /* epoch: the game's save-state key (metadata, shelf shots).  art_epoch:
    * its artwork key (tile, still), advanced only between batches. */
   uint32_t generation, focus, epoch, art_epoch;
   int console;
   unsigned tasks;
   char rom[ML_PATH], root[ML_PATH], stem[ML_STEM], state1[ML_PATH];
} menu_load_job;

typedef struct menu_load_meta
{
   int battery, state[5];
   char when[5][24];
   char blurb[400];
   /* where the game was left (state_slots.h psp_state_resume_path): ML_*,
    * and when; and a patch beside the ROM (rom_patch.h ROM_PATCH_*) */
   int resume;
   char resume_when[24];
   int patch;
} menu_load_meta;

typedef struct menu_load_result
{
   menu_load_job job;
   unsigned kind, slot, source;
   int status;
   menu_load_meta meta;
   uint32_t *pixels;
   unsigned w, h;
   uint64_t finished_us;
} menu_load_result;

/* Called only by the dedicated worker. Check cancelled between I/O steps.
 * A successful image transfers malloc-owned pixels to the result slot. */
typedef int (*menu_load_fn)(const menu_load_job *, unsigned kind,
                            menu_load_result *, int (*cancelled)(void));

int menu_load_start(menu_load_fn load);
/* Latest selected snapshot replaces pending work; jobs[0] is selected,
 * following jobs are distance-ordered visible neighbors. Copies all paths.
 * Nonblocking: no worker join, filesystem call or result wait. */
int menu_load_submit(const menu_load_job *jobs, unsigned count);
void menu_load_pause(void);
void menu_load_resume(void);
/* Metadata and images have separate bounded slots. A successful take pins
 * the result until release; pixels/identity cannot change while pinned. */
int menu_load_take(int image, menu_load_result *out);
void menu_load_release(unsigned slot);
/* Only at process exit. A timeout retains thread, buffers and processor. */
int menu_load_shutdown(uint32_t timeout_us);

typedef struct menu_load_diag
{
   unsigned started, paused, phase, kind, ready, pinned;
   unsigned submitted, published, stale, errors, dropped, job_us, wait_us;
} menu_load_diag;
void menu_load_get_diag(menu_load_diag *out);

/* Pure CPU/read-only implementation, shared by the native app and tests. */
int menu_files_load(const menu_load_job *job, unsigned kind,
                     menu_load_result *out, int (*cancelled)(void));

#endif
