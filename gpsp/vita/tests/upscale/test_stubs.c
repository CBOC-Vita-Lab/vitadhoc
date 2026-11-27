/* Stand-ins for what upscale_vita.c calls outside itself (the event log and
 * the fourth-core glue in handoff_glue.cpp): the upscaler's own test runs
 * the three-core layout with no watchdog and drops log lines. */
#include <stdarg.h>
void fe_evt(const char *fmt, ...) { (void)fmt; }
int  core4_on_core3(int role, int index) { (void)role; (void)index; return 0; }
int  core4_wd_add(const char *name, int role, int index) { (void)name; (void)role; (void)index; return -1; }
void core4_beat(int slot) { (void)slot; }
void core4_busy(int slot, int busy) { (void)slot; (void)busy; }
void core4_set_thread(int role, int index, int thid) { (void)role; (void)index; (void)thid; }
