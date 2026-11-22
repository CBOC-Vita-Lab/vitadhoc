/* test only: see wide_fuzz.cpp */
#ifndef WIDE_FUZZ_H
#define WIDE_FUZZ_H
void wide_fuzz_seed(unsigned seed);
void wide_fuzz_frame(int level);
void wide_fuzz_mid(int on);              /* also write in the middle of the frame, between lines (needs the CBOC_TEST_LINE_HOOK core: snes_wide_twin) */
unsigned long wide_fuzz_mid_calls(void); /* lines the hook was called for (0 = this build has no hook) */
unsigned long wide_fuzz_mid_writes(void);
#endif
