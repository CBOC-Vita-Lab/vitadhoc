/* mgba_shim.h -- just enough of mGBA's and cmocka's interfaces to compile
 * mGBA's own cheat test-suite (mgba/gba_test_cheats.c, an unmodified copy of
 * mGBA src/gba/test/cheats.c, commit c3c8e5e) against gpsp/cheat_engine.c.
 *
 * A "set" keeps its lines as text and is recompiled with the fixed
 * interpretation the test asked for; mCheatRefresh runs it once over a fake
 * 32 KiB internal RAM.  The stub headers under mgba/ all include this file.
 */
#ifndef GBADHOC_MGBA_SHIM_H
#define GBADHOC_MGBA_SHIM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheat_engine.h"

#define ATTRIBUTE_UNUSED __attribute__((unused))

enum { GBA_CHEAT_AUTODETECT, GBA_CHEAT_CODEBREAKER, GBA_CHEAT_GAMESHARK,
       GBA_CHEAT_PRO_ACTION_REPLAY, GBA_CHEAT_VBA };
enum GBACheatGameSharkVersion { GBA_GS_NOT_SET = 0, GBA_GS_GSAV1 = 1,
       GBA_GS_GSAV1_RAW = 2, GBA_GS_PARV3 = 3, GBA_GS_PARV3_RAW = 4 };

struct mCore;
struct mCheatDevice;

struct mCheatSet
{
   bool (*addLine)(struct mCheatSet *, const char *line, int type);
   bool enabled;
   int  version;
   char text[8192];
};
struct GBACheatSet;     /* the tests only cast to it */

struct mCheatDevice
{
   struct mCheatSet *(*createSet)(struct mCheatDevice *, const char *name);
   struct mCore *p;
   bool buttonDown;
};

struct mCore
{
   bool (*init)(struct mCore *);
   void (*deinit)(struct mCore *);
   void (*reset)(struct mCore *);
   struct mCheatDevice *(*cheatDevice)(struct mCore *);
   uint32_t (*rawRead8)(struct mCore *, uint32_t address, int segment);
   uint32_t (*rawRead16)(struct mCore *, uint32_t address, int segment);
   uint32_t (*rawRead32)(struct mCore *, uint32_t address, int segment);
   void (*rawWrite8)(struct mCore *, uint32_t address, int segment, uint8_t);
   void (*rawWrite16)(struct mCore *, uint32_t address, int segment, uint16_t);
   void (*rawWrite32)(struct mCore *, uint32_t address, int segment, uint32_t);
   int config;
   struct mCheatDevice device;
   uint8_t iwram[0x8000];
   uint8_t ewram[0x40000];
};

struct mCore *GBACoreCreate(void);
void mCoreInitConfig(struct mCore *, const char *);
void mCoreConfigDeinit(int *config);
void mCheatSetDeinit(struct mCheatSet *);
void mCheatRefresh(struct mCheatDevice *, struct mCheatSet *);
void mCheatPressButton(struct mCheatDevice *, bool down);
void GBACheatSetGameSharkVersion(struct GBACheatSet *, int version);

/* ---- cmocka ---------------------------------------------------------- */
struct CMUnitTest
{
   const char *name;
   void (*fn)(void **state);
   int (*setup)(void **state);
   int (*teardown)(void **state);
};
extern int shim_failures;
void shim_fail(const char *file, int line, const char *what, long a, long b);
int shim_run(const char *suite, const struct CMUnitTest *tests, unsigned n);

#define cmocka_unit_test_setup_teardown(f, s, t) { #f, f, s, t }
#define assert_true(x) \
   do { if (!(x)) shim_fail(__FILE__, __LINE__, "true: " #x, 0, 0); } while (0)
#define assert_false(x) \
   do { if (x) shim_fail(__FILE__, __LINE__, "false: " #x, 0, 0); } while (0)
#define assert_non_null(x) \
   do { if (!(x)) shim_fail(__FILE__, __LINE__, "non-null: " #x, 0, 0); } while (0)
#define assert_int_equal(a, b) \
   do { long a_ = (long)(a), b_ = (long)(b); \
        if (a_ != b_) shim_fail(__FILE__, __LINE__, #a " == " #b, a_, b_); } while (0)

#define M_TEST_DEFINE(NAME) static void NAME(void **state ATTRIBUTE_UNUSED)
#define M_TEST_SUITE_DEFINE(NAME, ...) \
   int mgba_suite_main(void); \
   int mgba_suite_main(void) { \
      static const struct CMUnitTest tests[] = { __VA_ARGS__ }; \
      return shim_run(#NAME, tests, sizeof(tests) / sizeof(tests[0])); \
   }

#endif
