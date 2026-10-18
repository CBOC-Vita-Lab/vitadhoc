/* mgba_shim.c -- see mgba_shim.h. */
#include "mgba_shim.h"

int shim_failures;
static int test_failed;

void shim_fail(const char *file, int line, const char *what, long a, long b)
{
   const char *base = strrchr(file, '/');
   if (!test_failed)
      printf("\n");
   printf("      %s:%d: %s  (0x%lx, 0x%lx)\n", base ? base + 1 : file, line,
          what, a, b);
   test_failed = 1;
}

/* ---- memory ------------------------------------------------------------ */

static uint8_t *mem_ptr(struct mCore *core, uint32_t address)
{
   if ((address >> 24) == 3)
      return &core->iwram[address & 0x7FFF];
   if ((address >> 24) == 2)
      return &core->ewram[address & 0x3FFFF];
   return NULL;
}

static uint32_t mem_read(struct mCore *core, uint32_t address, int width)
{
   uint32_t v = 0;
   int i;
   for (i = 0; i < width; i++)
   {
      uint8_t *p = mem_ptr(core, address + (uint32_t)i);
      if (p)
         v |= (uint32_t)*p << (8 * i);
   }
   return v;
}

static void mem_write(struct mCore *core, uint32_t address, int width,
                      uint32_t value)
{
   int i;
   for (i = 0; i < width; i++)
   {
      uint8_t *p = mem_ptr(core, address + (uint32_t)i);
      if (p)
         *p = (uint8_t)(value >> (8 * i));
   }
}

static uint32_t bus_read(void *ud, uint32_t a, int w)
{
   return mem_read((struct mCore *)ud, a, w);
}
static void bus_write(void *ud, uint32_t a, int w, uint32_t v)
{
   mem_write((struct mCore *)ud, a, w, v);
}

static uint32_t raw_read8(struct mCore *c, uint32_t a, int s)  { (void)s; return mem_read(c, a, 1); }
static uint32_t raw_read16(struct mCore *c, uint32_t a, int s) { (void)s; return mem_read(c, a, 2); }
static uint32_t raw_read32(struct mCore *c, uint32_t a, int s) { (void)s; return mem_read(c, a, 4); }
static void raw_write8(struct mCore *c, uint32_t a, int s, uint8_t v)   { (void)s; mem_write(c, a, 1, v); }
static void raw_write16(struct mCore *c, uint32_t a, int s, uint16_t v) { (void)s; mem_write(c, a, 2, v); }
static void raw_write32(struct mCore *c, uint32_t a, int s, uint32_t v) { (void)s; mem_write(c, a, 4, v); }

/* ---- core / device / set ------------------------------------------------ */

static bool core_init(struct mCore *core) { (void)core; return true; }
static void core_deinit(struct mCore *core) { free(core); }
static void core_reset(struct mCore *core)
{
   memset(core->iwram, 0, sizeof(core->iwram));
   memset(core->ewram, 0, sizeof(core->ewram));
}
static struct mCheatDevice *core_device(struct mCore *core)
{
   return &core->device;
}

static cheat_op ops[2048];
static cheat_patch patches[64];

static int set_compile(struct mCheatSet *set, cheat_out *out)
{
   memset(out, 0, sizeof(*out));
   out->ops = ops;
   out->cap_ops = sizeof(ops) / sizeof(ops[0]);
   out->patches = patches;
   out->cap_patches = sizeof(patches) / sizeof(patches[0]);
   return cheat_gba_compile_as(set->text, set->version, out);
}

static bool set_add_line(struct mCheatSet *set, const char *line, int type)
{
   size_t len = strlen(set->text);
   cheat_out out;
   int rc;

   if (type == GBA_CHEAT_CODEBREAKER)
      set->version = 5;
   else if (!set->version)
      set->version = type == GBA_CHEAT_GAMESHARK ? 1 : 3;
   snprintf(set->text + len, sizeof(set->text) - len, "%s%s", len ? "+" : "",
            line);
   rc = set_compile(set, &out);
   /* A line that waits for its parameter line is accepted, as in mGBA. */
   if (rc == CE_OK || rc == CE_E_TRUNCATED)
      return true;
   set->text[len] = 0;
   return false;
}

static struct mCheatSet *create_set(struct mCheatDevice *device,
                                    const char *name)
{
   struct mCheatSet *set = (struct mCheatSet *)calloc(1, sizeof(*set));
   (void)device;
   (void)name;
   set->addLine = set_add_line;
   set->enabled = true;
   return set;
}

struct mCore *GBACoreCreate(void)
{
   struct mCore *core = (struct mCore *)calloc(1, sizeof(*core));
   core->init = core_init;
   core->deinit = core_deinit;
   core->reset = core_reset;
   core->cheatDevice = core_device;
   core->rawRead8 = raw_read8;
   core->rawRead16 = raw_read16;
   core->rawRead32 = raw_read32;
   core->rawWrite8 = raw_write8;
   core->rawWrite16 = raw_write16;
   core->rawWrite32 = raw_write32;
   core->device.createSet = create_set;
   core->device.p = core;
   return core;
}

void mCoreInitConfig(struct mCore *core, const char *x) { (void)core; (void)x; }
void mCoreConfigDeinit(int *config) { (void)config; }
void mCheatSetDeinit(struct mCheatSet *set) { free(set); }

void mCheatRefresh(struct mCheatDevice *device, struct mCheatSet *set)
{
   cheat_out out;
   cheat_bus bus;
   int rc = set_compile(set, &out);

   if (!set->enabled || (rc != CE_OK && rc != CE_E_TRUNCATED))
      return;
   bus.read = bus_read;
   bus.write = bus_write;
   bus.ud = device->p;
   cheat_vm_run(out.ops, out.n_ops, &bus, device->buttonDown);
}

void mCheatPressButton(struct mCheatDevice *device, bool down)
{
   device->buttonDown = down;
}

void GBACheatSetGameSharkVersion(struct GBACheatSet *set, int version)
{
   ((struct mCheatSet *)set)->version = version;
}

/* ---- runner ---------------------------------------------------------- */

int shim_run(const char *suite, const struct CMUnitTest *tests, unsigned n)
{
   unsigned i, failed = 0;
   for (i = 0; i < n; i++)
   {
      void *state = NULL;
      test_failed = 0;
      printf("   %s.%-32s", suite, tests[i].name);
      if (tests[i].setup)
         tests[i].setup(&state);
      tests[i].fn(&state);
      if (tests[i].teardown)
         tests[i].teardown(&state);
      if (test_failed)
      {
         failed++;
         printf("   -> FAILED\n");
      }
      else
         printf(" ok\n");
   }
   shim_failures += (int)failed;
   printf("   %s: %u of %u passed\n", suite, n - failed, n);
   return (int)failed;
}
