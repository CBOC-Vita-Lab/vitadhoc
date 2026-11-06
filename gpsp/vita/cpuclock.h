/* cpuclock.h -- the CPU speed setting (Settings > CPU speed): 500 MHz when
 * the console allows it, 444 MHz otherwise.  Portable C over plat.h; the
 * twin runs the same code against stand-in folders (plat_host.c).
 *
 * 500 MHz needs an overclock plugin.  Two kinds exist: one lets the app ask
 * (scePowerSetArmClockFrequency(500) is then accepted), PSVshellPlus ignores
 * the ask and applies a per-title profile file instead
 * (ur0:data/PSVshell/profiles/<TITLEID>, docs/TECHNICAL.md).  This module
 * does both: it asks, and where PSVshellPlus is installed it writes the
 * profile -- only when none exists, or when the player switches the setting
 * in our menu (their own file is then backed up, and put back on switching
 * to 444). */
#ifndef CPUCLOCK_H
#define CPUCLOCK_H

/* Once, after the log is open: root = the app's data folder (the backup and
 * the stamp live there), fast = the saved setting. */
void cpuclk_boot(const char *root, int fast);
int  cpuclk_fast(void);
/* The menu's change.  Requests the clock, writes or removes our profile. */
void cpuclk_set(int fast);
/* Called from menus (and a quiet game moment): the second measurement,
 * about 8 s after the start. */
void cpuclk_tick(void);
/* The text under the row. */
const char *cpuclk_note(void);

/* The profile for the title: 28 bytes, little-endian (also for the tests). */
#define CPUCLK_PROFILE_SIZE 28
void cpuclk_profile_bytes(unsigned char out[CPUCLK_PROFILE_SIZE]);

#endif
