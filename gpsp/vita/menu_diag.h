#ifndef GBADHOC_MENU_DIAG_H
#define GBADHOC_MENU_DIAG_H

/* Main-thread-only counters. Nested stages overlap; they are not additive. */
enum md_stage {
   MD_STILL, MD_HERO, MD_THUMB, MD_PREFETCH, MD_ART_READ, MD_DECODE,
   MD_RESAMPLE, MD_CACHE_WRITE, MD_UPLOAD, MD_DRAW, MD_VBLANK,
   MD_PRESENT, MD_SWAP, MD_DEFER, MD_PROBE, MD_PRE_DRAW, MD_META,
   MD_DESCRIPTION, MD_PUBLISH, MD_STAGE_COUNT
};
void md_start(void);
void md_stop(void);
void md_pause(void);
void md_resume(void);
int md_active(void);
unsigned long long md_begin(void);
void md_end(enum md_stage stage, unsigned long long began);
void md_frame_begin(int rom, int console, int input_active);
void md_selection(int rom, int console);
void md_input(int active);
void md_frame_end(void);
/* Periodic platform snapshot is due once per five seconds, never catch-up. */
int md_sample_due(void);

#endif
