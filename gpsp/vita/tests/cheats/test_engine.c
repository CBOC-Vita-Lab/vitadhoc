/* test_engine.c -- unit tests of gpsp/cheat_engine.c.
 *
 *   1. mGBA's own cheat test-suite (mgba/gba_test_cheats.c, unmodified)
 *      through mgba_shim.c: 25 tests of the Action Replay v3 semantics.
 *   2. The ciphers against a second implementation: VisualBoyAdvance-M's
 *      (vbam_ref.cpp, unmodified excerpts), on thousands of random inputs.
 *   3. Published codes with a self-evident plaintext: a master code must
 *      decrypt to "<game id> 001DC0DE" and a hook line; and an encrypted
 *      CodeBreaker list (Mega Man Battle Network 6 Gregar, libretro
 *      database) must decrypt to the plain list the database has for its
 *      twin (Falzar).
 *   4. Every code type: text in, memory out, over a fake bus.
 *   5. Detection, the '+' spelling, placeholders, errors, seeds collected
 *      from a whole list in any order.
 *   6. Game Boy: GameShark and Game Genie.
 *
 * This file includes the engine's source to reach its static functions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../cheat_engine.c"

int mgba_suite_main(void);

/* vbam_ref.cpp */
void vbam_gsa_seed(uint16_t deadface, int v3);
void vbam_gsa_decrypt(uint32_t *a, uint32_t *v, int v3);
void vbam_gsa_seeds(uint32_t out[4], int v3);
void vbam_cba_seed(uint32_t address, uint32_t value);
void vbam_cba_decrypt(uint32_t *address, uint16_t *value);
extern uint8_t cheatsCBASeedBuffer[0x30];
extern uint32_t cheatsCBASeed[4];

static int failures, checks;

#define CHECK(cond, ...)                                                     \
   do {                                                                      \
      checks++;                                                              \
      if (!(cond))                                                           \
      {                                                                      \
         failures++;                                                         \
         printf("   FAIL %s:%d: ", __FILE__, __LINE__);                      \
         printf(__VA_ARGS__);                                                \
         printf("\n");                                                       \
      }                                                                      \
   } while (0)

/* ---- a fake GBA ---------------------------------------------------------- */

static uint8_t ewram[0x40000], iwram[0x8000], io[0x400];
static unsigned writes_elsewhere;

static uint8_t *mem(uint32_t a)
{
   switch (a >> 24)
   {
   case 2: return &ewram[a & 0x3FFFF];
   case 3: return &iwram[a & 0x7FFF];
   case 4: return (a & 0xFFFFFF) < 0x400 ? &io[a & 0x3FF] : NULL;
   }
   return NULL;
}

static uint32_t rd(void *ud, uint32_t a, int w)
{
   uint32_t v = 0;
   int i;
   (void)ud;
   for (i = 0; i < w; i++)
      if (mem(a + (uint32_t)i))
         v |= (uint32_t)*mem(a + (uint32_t)i) << (8 * i);
   return v;
}

static void wr(void *ud, uint32_t a, int w, uint32_t v)
{
   int i;
   (void)ud;
   for (i = 0; i < w; i++)
   {
      if (mem(a + (uint32_t)i))
         *mem(a + (uint32_t)i) = (uint8_t)(v >> (8 * i));
      else
         writes_elsewhere++;
   }
}

static const cheat_bus bus = { rd, wr, NULL };
static cheat_op ops[4096];
static cheat_patch patches[256];

static void reset(void)
{
   memset(ewram, 0, sizeof(ewram));
   memset(iwram, 0, sizeof(iwram));
   memset(io, 0, sizeof(io));
   io[0x130] = 0xFF;            /* KEYINPUT: nothing held */
   io[0x131] = 0x03;
   writes_elsewhere = 0;
}

static cheat_out out_new(void)
{
   cheat_out o;
   memset(&o, 0, sizeof(o));
   o.ops = ops;
   o.cap_ops = sizeof(ops) / sizeof(ops[0]);
   o.patches = patches;
   o.cap_patches = sizeof(patches) / sizeof(patches[0]);
   return o;
}

/* compile with detection, run once; returns rc */
static int run(const char *code, const cheat_gba_ctx *ctx, int *kind,
               cheat_out *o, int button)
{
   int rc;
   *o = out_new();
   rc = cheat_gba_compile(code, ctx, o, kind);
   if (rc == CE_OK)
      cheat_vm_run(o->ops, o->n_ops, &bus, button);
   return rc;
}

static uint32_t rng_state = 0x12345678;
static uint32_t rnd(void)
{
   rng_state ^= rng_state << 13;
   rng_state ^= rng_state >> 17;
   rng_state ^= rng_state << 5;
   return rng_state;
}

/* ---- 2. the ciphers against VBA-M ------------------------------------------ */

/* The inverse of cb_decrypt (the engine only ever decrypts). */
static void cb_encrypt(const gba_build *b, uint32_t *op1, uint16_t *op2)
{
   uint8_t buffer[6];
   uint32_t master = b->cb_master;
   int i;

   *op1 ^= b->cb_seeds[2];
   *op2 ^= (uint16_t)b->cb_seeds[3];
   cb_load(buffer, *op1, *op2);
   buffer[0] ^= (uint8_t)master;
   for (i = 1; i <= 5; ++i)
      buffer[i] ^= (uint8_t)(master ^ buffer[i - 1]);
   buffer[5] ^= (uint8_t)(master >> 8);
   for (i = 4; i >= 0; --i)
      buffer[i] ^= (uint8_t)((master >> 8) ^ buffer[i + 1]);
   cb_store(buffer, op1, op2);
   *op1 ^= b->cb_seeds[0];
   *op2 ^= (uint16_t)b->cb_seeds[1];
   cb_load(buffer, *op1, *op2);
   for (i = 0; i < (int)sizeof(b->cb_table); ++i)
   {
      unsigned ox = (unsigned)i >> 3, oy = b->cb_table[i] >> 3;
      int bx = i & 7, by = b->cb_table[i] & 7;
      uint8_t x = (buffer[ox] >> bx) & 1, y = (buffer[oy] >> by) & 1;
      buffer[ox] = (uint8_t)((buffer[ox] & ~(1 << bx)) | (y << bx));
      buffer[oy] = (uint8_t)((buffer[oy] & ~(1 << by)) | (x << by));
   }
   cb_store(buffer, op1, op2);
}

static void test_ciphers(void)
{
   int i, k, v3;
   uint32_t seeds[4], ref[4];
   cheat_out o = out_new();
   gba_build b;

   printf("ciphers against VisualBoyAdvance-M\n");
   for (v3 = 0; v3 < 2; v3++)
   {
      const uint8_t *t1 = v3 ? ce_par3T1 : ce_gsa1T1;
      const uint8_t *t2 = v3 ? ce_par3T2 : ce_gsa1T2;
      vbam_gsa_seed(0, v3);
      vbam_gsa_seeds(ref, v3);
      CHECK(!memcmp(ref, v3 ? cheat_par3_default_seeds : cheat_gsa1_default_seeds,
                    sizeof(ref)),
            "default seeds v%d: %08X %08X %08X %08X", v3 ? 3 : 1,
            (unsigned)ref[0], (unsigned)ref[1], (unsigned)ref[2], (unsigned)ref[3]);
      for (i = 0; i < 2000; i++)
      {
         uint16_t df = (uint16_t)rnd();
         uint32_t a = rnd(), v = rnd(), a2 = a, v2 = v, a3, v3_;
         memset(seeds, 0, sizeof(seeds));
         gsa_reseed(seeds, df, t1, t2);
         vbam_gsa_seed(df, v3);
         vbam_gsa_seeds(ref, v3);
         CHECK(!memcmp(seeds, ref, sizeof(ref)), "reseed %04X v%d", df, v3 ? 3 : 1);
         cheat_gsa_decrypt(&a, &v, seeds);
         vbam_gsa_decrypt(&a2, &v2, v3);
         CHECK(a == a2 && v == v2, "TEA decrypt differs, seed %04X", df);
         a3 = a;
         v3_ = v;
         cheat_gsa_encrypt(&a3, &v3_, seeds);
         cheat_gsa_decrypt(&a3, &v3_, seeds);
         CHECK(a3 == a && v3_ == v, "encrypt then decrypt");
      }
   }

   for (i = 0; i < 400; i++)
   {
      uint32_t s1 = 0x90000000u | (rnd() & 0x0FFFFFFF);
      uint16_t s2 = (uint16_t)rnd();
      build_init(&b, &o);
      cb_reseed(&b, s1, s2);
      vbam_cba_seed(s1, s2);
      CHECK(!memcmp(b.cb_table, cheatsCBASeedBuffer, 0x30),
            "CodeBreaker table, seed %08X %04X", (unsigned)s1, s2);
      CHECK(!memcmp(b.cb_seeds, cheatsCBASeed, sizeof(b.cb_seeds)),
            "CodeBreaker seeds, seed %08X %04X", (unsigned)s1, s2);
      for (k = 0; k < 25; k++)
      {
         uint32_t a = rnd(), a2, a3;
         uint16_t v = (uint16_t)rnd(), v2, v3_;
         a2 = a3 = a;
         v2 = v3_ = v;
         cb_decrypt(&b, &a, &v);
         vbam_cba_decrypt(&a2, &v2);
         CHECK(a == a2 && v == v2, "CodeBreaker decrypt differs, seed %08X %04X",
               (unsigned)s1, s2);
         /* and the test's own encryptor inverts it (used below) */
         cb_encrypt(&b, &a, &v);
         CHECK(a == a3 && v == v3_, "CodeBreaker encrypt is not the inverse");
      }
   }
}

/* ---- 3. published codes -------------------------------------------------------- */

static void test_published(void)
{
   uint32_t a, v;
   gba_build b;
   cheat_out o = out_new();
   static const struct { uint32_t c1; uint16_t c2; uint32_t p1; uint16_t p2; } mmbn6[] = {
      /* "[M] Must Be On" after its seed line: the game id and the hook */
      { 0x5E4E1F94, 0x523A, 0x000055D4, 0x000A },
      { 0xDB3A6436, 0x9A43, 0x1004F754, 0x0007 },
      /* "No Random Battles" / "Infinite HP": the Falzar file has them plain */
      { 0xE3F3DCD3, 0x10A6, 0x720094C0, 0x0001 },
      { 0x1A083DBC, 0xD816, 0x82001C16, 0x0000 },
      { 0xD8609890, 0x2576, 0x7203AA04, 0x84F0 },
      { 0x777F4707, 0xDA29, 0x8203A9D4, 0x270F },
   };
   unsigned i;

   printf("published codes\n");
   /* Pokemon Emerald, Action Replay v3 master code */
   a = 0xD8BAE4D9; v = 0x4864DCE5;
   cheat_gsa_decrypt(&a, &v, cheat_par3_default_seeds);
   CHECK(a == 0xC40005EC && v == 0x00008401, "Emerald hook %08X %08X",
         (unsigned)a, (unsigned)v);
   a = 0xA86CDBA5; v = 0x19BA49B3;
   cheat_gsa_decrypt(&a, &v, cheat_par3_default_seeds);
   CHECK(a == 0x45455042 /* "BPEE" */ && v == 0x001DC0DE, "Emerald id %08X %08X",
         (unsigned)a, (unsigned)v);
   /* Mario & Luigi: Superstar Saga, GameShark Advance master code */
   a = 0x502F4DE9; v = 0x75D4B6CA;
   cheat_gsa_decrypt(&a, &v, cheat_gsa1_default_seeds);
   CHECK(a == 0xF8017F8C && v == 0x00000101, "M&L hook %08X %08X",
         (unsigned)a, (unsigned)v);
   a = 0x8791F574; v = 0x20D9F121;
   cheat_gsa_decrypt(&a, &v, cheat_gsa1_default_seeds);
   CHECK(a == 0x45383841 /* "A88E" */ && v == 0x001DC0DE, "M&L id %08X %08X",
         (unsigned)a, (unsigned)v);
   /* ... and "Unlimited Coins" of the same list: a 16-bit write of 9999 */
   a = 0xA9873144; v = 0x203A53CE;
   cheat_gsa_decrypt(&a, &v, cheat_gsa1_default_seeds);
   CHECK(a == 0x120048E0 && v == 0x0000270F, "M&L coins %08X %08X",
         (unsigned)a, (unsigned)v);

   /* Mega Man Battle Network 6 Gregar: seed code 930CFE0E 61A7 */
   build_init(&b, &o);
   cb_reseed(&b, 0x930CFE0E, 0x61A7);
   for (i = 0; i < sizeof(mmbn6) / sizeof(mmbn6[0]); i++)
   {
      uint32_t x = mmbn6[i].c1;
      uint16_t y = mmbn6[i].c2;
      cb_decrypt(&b, &x, &y);
      CHECK(x == mmbn6[i].p1 && y == mmbn6[i].p2, "MMBN6 line %u: %08X %04X",
            i, (unsigned)x, y);
   }

   /* The same through the front door: the list as the database has it. */
   {
      cheat_gba_ctx ctx;
      int kind = 0, rc;
      cheat_gba_ctx_init(&ctx);
      cheat_gba_ctx_scan(&ctx, "D86098902576+777F4707DA29");
      cheat_gba_ctx_scan(&ctx, "930CFE0E61A7+5E4E1F94523A+DB3A64369A43");
      CHECK(ctx.n_cb == 1, "one CodeBreaker seed set, got %u", ctx.n_cb);
      reset();
      ewram[0x3AA04] = 0xF0; ewram[0x3AA05] = 0x84;   /* the "if" is true */
      rc = run("D86098902576+777F4707DA29", &ctx, &kind, &o, 0);
      CHECK(rc == CE_OK && kind == CE_KIND_GBA_CODEBREAKER &&
            (o.flags & CE_F_DECRYPTED), "MMBN6 Infinite HP rc %d kind %d", rc, kind);
      CHECK(rd(NULL, 0x0203A9D4, 2) == 0x270F, "MMBN6 Infinite HP wrote %04X",
            (unsigned)rd(NULL, 0x0203A9D4, 2));
      rc = run("930CFE0E61A7+5E4E1F94523A+DB3A64369A43", &ctx, &kind, &o, 0);
      CHECK(rc == CE_OK && (o.flags & CE_F_NOOP) && (o.flags & CE_F_MASTER),
            "MMBN6 master rc %d flags %x", rc, o.flags);
      /* without the master code in the list it is refused, not run as noise */
      rc = run("D86098902576+777F4707DA29", NULL, &kind, &o, 0);
      CHECK(rc == CE_E_ENCRYPTED, "no seeds: rc %d", rc);
   }

   /* German Pokemon Emerald, Action Replay v3 (libretro database): codes
    * that name RAM by a mirror address.  Believed only because the list's
    * master code proves it an Action Replay's. */
   {
      cheat_gba_ctx ctx;
      int kind = 0, rc;
      cheat_gba_ctx_init(&ctx);
      cheat_gba_ctx_scan(&ctx, "593A913F+EEDFDBF3");
      cheat_gba_ctx_scan(&ctx, "D8BAE4D9+4864DCE5+DD5F41DA+90B02762+96B7FF51+60DE4853+1C7B3231+B494738C");
      CHECK(ctx.par3_sure[0] && !ctx.gsa1_sure[0], "the master code's id line marks the list");
      reset();
      rc = run("593A913F+EEDFDBF3", &ctx, &kind, &o, 0);      /* 022C2DEC 0000270F */
      CHECK(rc == CE_OK && kind == CE_KIND_GBA_AR3 && rd(NULL, 0x02002DEC, 2) == 0x270F,
            "mirrored address in a sure list: rc %d kind %d", rc, kind);
      rc = run("D8BAE4D9+4864DCE5+DD5F41DA+90B02762+96B7FF51+60DE4853+1C7B3231+B494738C",
               &ctx, &kind, &o, 0);
      CHECK(rc == CE_OK && kind == CE_KIND_GBA_AR3 && o.n_patches == 1 && !o.n_ops &&
            patches[0].address == 0x08076BF0 && patches[0].value == 0x2400,
            "its master code: rc %d, %u patches at %08X", rc, o.n_patches,
            (unsigned)patches[0].address);
      reset();
      rc = run("593A913F+EEDFDBF3", NULL, &kind, &o, 0);
      CHECK(rc == CE_E_FORMAT && !rd(NULL, 0x02002DEC, 2), "the same line alone: rc %d", rc);
   }

   /* Emerald's master code through the front door */
   {
      int kind = 0, rc;
      rc = run("D8BAE4D9 4864DCE5+A86CDBA5 19BA49B3", NULL, &kind, &o, 0);
      CHECK(rc == CE_OK && kind == CE_KIND_GBA_AR3 && (o.flags & CE_F_NOOP),
            "Emerald master rc %d kind %d flags %x", rc, kind, o.flags);
      rc = run("502F4DE9+75D4B6CA+8791F574+20D9F121", NULL, &kind, &o, 0);
      CHECK(rc == CE_OK && kind == CE_KIND_GBA_GAMESHARK && (o.flags & CE_F_NOOP),
            "M&L master rc %d kind %d", rc, kind);
      reset();
      rc = run("A9873144+203A53CE", NULL, &kind, &o, 0);
      CHECK(rc == CE_OK && kind == CE_KIND_GBA_GAMESHARK &&
            rd(NULL, 0x020048E0, 2) == 0x270F, "M&L coins rc %d kind %d", rc, kind);
   }
}

/* ---- 4. every code type ---------------------------------------------------------- */

typedef struct vec
{
   const char *what;
   const char *code;
   int kind;
   /* memory before (address, width, value) x3 and after x6; width 0 ends */
   struct { uint32_t a; int w; uint32_t v; } pre[3], post[6];
   int button;
   uint16_t keys_held;          /* GBA key bits held (1 = held) */
} vec;

static const vec vectors[] = {
   /* ---- raw ---- */
   { "raw VBA 8",  "02000100:AB", CE_KIND_GBA_RAW, {{0}}, {{0x02000100, 4, 0xAB}} },
   { "raw VBA 16", "02000100:ABCD", CE_KIND_GBA_RAW, {{0}}, {{0x02000100, 4, 0xABCD}} },
   { "raw VBA 32", "03000100:89ABCDEF", CE_KIND_GBA_RAW, {{0}}, {{0x03000100, 4, 0x89ABCDEF}} },
   { "raw 8+2",    "02000100 7F", CE_KIND_GBA_RAW, {{0}}, {{0x02000100, 4, 0x7F}} },
   { "raw 8+4",    "02000100 1234", CE_KIND_GBA_RAW, {{0}}, {{0x02000100, 4, 0x1234}} },
   { "raw 8+8 wide", "02000100 12345678", CE_KIND_GBA_RAW, {{0}}, {{0x02000100, 4, 0x12345678}} },
   { "raw 8+8 byte", "02000100 000000FF", CE_KIND_GBA_RAW,
     {{0x02000100, 4, 0x11111111}}, {{0x02000100, 4, 0x111111FF}} },
   { "raw lower case, newline", "02000100:ab\n02000104:cd", CE_KIND_GBA_RAW, {{0}},
     {{0x02000100, 1, 0xAB}, {0x02000104, 1, 0xCD}} },

   /* ---- CodeBreaker ---- */
   { "CB 3 write 8",  "33000010 00AB", CE_KIND_GBA_CODEBREAKER, {{0}}, {{0x03000010, 2, 0xAB}} },
   { "CB 8 write 16", "82000100 1234", CE_KIND_GBA_CODEBREAKER, {{0}}, {{0x02000100, 4, 0x1234}} },
   { "CB 2 or",  "22000100 00F0", CE_KIND_GBA_CODEBREAKER, {{0x02000100, 2, 0x1203}}, {{0x02000100, 2, 0x12F3}} },
   { "CB 6 and", "62000100 0F0F", CE_KIND_GBA_CODEBREAKER, {{0x02000100, 2, 0x1234}}, {{0x02000100, 2, 0x0204}} },
   { "CB E add 16", "E2000100 0005", CE_KIND_GBA_CODEBREAKER, {{0x02000100, 2, 0xFFFE}}, {{0x02000100, 4, 0x0003}} },
   { "CB E add 32", "E2000101 FFFF", CE_KIND_GBA_CODEBREAKER, {{0x02000100, 4, 0x00010000}}, {{0x02000100, 4, 0x0000FFFF}} },
   { "CB 4 slide", "42000200 0010+00010004 0002", CE_KIND_GBA_CODEBREAKER, {{0}},
     {{0x02000200, 2, 0x10}, {0x02000202, 2, 0x11}, {0x02000204, 2, 0x12},
      {0x02000206, 2, 0x13}, {0x02000208, 2, 0}} },
   { "CB 5 super", "52000300 0003+11223344 5566", CE_KIND_GBA_CODEBREAKER, {{0}},
     {{0x02000300, 4, 0x44332211}, {0x02000304, 4, 0x00006655}} },
   { "CB 5 super, two data lines", "52000300 0004+11223344 5566+77880000 0000",
     CE_KIND_GBA_CODEBREAKER, {{0}},
     {{0x02000300, 4, 0x44332211}, {0x02000304, 4, 0x88776655}, {0x02000308, 4, 0}} },
   { "CB 7 if eq, true", "72000100 0005+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0x02000100, 2, 5}}, {{0x02000104, 2, 0xBEEF}} },
   { "CB 7 if eq, false", "72000100 0005+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0x02000100, 2, 6}}, {{0x02000104, 2, 0}} },
   { "CB A if ne, true", "A2000100 0005+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0x02000100, 2, 6}}, {{0x02000104, 2, 0xBEEF}} },
   { "CB A if ne, false", "A2000100 0005+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0x02000100, 2, 5}}, {{0x02000104, 2, 0}} },
   { "CB B if gt, true", "B2000100 0005+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0x02000100, 2, 6}}, {{0x02000104, 2, 0xBEEF}} },
   { "CB B if gt, false", "B2000100 0005+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0x02000100, 2, 5}}, {{0x02000104, 2, 0}} },
   { "CB C if lt, true", "C2000100 0005+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0x02000100, 2, 4}}, {{0x02000104, 2, 0xBEEF}} },
   { "CB C if lt, false", "C2000100 0005+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0x02000100, 2, 5}}, {{0x02000104, 2, 0}} },
   { "CB F if and, true", "F2000100 0010+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0x02000100, 2, 0x30}}, {{0x02000104, 2, 0xBEEF}} },
   { "CB F if and, false", "F2000100 0010+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0x02000100, 2, 0x2F}}, {{0x02000104, 2, 0}} },
   { "CB if guards only the next code", "72000100 0005+82000104 BEEF+82000108 CAFE",
     CE_KIND_GBA_CODEBREAKER, {{0x02000100, 2, 6}},
     {{0x02000104, 2, 0}, {0x02000108, 2, 0xCAFE}} },
   { "CB false if skips a whole super code", "72000100 0005+52000300 0003+11223344 5566+82000108 CAFE",
     CE_KIND_GBA_CODEBREAKER, {{0x02000100, 2, 6}},
     {{0x02000300, 4, 0}, {0x02000304, 4, 0}, {0x02000108, 2, 0xCAFE}} },
   { "CB true if runs a whole super code", "72000100 0005+52000300 0003+11223344 5566+82000108 CAFE",
     CE_KIND_GBA_CODEBREAKER, {{0x02000100, 2, 5}},
     {{0x02000300, 4, 0x44332211}, {0x02000304, 4, 0x6655}, {0x02000108, 2, 0xCAFE}} },
   { "CB false if skips a slide", "72000100 0005+42000200 0010+00010004 0002+82000108 CAFE",
     CE_KIND_GBA_CODEBREAKER, {{0x02000100, 2, 6}},
     {{0x02000200, 4, 0}, {0x02000204, 4, 0}, {0x02000108, 2, 0xCAFE}} },
   { "CB two ifs", "72000100 0005+72000102 0006+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0x02000100, 2, 5}, {0x02000102, 2, 6}}, {{0x02000104, 2, 0xBEEF}} },
   { "CB D 20 keys held", "D0000020 0204+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0}}, {{0x02000104, 2, 0xBEEF}}, 0, 0x0204 },       /* L + Select */
   { "CB D 20 one key short", "D0000020 0204+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0}}, {{0x02000104, 2, 0}}, 0, 0x0200 },
   { "CB D 10 not all held", "D0000010 0204+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0}}, {{0x02000104, 2, 0xBEEF}}, 0, 0x0200 },
   { "CB D 10 all held", "D0000010 0204+82000104 BEEF", CE_KIND_GBA_CODEBREAKER,
     {{0}}, {{0x02000104, 2, 0}}, 0, 0x0204 },
   { "CB master only", "0000317D+000A+1006C9E4+0007", CE_KIND_GBA_CODEBREAKER, {{0}}, {{0}} },
   { "CB run together", "820001001234", CE_KIND_GBA_CODEBREAKER, {{0}}, {{0x02000100, 2, 0x1234}} },
   { "CB libretro spelling", "82000100+1234+33000010+00AB", CE_KIND_GBA_CODEBREAKER, {{0}},
     {{0x02000100, 2, 0x1234}, {0x03000010, 1, 0xAB}} },

   /* ---- GameShark Advance / Action Replay v1, raw ---- */
   { "GSA 1 write 16", "12000100 0000ABCD", CE_KIND_GBA_GAMESHARK, {{0}}, {{0x02000100, 4, 0xABCD}} },
   { "GSA 2 write 32", "23000100 89ABCDEF", CE_KIND_GBA_GAMESHARK, {{0}}, {{0x03000100, 4, 0x89ABCDEF}} },
   { "GSA 0 write 8",  "03000101 000000EE+12000100 00001111", CE_KIND_GBA_GAMESHARK, {{0}},
     {{0x03000100, 4, 0x0000EE00}, {0x02000100, 2, 0x1111}} },
   { "GSA 3 list", "30000003 CAFEF00D+02000100 02000200+02000300 00000000", CE_KIND_GBA_GAMESHARK, {{0}},
     {{0x02000100, 4, 0xCAFEF00D}, {0x02000200, 4, 0xCAFEF00D}, {0x02000300, 4, 0xCAFEF00D}} },
   { "GSA D if eq, true", "D2000100 00000005+12000104 0000BEEF", CE_KIND_GBA_GAMESHARK,
     {{0x02000100, 2, 5}}, {{0x02000104, 2, 0xBEEF}} },
   { "GSA D if eq, false", "D2000100 00000005+12000104 0000BEEF", CE_KIND_GBA_GAMESHARK,
     {{0x02000100, 2, 4}}, {{0x02000104, 2, 0}} },
   { "GSA E if eq, next two, false", "E0020005 02000100+12000104 0000BEEF+12000108 0000CAFE+1200010C 0000F00D",
     CE_KIND_GBA_GAMESHARK, {{0x02000100, 2, 4}},
     {{0x02000104, 2, 0}, {0x02000108, 2, 0}, {0x0200010C, 2, 0xF00D}} },
   { "GSA E if eq, next two, true", "E0020005 02000100+12000104 0000BEEF+12000108 0000CAFE+1200010C 0000F00D",
     CE_KIND_GBA_GAMESHARK, {{0x02000100, 2, 5}},
     {{0x02000104, 2, 0xBEEF}, {0x02000108, 2, 0xCAFE}, {0x0200010C, 2, 0xF00D}} },
   { "GSA 8 button, held", "82100104 000000AB+12000108 00001111", CE_KIND_GBA_GAMESHARK, {{0}},
     {{0x02000104, 2, 0xAB}, {0x02000108, 2, 0x1111}}, 1 },
   { "GSA 8 button, not held", "82100104 000000AB+12000108 00001111", CE_KIND_GBA_GAMESHARK, {{0}},
     {{0x02000104, 2, 0}, {0x02000108, 2, 0x1111}}, 0 },
   { "GSA id + hook", "45383841 001DC0DE+F8017F8C 00000101", CE_KIND_GBA_GAMESHARK, {{0}}, {{0}} },

   /* ---- Pro Action Replay v3, raw (mGBA's suite covers the block ifs) ---- */
   { "PAR3 write 16", "02200100 0000ABCD", CE_KIND_GBA_AR3, {{0}}, {{0x02000100, 4, 0xABCD}} },
   { "PAR3 write 32", "04300100 89ABCDEF", CE_KIND_GBA_AR3, {{0}}, {{0x03000100, 4, 0x89ABCDEF}} },
   { "PAR3 fill 8", "00200100 000003AA", CE_KIND_GBA_AR3, {{0}},
     {{0x02000100, 4, 0xAAAAAAAA}, {0x02000104, 4, 0}} },
   { "PAR3 pointer 16", "42200100 0002BEEF", CE_KIND_GBA_AR3,
     {{0x02000100, 4, 0x02000200}}, {{0x02000204, 2, 0xBEEF}, {0x02000200, 2, 0}} },
   { "PAR3 add 8", "80200100 00000003", CE_KIND_GBA_AR3, {{0x02000100, 1, 0xFE}}, {{0x02000100, 2, 0x0001}} },
   { "PAR3 if eq 16, true", "0A200100 00000005+02200104 0000BEEF", CE_KIND_GBA_AR3,
     {{0x02000100, 2, 5}}, {{0x02000104, 2, 0xBEEF}} },
   { "PAR3 if eq 16, false", "0A200100 00000005+02200104 0000BEEF", CE_KIND_GBA_AR3,
     {{0x02000100, 2, 6}}, {{0x02000104, 2, 0}} },
   { "PAR3 if less (signed 8), true", "18200100 00000001+02200104 0000BEEF", CE_KIND_GBA_AR3,
     {{0x02000100, 1, 0xFF}}, {{0x02000104, 2, 0xBEEF}} },    /* -1 < 1 */
   { "PAR3 if less (unsigned 8), false", "28200100 00000001+02200104 0000BEEF", CE_KIND_GBA_AR3,
     {{0x02000100, 1, 0xFF}}, {{0x02000104, 2, 0}} },         /* 255 < 1 */
   { "PAR3 slide 16", "00000000 82200200+00000010 01040002", CE_KIND_GBA_AR3, {{0}},
     {{0x02000200, 2, 0x10}, {0x02000204, 2, 0x11}, {0x02000208, 2, 0x12},
      {0x0200020C, 2, 0x13}, {0x02000210, 2, 0}} },
   { "PAR3 I/O write 16", "C6000020 00001234+02200104 0000BEEF", CE_KIND_GBA_AR3, {{0}},
     {{0x04000020, 2, 0x1234}, {0x02000104, 2, 0xBEEF}} },
   { "PAR3 button, held", "00000000 10200100+000000AB 00000000", CE_KIND_GBA_AR3, {{0}},
     {{0x02000100, 1, 0xAB}}, 1 },
   { "PAR3 id + hook", "C40005EC 00008401+45455042 001DC0DE", CE_KIND_GBA_AR3, {{0}}, {{0}} },
   { "PAR3 padded with 80000000", "00000000+84200200+00630001+01030001+80000000+00000000", CE_KIND_GBA_AR3, {{0}},
     {{0x02000200, 4, 0x00630001}, {0x02000204, 4, 0x00630002}, {0x02000208, 4, 0x00630003},
      {0x0200020C, 4, 0}} },
};

static void test_vectors(void)
{
   unsigned i, k;
   printf("code types (%u vectors)\n", (unsigned)(sizeof(vectors) / sizeof(vectors[0])));
   for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
   {
      const vec *t = &vectors[i];
      cheat_out o;
      int kind = 0, rc;
      reset();
      for (k = 0; k < 3 && t->pre[k].w; k++)
         wr(NULL, t->pre[k].a, t->pre[k].w, t->pre[k].v);
      if (t->keys_held)
      {
         uint16_t reg = (uint16_t)(0x3FF & ~t->keys_held);
         io[0x130] = (uint8_t)reg;
         io[0x131] = (uint8_t)(reg >> 8);
      }
      rc = run(t->code, NULL, &kind, &o, t->button);
      CHECK(rc == CE_OK, "%s: rc %d", t->what, rc);
      CHECK(kind == t->kind, "%s: kind %d, want %d", t->what, kind, t->kind);
      for (k = 0; k < 6 && t->post[k].w; k++)
         CHECK(rd(NULL, t->post[k].a, t->post[k].w) == t->post[k].v,
               "%s: [%08X] = %X, want %X", t->what, (unsigned)t->post[k].a,
               (unsigned)rd(NULL, t->post[k].a, t->post[k].w),
               (unsigned)t->post[k].v);
      CHECK(!writes_elsewhere, "%s: %u writes outside memory", t->what,
            writes_elsewhere);
      /* a second frame changes nothing an assignment did */
   }
}

/* ---- 5. ROM patches, detection, errors, seeds ------------------------------------ */

static void test_rom_and_errors(void)
{
   cheat_out o;
   int kind, rc;
   unsigned i;
   static const struct { const char *code; int rc; } bad[] = {
      { "", CE_E_FORMAT },
      { "N/A", CE_E_FORMAT },
      { "Habe alle Medaillen", CE_E_FORMAT },
      { "8200282E2+FFFF", CE_E_FORMAT },          /* nine digits */
      { "82000100", CE_E_FORMAT },                /* half a line */
      { "82000100 1234 5678", CE_E_FORMAT },
      { "82000100 1234+12000100 0000ABCD", CE_E_FORMAT },   /* two devices */
      { "3300075C 00??", CE_E_PLACEHOLDER },
      { "82024E59+XXXX", CE_E_PLACEHOLDER },
      { "3300238C+00XX+330023EC+00YY", CE_E_PLACEHOLDER },
      { "XXX", CE_E_FORMAT },
      { "42000200 0010", CE_E_TRUNCATED },        /* slide without its line */
      { "52000300 0004+11223344 5566", CE_E_TRUNCATED },
      { "00000000 82200200", CE_E_TRUNCATED },
      { "30000003 CAFEF00D+02000100 02000200", CE_E_TRUNCATED },
      { "60A2DCEA C78E", CE_E_ENCRYPTED },        /* Klonoa 2: no seed code */
      { "7F0A8652EC54+67C8AB6D13BD", CE_E_ENCRYPTED },
      { "00000000 08000000", CE_E_UNSUPPORTED },  /* PARv3 slow motion */
      { "80F00000 00000101", CE_E_UNSUPPORTED },  /* GSA slow motion */
      { "6390870E 2BE30D25", CE_E_FORMAT },       /* noise in every reading */
   };

   printf("ROM patches, detection, errors\n");
   reset();
   rc = run("60001000 00002009", NULL, &kind, &o, 0);
   CHECK(rc == CE_OK && kind == CE_KIND_GBA_GAMESHARK && o.n_patches == 1 &&
         o.n_ops == 0 && patches[0].address == 0x08002000 &&
         patches[0].value == 0x2009 && patches[0].width == 2 &&
         (o.flags & CE_F_ROM_PATCH), "GSA 6 ROM patch rc %d", rc);
   rc = run("00000000 18001004+00009999 00000000", NULL, &kind, &o, 0);
   CHECK(rc == CE_OK && kind == CE_KIND_GBA_AR3 && o.n_patches == 1 &&
         patches[0].address == 0x08002008 && patches[0].value == 0x9999,
         "PARv3 ROM patch rc %d", rc);
   rc = run("08002008:55667788", NULL, &kind, &o, 0);
   CHECK(rc == CE_OK && kind == CE_KIND_GBA_RAW && o.n_patches == 1 &&
         patches[0].address == 0x08002008 && patches[0].value == 0x55667788 &&
         patches[0].width == 4, "raw ROM patch rc %d", rc);
   /* a CodeBreaker write into the cartridge is not a patch (nor memory) */
   rc = run("88002000 2009", NULL, &kind, &o, 0);
   CHECK(rc == CE_E_ENCRYPTED && !o.n_patches, "CB to ROM rc %d", rc);

   for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      reset();
      rc = run(bad[i].code, NULL, &kind, &o, 0);
      CHECK(rc == bad[i].rc, "\"%s\": rc %d, want %d", bad[i].code, rc, bad[i].rc);
      CHECK(!writes_elsewhere, "\"%s\" wrote", bad[i].code);
   }

   /* too many operations for the caller's array */
   {
      cheat_out small = out_new();
      small.cap_ops = 2;
      rc = cheat_gba_compile("82000100 0001+82000102 0002+82000104 0003", NULL,
                             &small, &kind);
      CHECK(rc == CE_E_TOO_LONG, "three ops into two: rc %d", rc);
   }
}

static void test_seeds(void)
{
   /* A game's list as a cheat device has it: a master code that changes the
    * seeds, and codes encrypted with the new ones.  The engine must decode
    * every entry whatever the order it meets them in. */
   cheat_gba_ctx ctx;
   cheat_out o;
   uint32_t seeds[4], a, v;
   char master[64], code[64], plain[160];
   int kind, rc, family;
   gba_build b;

   printf("seeds from the whole list\n");
   for (family = 0; family < 2; family++)
   {
      const uint32_t *def = family ? cheat_par3_default_seeds : cheat_gsa1_default_seeds;
      /* the same effect in each family's spelling: 16 bits to 0x02000104 */
      uint32_t p1 = family ? 0x02200104 : 0x12000104, p2 = 0x0000BEEF;
      a = 0xDEADFACE; v = 0x0000A5C3;
      cheat_gsa_encrypt(&a, &v, def);
      snprintf(master, sizeof(master), "%08X %08X", (unsigned)a, (unsigned)v);
      memset(seeds, 0, sizeof(seeds));
      gsa_reseed(seeds, 0xA5C3, family ? ce_par3T1 : ce_gsa1T1,
                 family ? ce_par3T2 : ce_gsa1T2);
      a = p1; v = p2;
      cheat_gsa_encrypt(&a, &v, seeds);
      snprintf(code, sizeof(code), "%08X+%08X", (unsigned)a, (unsigned)v);

      cheat_gba_ctx_init(&ctx);
      cheat_gba_ctx_scan(&ctx, code);       /* the code first, the master after */
      cheat_gba_ctx_scan(&ctx, master);
      CHECK((family ? ctx.n_par3 : ctx.n_gsa1) == 2, "family %d: seed sets", family);
      reset();
      rc = run(code, &ctx, &kind, &o, 0);
      CHECK(rc == CE_OK && kind == (family ? CE_KIND_GBA_AR3 : CE_KIND_GBA_GAMESHARK) &&
            rd(NULL, 0x02000104, 2) == 0xBEEF, "family %d: reseeded code rc %d kind %d",
            family, rc, kind);
      /* with the default seeds only it is noise, and refused */
      reset();
      rc = run(code, NULL, &kind, &o, 0);
      CHECK(rc != CE_OK && !rd(NULL, 0x02000104, 2), "family %d: without the master rc %d",
            family, rc);
      /* default-seed codes of the same list still decode */
      a = p1; v = p2;
      cheat_gsa_encrypt(&a, &v, def);
      snprintf(plain, sizeof(plain), "%08X %08X", (unsigned)a, (unsigned)v);
      reset();
      rc = run(plain, &ctx, &kind, &o, 0);
      CHECK(rc == CE_OK && rd(NULL, 0x02000104, 2) == 0xBEEF,
            "family %d: default-seed code beside a reseeding master rc %d", family, rc);
      /* master and code in ONE entry: the seeds change mid-entry */
      snprintf(plain, sizeof(plain), "%s+%s", master, code);
      reset();
      rc = run(plain, NULL, &kind, &o, 0);
      CHECK(rc == CE_OK && rd(NULL, 0x02000104, 2) == 0xBEEF,
            "family %d: master and code in one entry rc %d", family, rc);
   }

   /* CodeBreaker: seed code, encrypted master lines, encrypted codes; and a
    * plain code in the same list must stay plain. */
   o = out_new();
   build_init(&b, &o);
   cb_reseed(&b, 0x9A6C1234, 0x5E1F);
   {
      uint32_t x = 0x82000104;
      uint16_t y = 0xBEEF;
      cb_encrypt(&b, &x, &y);
      snprintf(code, sizeof(code), "%08X %04X", (unsigned)x, y);
      x = 0x0000ABCD; y = 0x000A;
      cb_encrypt(&b, &x, &y);
      snprintf(master, sizeof(master), "9A6C1234 5E1F+%08X %04X", (unsigned)x, y);
   }
   cheat_gba_ctx_init(&ctx);
   cheat_gba_ctx_scan(&ctx, "82000108 CAFE");
   cheat_gba_ctx_scan(&ctx, code);
   cheat_gba_ctx_scan(&ctx, master);
   CHECK(ctx.n_cb == 1, "CodeBreaker seed sets: %u", ctx.n_cb);
   reset();
   rc = run(code, &ctx, &kind, &o, 0);
   CHECK(rc == CE_OK && kind == CE_KIND_GBA_CODEBREAKER &&
         rd(NULL, 0x02000104, 2) == 0xBEEF, "encrypted CodeBreaker rc %d", rc);
   reset();
   rc = run("82000108 CAFE", &ctx, &kind, &o, 0);
   CHECK(rc == CE_OK && !(o.flags & CE_F_DECRYPTED) &&
         rd(NULL, 0x02000108, 2) == 0xCAFE, "plain CodeBreaker beside a seed code rc %d", rc);
   rc = run(master, &ctx, &kind, &o, 0);
   CHECK(rc == CE_OK && (o.flags & CE_F_NOOP), "CodeBreaker master rc %d flags %x", rc, o.flags);
   rc = run(code, NULL, &kind, &o, 0);
   CHECK(rc == CE_E_ENCRYPTED, "encrypted CodeBreaker without seeds rc %d", rc);
}

/* ---- 6. Game Boy -------------------------------------------------------------------- */

static void test_gb(void)
{
   cheat_gb_code c[16];
   unsigned n;
   int kind, rc;
   static const struct { const char *code; int rc; } bad[] = {
      { "", CE_E_FORMAT }, { "N/A", CE_E_FORMAT }, { "0.00", CE_E_FORMAT },
      { "010F214D1", CE_E_FORMAT }, { "0FF14CD", CE_E_FORMAT },
      { "016F4C06", CE_E_FORMAT },              /* a GameShark write to ROM */
      { "EE23-47AB", CE_E_FORMAT }, { "1004-39B-E6E", CE_E_FORMAT },
      { "01??C5D0", CE_E_PLACEHOLDER }, { "XX1-0D8-E6A", CE_E_PLACEHOLDER },
      { "$0D24206E", CE_E_UNSUPPORTED }, { "0D24206E", CE_E_UNSUPPORTED },
      { "003+7E8", CE_OK }, { "003", CE_E_FORMAT },
   };
   unsigned i;

   printf("Game Boy\n");
   rc = cheat_gb_compile("01FF16D0", c, 16, &n, &kind);
   CHECK(rc == CE_OK && kind == CE_KIND_GB_GAMESHARK && n == 1 && !c[0].rom &&
         c[0].type == 0x01 && c[0].value == 0xFF && c[0].address == 0xD016,
         "GameShark 01FF16D0");
   rc = cheat_gb_compile("91051AC8+0109d6ce", c, 16, &n, &kind);
   CHECK(rc == CE_OK && n == 2 && c[0].type == 0x91 && c[0].value == 0x05 &&
         c[0].address == 0xC81A && c[1].address == 0xCED6 && c[1].value == 0x09,
         "two GameShark codes");
   rc = cheat_gb_compile("D016:7F", c, 16, &n, &kind);
   CHECK(rc == CE_OK && n == 1 && c[0].address == 0xD016 && c[0].value == 0x7F,
         "address:value");

   /* Game Genie.  ABC-DEF-GHI: AB the new byte, FCDE the address with F
    * inverted, GI the old byte rotated left twice after XOR 0xBA. */
   rc = cheat_gb_compile("993-00F-A2E", c, 16, &n, &kind);
   CHECK(rc == CE_OK && kind == CE_KIND_GB_GAMEGENIE && n == 1 && c[0].rom &&
         c[0].address == 0x0300 && c[0].value == 0x99 && c[0].has_compare &&
         c[0].compare == 0x11, "Game Genie 993-00F-A2E: %04X %02X cmp %02X",
         c[0].address, c[0].value, c[0].compare);
   rc = cheat_gb_compile("990-00B-6E2", c, 16, &n, &kind);
   CHECK(rc == CE_OK && c[0].address == 0x4000 && c[0].value == 0x99 &&
         c[0].compare == 0x22, "Game Genie 990-00B-6E2");
   rc = cheat_gb_compile("FAA-12D", c, 16, &n, &kind);
   CHECK(rc == CE_OK && n == 1 && c[0].address == 0x2A12 && c[0].value == 0xFA &&
         !c[0].has_compare, "Game Genie without compare: %04X", c[0].address);
   /* the sixteenth address line is not compared: 0x8B94 is 0x0B94 */
   rc = cheat_gb_compile("66B-947-808", c, 16, &n, &kind);
   CHECK(rc == CE_OK && c[0].address == 0x0B94 && c[0].value == 0x66,
         "Game Genie high address: %04X", c[0].address);
   /* the database's spellings of a list */
   rc = cheat_gb_compile("009-ECE-7F2+C99-F5E-806", c, 16, &n, &kind);
   CHECK(rc == CE_OK && n == 2 && c[1].value == 0xC9, "two Game Genie codes");
   rc = cheat_gb_compile("003+7E8+B3A", c, 16, &n, &kind);
   CHECK(rc == CE_OK && n == 1 && c[0].value == 0x00 && c[0].address == 0x737E &&
         c[0].has_compare, "'+' for '-': %u codes, %04X", n, c[0].address);
   rc = cheat_gb_compile("3E8-9A9+2AA+078-9B9-D5E+E08-9C9-2A9", c, 16, &n, &kind);
   CHECK(rc == CE_OK && n == 3 && c[0].has_compare && c[1].value == 0x07 &&
         c[2].value == 0xE0, "mixed spelling: %u codes", n);
   rc = cheat_gb_compile("11B-E6A+100-7FB-4C7+410-80B-E63", c, 16, &n, &kind);
   CHECK(rc == CE_OK && n == 3 && !c[0].has_compare && c[1].has_compare,
         "short then long: %u codes", n);
   rc = cheat_gb_compile("459-84F-E6A+CA9-85F-E69+8DF-E6A", c, 16, &n, &kind);
   CHECK(rc == CE_OK && n == 3 && !c[2].has_compare, "long, long, short: %u", n);
   rc = cheat_gb_compile("01FF16D0+993-00F-A2E", c, 16, &n, &kind);
   CHECK(rc == CE_OK && n == 2 && kind == CE_KIND_GB_GAMEGENIE && !c[0].rom && c[1].rom,
         "GameShark and Game Genie in one entry");
   rc = cheat_gb_compile("01FF16D0+01FF17D0", c, 1, &n, &kind);
   CHECK(rc == CE_E_TOO_LONG, "two codes into one: rc %d", rc);
   rc = cheat_gb_compile("01FF16D0", NULL, 0, &n, &kind);
   CHECK(rc == CE_OK && n == 1, "validate without output");

   for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      rc = cheat_gb_compile(bad[i].code, c, 16, &n, &kind);
      CHECK(rc == bad[i].rc, "\"%s\": rc %d, want %d", bad[i].code, rc, bad[i].rc);
   }
}

int main(void)
{
   int suite;
   printf("mGBA's cheat test-suite\n");
   suite = mgba_suite_main();
   test_ciphers();
   test_published();
   test_vectors();
   test_rom_and_errors();
   test_seeds();
   test_gb();
   printf("test_engine: %d checks, %d failed; mGBA suite: %d failed\n", checks,
          failures, suite);
   return failures || suite ? 1 : 0;
}
