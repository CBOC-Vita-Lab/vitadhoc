/* GPSP_CHEATS (docs/CHEATS.md): the strict parser, the RAM-only accessors,
 * the invalidation trigger (cheats.c) and the cheat-file reader, the per-game
 * enable state and the Game Boy codes (frontend-common/fe_cheats.c).
 *
 * Built with -DGPSP_CHEATS=1 -DHAVE_DYNAREC against the real cheats.c and
 * fe_cheats.c under ASan/UBSan; RAM, the tag planes and the flush are this
 * file's, so an out-of-bounds host write is a sanitizer failure.  What it
 * pins down:
 *   - which code shapes parse, and which are refused and WHY (GameShark/AR,
 *     ROM/IO/VRAM/save addresses, misalignment, encrypted CB, mixed formats);
 *   - raw 8/16/32-bit writes land little-endian where they should;
 *   - CodeBreaker conditionals, slide and super codes still behave, and a
 *     slide that walks out of RAM writes nothing outside it;
 *   - a write onto TAGGED RAM (translated code) requests exactly one flush
 *     per frame, a write of the SAME value none, untagged RAM none;
 *   - the pool and slot limits fail loudly, per cheat;
 *   - suspended (a link session) => nothing runs;
 *   - cheat files: the plain list (CRLF, BOM, comments, headings, brackets,
 *     duplicate names, unnamed codes), libretro .cht, files larger than the
 *     buffer, more entries than the list, NUL and high bytes;
 *   - the .on state round trip, keyed by hash (order- and format-proof);
 *   - Game Boy GameShark: 01 RAM codes only, never cartridge RAM;
 *   - FUZZ_ROUNDS rounds of random and random-but-well-formed input. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common.h"
#include "../cheats.h"
#include "../gba_memory.h"
#include "../frontend-common/fe_cheats.h"

u8 ewram[1024 * 256 * 2];
u8 iwram[1024 * 32 * 2];
u16 io_registers[512];
#ifdef REWIND
u8 gba_rewind_clean[RW_CLEAN_BYTES];
extern u32 gba_rewind_cheat_gen;
#endif
int dynarec_enable = 1;
static unsigned flushes;
void flush_translation_cache_ram(void) { flushes++; }

static u32 ew32(u32 off) { return readaddress32(ewram, off); }
static u16 ew16(u32 off) { return readaddress16(ewram, off); }
static u16 iw16(u32 off) { return readaddress16(iwram + 0x8000, off); }

static void reset(void)
{
   memset(ewram, 0, sizeof(ewram));
   memset(iwram, 0, sizeof(iwram));
   memset(io_registers, 0, sizeof(io_registers));
   io_registers[REG_P1] = 0x3FF;           /* nothing pressed */
   fe_cheats_lock(0);
   fe_cheats_reset();
   cheat_clear();
   flushes = 0;
}

/* Exactly retro_run's hook. */
static void frame(void)
{
   if (cheats_active && !cheats_suspended)
      cheats_apply_frame();
}

#ifndef FUZZ_ROUNDS
#define FUZZ_ROUNDS 150000
#endif

static uint32_t rng_state = 0x1234ABCDu;
static uint32_t rnd(void)
{
   rng_state ^= rng_state << 13;
   rng_state ^= rng_state >> 17;
   rng_state ^= rng_state << 5;
   return rng_state;
}

/* ---- Game Boy: a 64 KiB address space the pokes land in ------------------ */
static uint8_t gb_mem[0x10000];
static unsigned gb_refused;
static int gb_poke(void *ctx, uint16_t addr, uint8_t v)
{
   (void)ctx;
   /* The real gbcore_poke refuses ROM and I/O; fe_cheats must never ask. */
   if (addr < 0xC000 || (addr > 0xDFFF && addr < 0xFF80) || addr == 0xFFFF)
   {
      gb_refused++;
      return -1;
   }
   gb_mem[addr] = v;
   return 0;
}

static void test_core(void)
{
   unsigned i;

   /* ---- shapes that parse ---------------------------------------------- */
   reset();
   assert(cheat_parse(0, "82000010 BEEF") == CheatNoError);        /* CB 16 */
   assert(cheat_parse(1, "320000200012") == CheatNoError);         /* CB, 12 run */
   assert(cheat_parse(2, "02000030:12345678") == CheatNoError);    /* raw 32 */
   assert(cheat_parse(3, "02000040:AB+02000042:CDEF") == CheatNoError);
   assert(cheat_parse(4, "  82000050 0001\r\n\t82000052 0002 ; 82000054 0003,")
          == CheatNoError);                                         /* layout */
   assert(cheats_active == 5);
   frame();
   assert(ew16(0x10) == 0xBEEF);
   assert(ewram[0x20] == 0x12);
   assert(ew32(0x30) == 0x12345678);
   assert(ewram[0x40] == 0xAB && ew16(0x42) == 0xCDEF);
   assert(ew16(0x50) == 1 && ew16(0x52) == 2 && ew16(0x54) == 3);
   assert(flushes == 0);                     /* nothing was translated */

   /* ---- shapes that are refused, each with its reason ---------------- */
   reset();
   assert(cheat_parse(0, "12345678 9ABCDEF0") == CheatErrorGameShark);
   assert(cheat_parse(0, "123456789ABCDEF0") == CheatErrorGameShark);
   assert(cheat_parse(0, "88000000 1234") == CheatErrorRegion);    /* ROM */
   assert(cheat_parse(0, "84000200 0001") == CheatErrorRegion);    /* IO */
   assert(cheat_parse(0, "85000000 0001") == CheatErrorRegion);    /* palette */
   assert(cheat_parse(0, "86000000 0001") == CheatErrorRegion);    /* VRAM */
   assert(cheat_parse(0, "87000000 0001") == CheatErrorRegion);    /* OAM */
   assert(cheat_parse(0, "8D000000 0001") == CheatErrorRegion);    /* EEPROM */
   assert(cheat_parse(0, "3E000000 0001") == CheatErrorRegion);    /* SRAM */
   assert(cheat_parse(0, "80000000 0001") == CheatErrorRegion);    /* BIOS */
   assert(cheat_parse(0, "08000000:12") == CheatErrorRegion);      /* raw ROM */
   assert(cheat_parse(0, "0E000000:12") == CheatErrorRegion);      /* raw SRAM */
   assert(cheat_parse(0, "12000000:12") == CheatErrorRegion);      /* > 28 bit */
   assert(cheat_parse(0, "72000000 0001\n38000000 0001") == CheatErrorRegion);
   assert(cheat_parse(0, "78000000 0001\n32000000 0001") == CheatErrorRegion);
   assert(cheat_parse(0, "82000001 0001") == CheatErrorAlign);
   assert(cheat_parse(0, "02000002:12345678") == CheatErrorAlign);
   assert(cheat_parse(0, "02000001:1234") == CheatErrorAlign);
   assert(cheat_parse(0, "E2000003 0001") == CheatErrorAlign);     /* 32-bit add */
   assert(cheat_parse(0, "9ABCDEF0 1234") == CheatErrorEncrypted);
   assert(cheat_parse(0, "82000010 0001+02000000:01") == CheatErrorMixed);
   assert(cheat_parse(0, "02000000:01 82000010 0001") == CheatErrorMixed);
   assert(cheat_parse(0, "hello") == CheatErrorSyntax);
   assert(cheat_parse(0, "82000010") == CheatErrorSyntax);
   assert(cheat_parse(0, "82000010 123") == CheatErrorSyntax);
   assert(cheat_parse(0, "82000010 12345") == CheatErrorSyntax);
   assert(cheat_parse(0, "02000000:123") == CheatErrorSyntax);
   assert(cheat_parse(0, "02000000:") == CheatErrorSyntax);
   assert(cheat_parse(0, ":12") == CheatErrorSyntax);
   assert(cheat_parse(0, "0200000000:12") == CheatErrorSyntax);
   assert(cheat_parse(0, "") == CheatErrorSyntax);
   assert(cheat_parse(0, " \r\n+;,") == CheatErrorSyntax);
   assert(cheat_parse(0, NULL) == CheatErrorSyntax);
   assert(cheat_parse(0, "50001000 0002\n11223344 5566") == CheatErrorRegion);
   assert(cheat_parse(0, "52001000 0004\n12345678 9abc") == CheatErrorNotSupported);
   assert(cheat_parse(0, "42000300 0010") == CheatErrorNotSupported);
   assert(cheat_parse(MAX_CHEATS, "82000010 0001") == CheatErrorTooMany);
   assert(cheat_parse(0xFFFFFFFFu, "82000010 0001") == CheatErrorTooMany);
   assert(cheats_active == 0);
   frame();
   assert(flushes == 0);
   /* cheat_check agrees with cheat_parse and installs nothing. */
   assert(cheat_check("82000010 0001") == CheatNoError);
   assert(cheat_check("12345678 9ABCDEF0") == CheatErrorGameShark);
   assert(cheat_check("3E000000 0001") == CheatErrorRegion);
   assert(cheats_active == 0);

   /* A refused replacement disables what the slot held before. */
   assert(cheat_parse(0, "82000010 0001") == CheatNoError);
   assert(cheats_active == 1);
   assert(cheat_parse(0, "88000000 0001") == CheatErrorRegion);
   assert(cheats_active == 0);
   frame();
   assert(ew16(0x10) == 0);

   /* ---- master/hook and game-id records: accepted, never installed ---- */
   reset();
   assert(cheat_parse(0, "0000ABCD 000A\n1008B3B8 0007\n82000010 0042") ==
          CheatNoError);
   assert(cheat_master_hook == 0xFFFFFFFFu);
   frame();
   assert(ew16(0x10) == 0x42);

   /* ---- CodeBreaker semantics through the RAM accessors ---------------- */
   reset();
   assert(cheat_parse(0, "72000100 0005\n82000102 0077") == CheatNoError);
   frame();
   assert(ew16(0x102) == 0);
   address16(ewram, 0x100) = eswap16(5);
   frame();
   assert(ew16(0x102) == 0x77);
   /* 2 / 6: 16-bit OR and AND. */
   reset();
   address16(ewram, 0x110) = eswap16(0x0F0F);
   address16(ewram, 0x112) = eswap16(0x0F0F);
   assert(cheat_parse(0, "22000110 F000\n62000112 00FF") == CheatNoError);
   frame();
   assert(ew16(0x110) == 0xFF0F && ew16(0x112) == 0x000F);
   /* A / B / C / F: skip the next code when equal / <= / >= / no bit set. */
   reset();
   address16(ewram, 0x120) = eswap16(7);
   assert(cheat_parse(0, "A2000120 0007\n82000130 0001\n"     /* 7 == 7: skip */
                         "A2000120 0008\n82000132 0001\n"     /* run */
                         "B2000120 0007\n82000134 0001\n"     /* 7 <= 7: skip */
                         "B2000120 0006\n82000136 0001\n"     /* run */
                         "C2000120 0007\n82000138 0001\n"     /* 7 >= 7: skip */
                         "C2000120 0008\n8200013A 0001\n"     /* run */
                         "F2000120 0008\n8200013C 0001\n"     /* 7 & 8 == 0: skip */
                         "F2000120 0004\n8200013E 0001") == CheatNoError);
   frame();
   assert(ew16(0x130) == 0 && ew16(0x132) == 1 && ew16(0x134) == 0 &&
          ew16(0x136) == 1 && ew16(0x138) == 0 && ew16(0x13A) == 1 &&
          ew16(0x13C) == 0 && ew16(0x13E) == 1);
   /* D: button test gates the next code. */
   reset();
   assert(cheat_parse(0, "D0000020 0001\n82000200 0099") == CheatNoError);
   frame();
   assert(ew16(0x200) == 0);
   io_registers[REG_P1] = 0x3FF & ~1;         /* A pressed */
   frame();
   assert(ew16(0x200) == 0x99);
   /* 4: slide -- 3 writes, value +1, address +2. */
   reset();
   assert(cheat_parse(0, "42000300 0010\n00010003 0002") == CheatNoError);
   frame();
   assert(ew16(0x300) == 0x10 && ew16(0x302) == 0x11 && ew16(0x304) == 0x12 &&
          ew16(0x306) == 0);
   /* A slide that walks out of RAM drops those writes and counts them. */
   reset();
   cheat_writes_dropped = 0;
   assert(cheat_parse(0, "43FF0000 0001\n00000004 8000") == CheatNoError);
   frame();
   assert(cheat_writes_dropped == 2);   /* 0x04000000 and 0x04008000 */
   /* The biggest slide there is: 65535 writes, stride 0xFFFF.  It wraps the
    * 28-bit address all over the map; only RAM takes any of it. */
   reset();
   cheat_writes_dropped = 0;
   /* since 4.0.0 beta (review C10) a slide count above 4096 does not
    * validate; the largest accepted one is run instead */
   assert(cheat_parse(0, "42000000 ABCD\n0000FFFF FFFE") == CheatErrorNotSupported);
   assert(cheat_parse(0, "42000000 ABCD\n00001001 FFFE") == CheatErrorNotSupported);
   assert(cheat_parse(0, "42000000 ABCD\n00001000 FFFE") == CheatNoError);
   frame();
   assert(cheat_writes_dropped > 0);
   /* E: 32-bit add (bit 0 set), sign-extended. */
   reset();
   address32(ewram, 0x400) = eswap32(0x00010000);
   assert(cheat_parse(0, "E2000401 FFFF") == CheatNoError);
   frame();
   assert(ew32(0x400) == 0x0000FFFF);
   /* 5: super code -- 4 bytes from the payload. */
   reset();
   assert(cheat_parse(0, "53000010 0002\n11223344 5566") == CheatNoError);
   frame();
   assert(iw16(0x10) == 0x2211 && iw16(0x12) == 0x4433);
   /* The last word of each region, at every width: nothing past the end. */
   reset();
   assert(cheat_parse(0, "0203FFFC:11223344 0203FFFF:55") == CheatNoError);
   assert(cheat_parse(1, "03007FFC:11223344 03007FFE:6677") == CheatNoError);
   assert(cheat_parse(2, "E203FFFD 0001") == CheatNoError);   /* add32 at FFFC */
   frame();
   assert(ewram[0x3FFFF] == 0x55 && iw16(0x7FFE) == 0x6677);
   /* Mirrors are masked into the array, never past it. */
   reset();
   assert(cheat_parse(0, "02FFFFFE:ABCD 03FFFFFC:11223344") == CheatNoError);
   frame();
   assert(ew16(0x3FFFE) == 0xABCD && iw16(0x7FFC) == 0x3344);

   /* ---- the invalidation trigger --------------------------------------- */
   reset();
   /* Pretend the dynarec translated IWRAM 0x03000100..0x0300010F. */
   memset(iwram + 0x100, 0x01, 0x10);
   assert(cheat_parse(0, "03000108:E2811002") == CheatNoError);
   frame();
   assert(readaddress32(iwram + 0x8000, 0x108) == 0xE2811002);
   assert(flushes == 1);
   frame();                                  /* same value: no write, no flush */
   assert(flushes == 1);
   /* The game puts the old code back, the cheat re-patches: one more. */
   address32(iwram + 0x8000, 0x108) = eswap32(0xE2811001);
   frame();
   assert(flushes == 2);
   /* The same at 8 and 16 bits: one flush when the bytes change, none while
    * they already hold the value. */
   reset();
   memset(ewram + 0x40000 + 0x700, 0x01, 0x10);
   assert(cheat_parse(0, "32000700 00AA") == CheatNoError);
   assert(cheat_parse(1, "82000704 BBCC") == CheatNoError);
   frame();
   assert(flushes == 1 && ewram[0x700] == 0xAA && ew16(0x704) == 0xBBCC);
   frame();
   frame();
   assert(flushes == 1);
   ewram[0x700] = 0;                         /* the game rewrote one byte */
   frame();
   assert(flushes == 2);
   address16(ewram, 0x704) = 0;
   frame();
   assert(flushes == 3);
   /* Several codes hitting translated bytes in one frame: still ONE flush. */
   reset();
   memset(ewram + 0x40000 + 0x500, 0x01, 0x20);
   assert(cheat_parse(0, "82000500 0001") == CheatNoError);
   assert(cheat_parse(1, "82000510 0001") == CheatNoError);
   assert(cheat_parse(2, "32000518 0001") == CheatNoError);
   frame();
   assert(flushes == 1);
   /* A write next to (not on) translated bytes: no flush. */
   reset();
   memset(ewram + 0x40000 + 0x500, 0x01, 0x10);
   assert(cheat_parse(0, "82000510 0001") == CheatNoError);
   frame();
   assert(flushes == 0);
   /* Interpreter: no translation cache, no flush. */
   reset();
   dynarec_enable = 0;
   memset(iwram + 0x100, 0x01, 0x10);
   assert(cheat_parse(0, "03000108:E2811002") == CheatNoError);
   frame();
   assert(flushes == 0);
   dynarec_enable = 1;

   /* ---- suspended: a link session ------------------------------------- */
   reset();
   assert(cheat_parse(0, "82000010 0042") == CheatNoError);
   cheats_set_suspended(1);
   frame();
   assert(ew16(0x10) == 0);
   cheats_set_suspended(0);
   frame();
   assert(ew16(0x10) == 0x42);

   /* ---- disable, clear ------------------------------------------------- */
   reset();
   assert(cheat_parse(0, "82000010 0042") == CheatNoError);
   assert(cheat_parse(1, "82000012 0043") == CheatNoError);
   cheat_disable(0);
   assert(cheats_active == 1);
   frame();
   assert(ew16(0x10) == 0 && ew16(0x12) == 0x43);
   cheat_clear();
   assert(cheats_active == 0);

   /* ---- the limits fail loudly ----------------------------------------- */
   reset();
   for (i = 0; i < MAX_CHEATS; i++)
      assert(cheat_parse(i, "82000010 0001") == CheatNoError);
   assert(cheats_active == MAX_CHEATS);
   assert(cheat_parse(MAX_CHEATS, "82000010 0001") == CheatErrorTooMany);
   {
      /* One cheat longer than the whole pool. */
      static char big[(CHEAT_POOL_RECORDS + 8) * 14 + 1];
      char *w = big;
      reset();
      for (i = 0; i < CHEAT_POOL_RECORDS + 8; i++)
         w += sprintf(w, "82000010 %04X\n", i & 0xFFFF);
      assert(cheat_parse(0, big) == CheatErrorTooBig);
      assert(cheats_active == 0);
      /* ...and exactly the pool fits; then the next cheat does not. */
      big[CHEAT_POOL_RECORDS * 14] = '\0';
      assert(cheat_parse(0, big) == CheatNoError);
      assert(cheat_parse(1, "82000010 0001") == CheatErrorTooBig);
      assert(cheat_check("82000010 0001") == CheatErrorTooBig);
      assert(cheats_active == 1);
      frame();
      cheat_clear();                       /* the pool comes back */
      assert(cheat_parse(1, "82000010 0001") == CheatNoError);
   }
}

/* Frozen vectors produced independently using mGBA's documented TEA keys
 * and DEADFACE table reseeding. The public D8BAE4D9 4864DCE5 AR3 master
 * decodes to C40005EC 00008401 and must be refused, never ignored. */
static void test_gs_cipher(void)
{
   static const char *const vectors[] = {
      "GS1: DF4FF124 9EB1FD0E+93B16540 389E4302+DD4EC3B1 CBA7D882",
      "AR3: 00AE5A50 2CA38934+6B60C2F8 35421A85+23137AA8 E43614B1",
      "GS1: 70BDB80D 69F37FCB+D28077B2 3E18D0B4+69104A15 CF0229DF+"
            "B365B7AD 35A7B5E5",
      "AR3: 9C7FE77C A5352DCA+B78EBC6F 5DA816AE+DBF4AFC2 1AB549DA+"
            "A33AD5AE 4A9748B1"
   };
   unsigned i;
   for (i = 0; i < sizeof(vectors) / sizeof(*vectors); i++)
   {
      reset();
      assert(cheat_check(vectors[i]) == CheatNoError && !cheats_active);
      assert(cheat_parse(0, vectors[i]) == CheatNoError);
      frame();
      assert(ewram[0x10] == 0xAB && ew16(0x12) == 0xCDEF &&
             ew32(0x14) == 0x12345678);
      /* A prior entry's reseed must not affect another parse/check. */
      assert(cheat_check(vectors[i & 1]) == CheatNoError);
      assert(cheat_parse(1, vectors[i & 1]) == CheatNoError);
   }
   assert(cheat_check("AR3: D8BAE4D9 4864DCE5") == CheatErrorUnsupported);
   assert(cheat_check("D8BAE4D9 4864DCE5") == CheatErrorGameShark);
   assert(cheat_check("GS1RAW: DEADFACE 00001234+02000010 000000AB") == CheatNoError);
   assert(cheat_check("AR3RAW: DEADFACE 00001234+00200010 000000AB") == CheatNoError);
   assert(cheat_check("AR3RAW: DEADFACE 00011234+00200010 000000AB") == CheatErrorUnsupported);
   assert(cheat_check("GS1RAW: DEADFACE 00001234") == CheatErrorSyntax);
}

static void test_gs_operations(void)
{
   unsigned i, w, cond;
   char code[256];
   reset();
   assert(cheat_parse(0, " gs1raw:02000011 000000AB;12000012 0000CDEF,"
                         "23007FFC89ABCDEF") == CheatNoError);
   assert(cheat_parse(1, "AR3RAW:00200020 000002CC+02200024 0001BEEF+"
                         "04307FF8 12345678") == CheatNoError);
   ewram[0x40000 + 0x12] = 1;
   iwram[0x7FFF] = 1;
   frame();
   assert(ewram[0x11] == 0xAB && ew16(0x12) == 0xCDEF);
   assert(ewram[0x20] == 0xCC && ewram[0x21] == 0xCC && ewram[0x22] == 0xCC);
   assert(ewram[0x23] == 0 && ew16(0x24) == 0xBEEF && ew16(0x26) == 0xBEEF);
   assert(readaddress32(iwram + 0x8000, 0x7FF8) == 0x12345678);
   assert(readaddress32(iwram + 0x8000, 0x7FFC) == 0x89ABCDEF);
   assert(flushes == 1);
   frame();
   assert(flushes == 1); /* unchanged tagged writes never flush twice */
   cheats_set_suspended(1);
   ewram[0x11] = 0;
   cheats_apply_frame(); /* guard also works when called directly */
   assert(ewram[0x11] == 0 && flushes == 1);
   cheats_set_suspended(0);
   frame();
   assert(ewram[0x11] == 0xAB);

   /* All four GS1 16-bit predicates, below/equal/above their threshold. */
   for (cond = 0; cond < 4; cond++)
      for (i = 6; i <= 8; i++)
      {
         int pass = cond == 0 ? i == 7 : cond == 1 ? i != 7 :
                    cond == 2 ? i <= 7 : i >= 7;
         reset();
         address16(ewram, 0x100) = eswap16(i);
         snprintf(code, sizeof(code), "GS1RAW:D2000100 %08X+22000200 12345678",
                  (cond << 20) | 7);
         assert(cheat_parse(0, code) == CheatNoError);
         frame();
         assert(ew32(0x200) == (pass ? 0x12345678u : 0));
      }
   /* AR3 equality/inequality/unsigned less/greater/bit-test, every width. */
   for (w = 0; w < 3; w++)
      for (cond = 1; cond <= 7; cond++)
      {
         if (cond == 3 || cond == 4) continue; /* unsupported signed forms */
         for (i = 6; i <= 8; i++)
         {
            int pass = cond == 1 ? i == 7 : cond == 2 ? i != 7 :
                       cond == 5 ? i < 7 : cond == 6 ? i > 7 : (i & 7) != 0;
            reset();
            address32(ewram, 0x100) = eswap32(i);
            snprintf(code, sizeof(code), "AR3RAW:%08X 00000007+04200200 12345678",
                     (cond << 27) | (w << 25) | 0x00200100u);
            assert(cheat_parse(0, code) == CheatNoError);
            frame();
            assert(ew32(0x200) == (pass ? 0x12345678u : 0));
         }
      }
   /* False next-two skips a whole fill including its private payload. */
   reset();
   assert(cheat_parse(0, "AR3RAW:4A200100 00000001+00200200 000003AA+"
                         "04200204 12345678+00200208 000000BB") == CheatNoError);
   frame();
   assert(ew32(0x200) == 0 && ew32(0x204) == 0 && ewram[0x208] == 0xBB);
   address16(ewram, 0x100) = eswap16(1);
   frame();
   assert(ew32(0x200) == 0xAAAAAAAAu && ew32(0x204) == 0x12345678);
   /* Reseed metadata is not counted as a target operation. */
   reset();
   assert(cheat_parse(0, "GS1RAW:D2000100 00000001+DEADFACE 00001234+"
                         "22000200 12345678") == CheatNoError);
   frame();
   assert(ew32(0x200) == 0);

#ifdef REWIND
   {
      u32 generation = gba_rewind_cheat_gen;
      assert(cheat_check("GS1RAW:02000010 00000001") == CheatNoError);
      assert(gba_rewind_cheat_gen == generation);
      assert(cheat_parse(1, "GS1RAW:02000010 00000001") == CheatNoError);
      assert(gba_rewind_cheat_gen != generation);
      generation = gba_rewind_cheat_gen;
      cheats_set_suspended(1);
      assert(gba_rewind_cheat_gen != generation);
      generation = gba_rewind_cheat_gen;
      cheats_set_suspended(1);
      assert(gba_rewind_cheat_gen == generation);
      cheats_set_suspended(0);
      memset(gba_rewind_clean, 1, sizeof(gba_rewind_clean));
      frame();
      assert(gba_rewind_clean[RW_CLEAN_EWRAM] == 0);
   }
#endif
}

static void test_gs_refusals(void)
{
   static const struct { const char *s; cheat_error error; } bad[] = {
      { "GS1:", CheatErrorSyntax },
      { "GS1RAW:02000010 0000000G", CheatErrorSyntax },
      { "GS1RAW:02000010 000000AB garbage", CheatErrorSyntax },
      { "GS1RAW:02000010 000000AB+AR3RAW:00200010 000000AB", CheatErrorMixed },
      { "GS1RAW:82000010 0001", CheatErrorMixed },
      { "AR3RAW:02000010:AB", CheatErrorMixed },
      { "GS1RAW:02000010 00000100", CheatErrorSyntax },
      { "GS1RAW:12000010 00010000", CheatErrorSyntax },
      { "GS1RAW:12000011 00000001", CheatErrorAlign },
      { "GS1RAW:22000012 00000001", CheatErrorAlign },
      { "AR3RAW:02200011 00000001", CheatErrorAlign },
      { "AR3RAW:04200012 00000001", CheatErrorAlign },
      { "GS1RAW:06000010 00000001", CheatErrorRegion },
      { "GS1RAW:0E000010 00000001", CheatErrorRegion },
      { "GS1RAW:D8000010 00000001+02000010 00000001", CheatErrorRegion },
      { "AR3RAW:04800010 00000001", CheatErrorRegion },
      { "AR3RAW:04E00010 00000001", CheatErrorRegion },
      { "AR3RAW:0C400010 00000001+00200010 00000001", CheatErrorRegion },
      { "GS1RAW:60000000 00000001", CheatErrorUnsupported },
      { "GS1RAW:F8000000 00000001", CheatErrorUnsupported },
      { "GS1RAW:30000002 02000010", CheatErrorUnsupported },
      { "GS1RAW:82000010 00000001", CheatErrorUnsupported },
      { "GS1RAW:D2000010 00400001", CheatErrorUnsupported },
      { "GS1RAW:D2000010 00000001", CheatErrorNotSupported },
      { "AR3RAW:4A200010 00000001+00200020 00000001", CheatErrorNotSupported },
      { "AR3RAW:00000000 80000010", CheatErrorUnsupported },
      { "AR3RAW:00000000 40000000", CheatErrorUnsupported },
      { "AR3RAW:C4000000 00000001", CheatErrorUnsupported },
      { "AR3RAW:C6000000 00000001", CheatErrorUnsupported },
      { "AR3RAW:44200010 00000001", CheatErrorUnsupported },
      { "AR3RAW:84200010 00000001", CheatErrorUnsupported },
      { "AR3RAW:8A200010 00000001", CheatErrorUnsupported },
      { "AR3RAW:CA200010 00000001", CheatErrorUnsupported },
      { "AR3RAW:1A200010 00000001", CheatErrorUnsupported },
      { "AR3RAW:06200010 00000001", CheatErrorUnsupported },
      { "AR3RAW:01200010 00000001", CheatErrorUnsupported },
      { "AR3RAW:04200010 001DC0DE", CheatErrorUnsupported },
      { "AR3RAW:08200010 00000100+00200020 00000001", CheatErrorSyntax },
      { "AR3RAW:00200010 00100001", CheatErrorWork }
   };
   unsigned i;
   char code[256];
   for (i = 0; i < sizeof(bad) / sizeof(*bad); i++)
   {
      reset();
      assert(cheat_check(bad[i].s) == bad[i].error);
      assert(cheat_parse(0, bad[i].s) == bad[i].error);
      assert(cheats_active == 0);
      frame();
      assert(ew32(0x10) == 0 && flushes == 0);
   }
   /* A good first operation does not survive an unsupported tail. */
   reset();
   assert(cheat_parse(0, "GS1RAW:02000010 000000AB") == CheatNoError);
   assert(cheat_parse(0, "GS1RAW:02000010 000000CD+F8000000 00000000")
          == CheatErrorUnsupported);
   assert(!cheats_active);
   frame();
   assert(ewram[0x10] == 0 && cheat_master_hook == 0xFFFFFFFFu);
   /* Every disallowed region is rejected for every assignment width. */
   for (i = 0; i < 16; i++)
   {
      if (i == 2 || i == 3) continue;
      snprintf(code, sizeof(code), "GS1RAW:%08X 00000001", 0x20000010u | (i << 24));
      assert(cheat_check(code) == CheatErrorRegion);
      snprintf(code, sizeof(code), "AR3RAW:%08X 00000001", 0x04000010u | (i << 20));
      assert(cheat_check(code) == CheatErrorRegion);
   }
}

static void test_cheat_work_budget(void)
{
   reset();
   assert(cheat_parse(0, "AR3RAW:00200000 000FFF5A") == CheatNoError);
   assert(cheat_parse(1, "GS1RAW:02002000 00000001") == CheatErrorWork);
   assert(cheat_parse(1, "82002000 0001") == CheatErrorWork);
   frame();
   assert(ewram[0] == 0x5A && ewram[4095] == 0x5A && ewram[4096] == 0);
   reset();
   assert(cheat_parse(0, "AR3RAW:00200000 0008005A+00202000 0008005A") == CheatErrorWork);
   assert(!cheats_active);
   frame();
   assert(ewram[0] == 0);
   assert(cheat_parse(0, "GS1RAW:02000000 00000001") == CheatNoError);
   reset();
   assert(cheat_parse(0, "42000000 0001+00000801 0002+42002000 0001+00000801 0002")
          == CheatErrorWork);
   assert(!cheats_active);
   assert(cheat_parse(0, "42000000 0001+00001000 0002") == CheatNoError);
   assert(cheat_parse(1, "AR3RAW:00203000 00000001") == CheatErrorWork);
   cheat_clear();
   assert(cheat_parse(0, "AR3RAW:00203000 00000001") == CheatNoError);
   /* GS records compete for exactly the same bounded pool as CB. */
   {
      static char full[(CHEAT_POOL_RECORDS + 1) * 18 + 16];
      char *p = full;
      unsigned i;
      reset();
      p += sprintf(p, "GS1RAW:");
      for (i = 0; i <= CHEAT_POOL_RECORDS; i++)
         p += sprintf(p, "02000010 00000001+");
      assert(cheat_parse(0, full) == CheatErrorTooBig && !cheats_active);
      full[7 + CHEAT_POOL_RECORDS * 18] = '\0';
      assert(cheat_parse(0, full) == CheatNoError);
      assert(cheat_parse(1, "82000010 0001") == CheatErrorTooBig);
   }
}

static void test_gs_files(void)
{
   static const char plain[] =
      "[GS first]\nGS1RAW:02000010 000000AB\n12000012 0000CDEF\n"
      "[AR next]\nar3raw:\n04200014 12345678\n"
      "[Malformed]\nGS1RAW: garbage\n"
      "[Bad continuation]\nAR3RAW:00200020 00000001\n00200021 WRONG\n"
      "[Old ambiguous]\n12345678 9ABCDEF0\n";
   static const char cht[] =
      "cheats = 2\ncheat0_desc = \"Encrypted\"\n"
      "cheat0_code = \"AR3:00AE5A50 2CA38934+23137AA8 E43614B1\"\n"
      "cheat0_enable = true\ncheat1_desc = \"Bad\"\n"
      "cheat1_code = \"GS1RAW: nonsense\"\ncheat1_enable = true\n";
   char state[FE_CHEATS_STATE_MAX];
   size_t n;
   reset();
   fe_cheats_load_text(plain, sizeof(plain) - 1, FE_CHEATS_SYS_GBA);
   assert(fe_cheats_count() == 5 && fe_cheats_usable_count() == 2);
   assert(fe_cheats_get(2)->error == CheatErrorSyntax);
   assert(fe_cheats_get(3)->error == CheatErrorSyntax);
   assert(fe_cheats_get(4)->error == CheatErrorGameShark);
   assert(!fe_cheats_enabled_count() && !cheats_active);
   fe_cheats_attach(1);
   assert(fe_cheats_set(0, 1) == 0 && fe_cheats_set(1, 1) == 0);
   assert(fe_cheats_set(2, 1) == CheatErrorSyntax);
   assert(fe_cheats_set(3, 1) == CheatErrorSyntax);
   frame();
   assert(ewram[0x10] == 0xAB && ew16(0x12) == 0xCDEF && ew32(0x14) == 0x12345678);
   assert(ewram[0x20] == 0);
   fe_cheats_lock(1);
   ewram[0x10] = 0;
   frame();
   assert(!ewram[0x10] && fe_cheats_enabled_count() == 2);
   fe_cheats_lock(0);
   frame();
   assert(ewram[0x10] == 0xAB);
   n = fe_cheats_state_serialise(state, sizeof(state));
   assert(n < sizeof(state));
   fe_cheats_load_text(plain, sizeof(plain) - 1, FE_CHEATS_SYS_GBA);
   assert(!fe_cheats_enabled_count());
   fe_cheats_state_parse(state, n);
   assert(fe_cheats_enabled_count() == 2);
   /* Changing the family is a changed identity, even with identical digits. */
   {
      static const char changed[] = "[GS first]\nAR3RAW:02000010 000000AB\n12000012 0000CDEF\n";
      fe_cheats_load_text(changed, sizeof(changed) - 1, FE_CHEATS_SYS_GBA);
      fe_cheats_state_parse(state, n);
      assert(!fe_cheats_enabled_count());
   }
   fe_cheats_load_text(cht, sizeof(cht) - 1, FE_CHEATS_SYS_GBA);
   assert(fe_cheats_count() == 2 && fe_cheats_usable_count() == 1);
   assert(!fe_cheats_enabled_count() && !cheats_active);
   assert(fe_cheats_get(1)->error == CheatErrorSyntax);
   assert(fe_cheats_set(0, 1) == 0);
   frame();
   assert(ewram[0x10] == 0xAB && ew32(0x14) == 0x12345678);
}

static const char PLAIN[] =
   "\xEF\xBB\xBF# Pokemon Ruby (USA) -- a test list\r\n"
   "\r\n"
   "Pokemon Ruby cheats\r\n"                 /* a heading: no codes under it */
   "Infinite money\r\n"
   "82025BC4 E0FF\r\n"
   "// a comment between two code lines\r\n"
   "82025BC6 05F5\r\n"
   "\r\n"
   "[DEADBEEF]\r\n"                          /* a name that looks like a code */
   "02000010:FF\r\n"
   "Raw and wide\r\n"
   "   02000020:1234   02000024:89ABCDEF\r\n"
   "GameShark thing\r\n"
   "12345678 9ABCDEF0\r\n"
   "ROM patch\r\n"
   "88000000 1234\r\n"
   "Infinite money\r\n"                      /* duplicate NAME, other code */
   "82025BC4 FFFF\r\n"
   "Infinite money\r\n"                      /* exact duplicate of the first */
   "82025BC4 E0FF 82025BC6 05F5\r\n"
   "Caf\xC3\xA9 \x7F\x80 bell\x07\r\n"       /* UTF-8, glyph bytes, control */
   "82000030 0001\r\n"
   "No codes at the end\r\n";

static void test_plain(void)
{
   const fe_cheat *e;
   static char st[FE_CHEATS_STATE_MAX];
   size_t n;
   uint32_t h0, h2;

   reset();
   assert(fe_cheats_status() == FE_CHEATS_NONE && fe_cheats_count() == 0);
   assert(fe_cheats_load_text(PLAIN, sizeof(PLAIN) - 1, FE_CHEATS_SYS_GBA) ==
          FE_CHEATS_LOADED);
   assert(fe_cheats_count() == 8 && fe_cheats_found() == 8);
   assert(!fe_cheats_truncated());

   e = fe_cheats_get(0);
   assert(!strcmp(e->name, "Infinite money") && e->error == 0 && !e->enabled);
   assert(strstr(e->code, "82025BC4 E0FF") && strstr(e->code, "82025BC6 05F5"));
   assert(!strstr(e->code, "comment"));
   e = fe_cheats_get(1);
   assert(!strcmp(e->name, "DEADBEEF") && e->error == 0);
   e = fe_cheats_get(2);
   assert(!strcmp(e->name, "Raw and wide") && e->error == 0);
   e = fe_cheats_get(3);
   assert(!strcmp(e->name, "GameShark thing") &&
          e->error == CheatErrorGameShark);
   e = fe_cheats_get(4);
   assert(!strcmp(e->name, "ROM patch") && e->error == CheatErrorRegion);
   e = fe_cheats_get(7);
   assert(!strcmp(e->name, "Caf? ? bell?") && e->error == 0);
   assert(fe_cheats_get(8) == NULL);
   assert(fe_cheats_usable_count() == 6);

   /* Duplicate names: different code => different identity; the same code
    * reformatted => the same identity. */
   h0 = fe_cheats_get(0)->hash;
   assert(fe_cheats_get(5)->hash != h0);
   assert(fe_cheats_get(6)->hash == h0);

   /* Nothing in a file switches anything on, and detached nothing runs. */
   assert(fe_cheats_enabled_count() == 0 && cheats_active == 0);
   assert(fe_cheats_set(0, 1) == 0);
   assert(fe_cheats_enabled_count() == 1 && cheats_active == 0);
   assert(fe_cheats_running() == 0);

   /* An entry with an error cannot be switched on, and says why. */
   assert(fe_cheats_set(3, 1) == CheatErrorGameShark);
   assert(!fe_cheats_get(3)->enabled);
   assert(fe_cheats_set(99, 1) == -1);

   /* Attach: the choices reach the core, and the frame hook writes. */
   fe_cheats_attach(1);
   assert(cheats_active == 1 && fe_cheats_running() == 1);
   frame();
   assert(ew32(0x25BC4) == 0x05F5E0FF);      /* 99,999,999 */
   assert(fe_cheats_set(2, 1) == 0);
   frame();
   assert(ew16(0x20) == 0x1234 && ew32(0x24) == 0x89ABCDEF);

   /* ---- the .on round trip --------------------------------------------- */
   h2 = fe_cheats_get(2)->hash;
   n = fe_cheats_state_serialise(st, sizeof(st));
   assert(n < sizeof(st) && n > 0);
   {
      char want[32];
      snprintf(want, sizeof(want), "%08lX Raw and wide\n", (unsigned long)h2);
      assert(memmem(st, n, want, strlen(want)));
   }
   /* Reload the file (everything off again), then the state. */
   assert(fe_cheats_load_text(PLAIN, sizeof(PLAIN) - 1, FE_CHEATS_SYS_GBA) ==
          FE_CHEATS_LOADED);
   assert(fe_cheats_enabled_count() == 0 && cheats_active == 0);
   fe_cheats_state_parse(st, n);
   /* 0 and its exact duplicate 6 share an identity, so both come back. */
   assert(fe_cheats_get(0)->enabled && fe_cheats_get(2)->enabled &&
          fe_cheats_get(6)->enabled && !fe_cheats_get(5)->enabled);
   assert(fe_cheats_enabled_count() == 3 && cheats_active == 3);

   /* The same choices survive the file being reordered and re-wrapped. */
   {
      static const char moved[] =
         "Raw and wide\n02000020:1234\n02000024:89abcdef\n"
         "New one\n82000040 0001\n"
         "Infinite money\n82025bc4 e0ff+82025bc6 05f5\n";
      assert(fe_cheats_load_text(moved, sizeof(moved) - 1,
                                 FE_CHEATS_SYS_GBA) == FE_CHEATS_LOADED);
      fe_cheats_state_parse(st, n);
      assert(fe_cheats_get(0)->enabled && !fe_cheats_get(1)->enabled &&
             fe_cheats_get(2)->enabled);
   }
   /* A state naming a cheat that is now unusable, garbage, or gone. */
   {
      char bad[160];
      assert(fe_cheats_load_text(PLAIN, sizeof(PLAIN) - 1,
                                 FE_CHEATS_SYS_GBA) == FE_CHEATS_LOADED);
      snprintf(bad, sizeof(bad), "%08lX GameShark thing\nzzzz\n\n1234\n"
               "FFFFFFFF gone\n%08lXjunk\n",
               (unsigned long)fe_cheats_get(3)->hash,
               (unsigned long)fe_cheats_get(1)->hash);
      fe_cheats_state_parse(bad, strlen(bad));
      assert(fe_cheats_enabled_count() == 0 && cheats_active == 0);
   }

   /* ---- all off, and the link lock ------------------------------------ */
   fe_cheats_state_parse(st, n);
   assert(cheats_active == 3);
   memset(ewram, 0, sizeof(ewram));
   fe_cheats_lock(1);
   assert(fe_cheats_locked() && fe_cheats_running() == 0);
   frame();
   assert(ew32(0x25BC4) == 0);               /* nothing ran */
   assert(fe_cheats_enabled_count() == 3);   /* the choices are kept */
   fe_cheats_lock(0);
   frame();
   assert(ew32(0x25BC4) == 0x05F5E0FF);
   fe_cheats_all_off();
   assert(fe_cheats_enabled_count() == 0 && cheats_active == 0);
   n = fe_cheats_state_serialise(st, sizeof(st));
   assert(n < sizeof(st));
   assert(fe_cheats_state_serialise(st, 8) == 8);     /* does not fit */

   /* Too many on at once: the 65th is refused, with the reason, and is off. */
   {
      static char many[80 * 24];
      char *w = many;
      unsigned i;
      for (i = 0; i < 80; i++)
         w += sprintf(w, "C%u\n82000%03X 0001\n", i, i * 2);
      assert(fe_cheats_load_text(many, (size_t)(w - many),
                                 FE_CHEATS_SYS_GBA) == FE_CHEATS_LOADED);
      assert(fe_cheats_count() == 80);
      for (i = 0; i < MAX_CHEATS; i++)
         assert(fe_cheats_set(i, 1) == 0);
      assert(fe_cheats_set(MAX_CHEATS, 1) == CheatErrorTooMany);
      assert(!fe_cheats_get(MAX_CHEATS)->enabled);
      assert(fe_cheats_enabled_count() == MAX_CHEATS);
      /* It stays usable: switch one off and it goes on. */
      assert(fe_cheats_set(0, 0) == 0);
      assert(fe_cheats_set(MAX_CHEATS, 1) == 0);
   }
   fe_cheats_reset();
   assert(cheats_active == 0 && fe_cheats_status() == FE_CHEATS_NONE);
}

static void test_shapes(void)
{
   /* Unnamed codes, a bare value line, an empty file, a file of words. */
   static const char unnamed[] = "82000010 0001\n82000012 0002\n";
   static const char words[] = "just some words\nand more\n\n";
   static const char oneline[] = "No newline at the end\n82000010 0001";
   static const char quoted[] = "\"82000010 0001\"\n82000010 0002\n";
   reset();
   assert(fe_cheats_load_text(unnamed, sizeof(unnamed) - 1,
                              FE_CHEATS_SYS_GBA) == FE_CHEATS_LOADED);
   assert(fe_cheats_count() == 1 &&
          !strcmp(fe_cheats_get(0)->name, "Unnamed cheat") &&
          fe_cheats_get(0)->error == 0);
   assert(fe_cheats_load_text("", 0, FE_CHEATS_SYS_GBA) == FE_CHEATS_EMPTY);
   assert(fe_cheats_load_text(words, sizeof(words) - 1, FE_CHEATS_SYS_GBA) ==
          FE_CHEATS_EMPTY);
   assert(fe_cheats_count() == 0 && fe_cheats_found() == 0);
   assert(fe_cheats_load_text(oneline, sizeof(oneline) - 1,
                              FE_CHEATS_SYS_GBA) == FE_CHEATS_LOADED);
   assert(fe_cheats_count() == 1 && fe_cheats_get(0)->error == 0);
   assert(fe_cheats_load_text(quoted, sizeof(quoted) - 1,
                              FE_CHEATS_SYS_GBA) == FE_CHEATS_LOADED);
   assert(fe_cheats_count() == 1 &&
          !strcmp(fe_cheats_get(0)->name, "82000010 0001"));

   /* More entries than the list holds: the first FE_CHEATS_MAX are listed
    * and the total is still counted (the menu says "256 of N"). */
   {
      static char lots[400 * 32];
      char *w = lots;
      unsigned i;
      for (i = 0; i < 400; i++)
         w += sprintf(w, "Cheat number %u\n82000010 %04X\n", i, i);
      assert(fe_cheats_load_text(lots, (size_t)(w - lots),
                                 FE_CHEATS_SYS_GBA) == FE_CHEATS_LOADED);
      assert(fe_cheats_count() == FE_CHEATS_MAX && fe_cheats_found() == 400);
      assert(!strcmp(fe_cheats_get(FE_CHEATS_MAX - 1)->name,
                     "Cheat number 255"));
   }
   /* A file far larger than the buffer: read to the cap, the cut line
    * dropped, flagged. */
   {
      size_t cap = 4u * 1024 * 1024, n = 0;
      char *huge = (char *)malloc(cap);
      unsigned i = 0;
      assert(huge);
      while (n + 64 < cap)
      {
         n += (size_t)sprintf(huge + n, "Entry %u\n82000010 %04X\n", i,
                              i & 0xFFFF);
         i++;
      }
      assert(fe_cheats_load_text(huge, n, FE_CHEATS_SYS_GBA) ==
             FE_CHEATS_LOADED);
      assert(fe_cheats_truncated() && fe_cheats_count() == FE_CHEATS_MAX);
      free(huge);
   }
   /* One enormous line, no newline at all: nothing to list, no overrun. */
   {
      static char line[FE_CHEATS_FILE_MAX + 4096];
      memset(line, 'A', sizeof(line));
      assert(fe_cheats_load_text(line, sizeof(line), FE_CHEATS_SYS_GBA) ==
             FE_CHEATS_EMPTY);
      assert(fe_cheats_truncated());
   }
   /* NUL bytes inside the text do not end it early. */
   {
      static const char nul[] = "Nul\0name\n82000010 0001\nSecond\n82000012 0002\n";
      assert(fe_cheats_load_text(nul, sizeof(nul) - 1, FE_CHEATS_SYS_GBA) ==
             FE_CHEATS_LOADED);
      assert(fe_cheats_count() == 2 &&
             !strcmp(fe_cheats_get(0)->name, "Nul name"));
   }
}

static void test_cht(void)
{
   static const char cht[] =
      "cheats = 4\n"
      "\n"
      "cheat0_desc = \"Infinite HP\"\n"
      "cheat0_code = \"82000010 0063+82000012 0063\"\n"
      "cheat0_enable = true\n"
      "cheat0_handler = \"0\"\n"
      "cheat2_desc = \"After the gap\"\n"
      "cheat2_code = \"02000020:7F\"\n"
      "cheat2_enable = false\n"
      "cheat1_desc = \"GameShark\"\n"
      "cheat1_code = \"12345678 9ABCDEF0\"\n"
      "cheat3_desc = \"No code\"\n"
      "cheat99999999999_code = \"82000010 0001\"\n"
      "cheatX_code = \"82000010 0001\"\n"
      "unrelated = 1\n";
   reset();
   assert(fe_cheats_load_text(cht, sizeof(cht) - 1, FE_CHEATS_SYS_GBA) ==
          FE_CHEATS_LOADED);
   assert(fe_cheats_count() == 3);
   assert(!strcmp(fe_cheats_get(0)->name, "Infinite HP") &&
          fe_cheats_get(0)->error == 0);
   /* cheatN_enable is not read: a file cannot switch a cheat on. */
   assert(!fe_cheats_get(0)->enabled);
   assert(!strcmp(fe_cheats_get(1)->name, "GameShark") &&
          fe_cheats_get(1)->error == CheatErrorGameShark);
   assert(!strcmp(fe_cheats_get(2)->name, "After the gap") &&
          fe_cheats_get(2)->error == 0);
   fe_cheats_attach(1);
   assert(fe_cheats_set(0, 1) == 0 && fe_cheats_set(2, 1) == 0);
   frame();
   assert(ew16(0x10) == 0x63 && ew16(0x12) == 0x63 && ewram[0x20] == 0x7F);
}

static void test_gb(void)
{
   static const char gb[] =
      "Infinite money\n"
      "019947D3\n019948D3\n019949D3\n"
      "High RAM\n"
      "0105D8FF\n"
      "Game Genie\n"
      "00A-17B-C49\n"
      "Bank code\n"
      "910147D3\n"
      "Writes the save\n"
      "01FF00A0\n"
      "Writes ROM\n"
      "01FF0001\n"
      "Writes IO\n"
      "01FF40FF\n"
      "Interrupt enable\n"
      "01FFFFFF\n"
      "CodeBreaker for the wrong console\n"
      "82025BC4 E0FF\n";
   static char st[FE_CHEATS_STATE_MAX];
   size_t n;
   reset();
   memset(gb_mem, 0, sizeof(gb_mem));
   gb_refused = 0;
   assert(fe_cheats_load_text(gb, sizeof(gb) - 1, FE_CHEATS_SYS_GB) ==
          FE_CHEATS_LOADED);
   assert(fe_cheats_count() == 9 && fe_cheats_system() == FE_CHEATS_SYS_GB);
   assert(fe_cheats_get(0)->error == 0 && fe_cheats_get(1)->error == 0);
   assert(fe_cheats_get(2)->error == FE_CHEAT_ERR_GB_GENIE);
   assert(fe_cheats_get(3)->error == FE_CHEAT_ERR_GB_TYPE);
   assert(fe_cheats_get(4)->error == FE_CHEAT_ERR_GB_REGION);
   assert(fe_cheats_get(5)->error == FE_CHEAT_ERR_GB_REGION);
   assert(fe_cheats_get(6)->error == FE_CHEAT_ERR_GB_REGION);
   assert(fe_cheats_get(7)->error == FE_CHEAT_ERR_GB_REGION);
   assert(fe_cheats_get(8)->error == FE_CHEAT_ERR_GB_TYPE);

   /* Detached, or all off: no pokes. */
   fe_cheats_gb_frame(gb_poke, NULL);
   fe_cheats_attach(1);
   fe_cheats_gb_frame(gb_poke, NULL);
   assert(gb_mem[0xD347] == 0);
   assert(fe_cheats_set(0, 1) == 0 && fe_cheats_set(1, 1) == 0);
   assert(fe_cheats_set(4, 1) == FE_CHEAT_ERR_GB_REGION);
   assert(cheats_active == 0);               /* the GBA core is not involved */
   fe_cheats_gb_frame(gb_poke, NULL);
   assert(gb_mem[0xD347] == 0x99 && gb_mem[0xD348] == 0x99 &&
          gb_mem[0xD349] == 0x99 && gb_mem[0xFFD8] == 0x05);
   assert(gb_refused == 0 && fe_cheats_gb_writes() == 4);
   /* Locked: nothing. */
   memset(gb_mem, 0, sizeof(gb_mem));
   fe_cheats_lock(1);
   fe_cheats_gb_frame(gb_poke, NULL);
   assert(gb_mem[0xD347] == 0);
   fe_cheats_lock(0);
   /* State round trip on the GB list. */
   n = fe_cheats_state_serialise(st, sizeof(st));
   assert(fe_cheats_load_text(gb, sizeof(gb) - 1, FE_CHEATS_SYS_GB) ==
          FE_CHEATS_LOADED);
   fe_cheats_gb_frame(gb_poke, NULL);
   assert(gb_mem[0xD347] == 0);
   fe_cheats_state_parse(st, n);
   fe_cheats_gb_frame(gb_poke, NULL);
   assert(gb_mem[0xD347] == 0x99 && fe_cheats_enabled_count() == 2);
   /* More writes on than the table holds: the cheat that does not fit is
    * refused whole. */
   {
      static char many[300 * 24];
      char *w = many;
      unsigned i;
      for (i = 0; i < 300; i++)
         w += sprintf(w, "G%u\n01%02X%02XC0\n", i, i & 0xFF, i & 0xFF);
      assert(fe_cheats_load_text(many, (size_t)(w - many), FE_CHEATS_SYS_GB) ==
             FE_CHEATS_LOADED);
      assert(fe_cheats_count() == FE_CHEATS_MAX);
      for (i = 0; i < FE_CHEATS_GB_POKES; i++)
         assert(fe_cheats_set(i, 1) == 0);
      fe_cheats_gb_frame(gb_poke, NULL);
      assert(gb_refused == 0);
   }
   fe_cheats_reset();
}

/* Random bytes, and random text that is shaped like codes, through every
 * entry point; then frames.  The assertions are the sanitizers'. */
static void test_fuzz(void)
{
   static char text[2048];
   static const char alpha[] = "0123456789abcdefABCDEF  \n\r+:;,-[]\"#/gG=_ch";
   unsigned round, i;
   reset();
   for (round = 0; round < FUZZ_ROUNDS; round++)
   {
      unsigned len = rnd() % (sizeof(text) - 1), mode = rnd() % 6;
      if (mode == 0)
         for (i = 0; i < len; i++)
            text[i] = (char)rnd();
      else if (mode == 1)
         for (i = 0; i < len; i++)
            text[i] = alpha[rnd() % (sizeof(alpha) - 1)];
      else if (mode >= 4)
      {
         static const char *const families[] = { "GS1:", "GS1RAW:", "AR3:", "AR3RAW:" };
         char *w = text;
         unsigned lines = 1 + rnd() % 12;
         w += sprintf(w, "%s", families[rnd() % 4]);
         for (i = 0; i < lines; i++)
         {
            u32 a = rnd(), v = rnd();
            if (mode == 5)
            {
               /* Frequent legal direct AR/GS RAM writes and bounded fills,
                * interspersed with unsupported or non-RAM operations. */
               a = (rnd() % 2 ? 0x00200000u : 0x02000000u) |
                   (rnd() & 0xFCu) | ((rnd() % 3) << 25);
               v &= 0xFF;
            }
            w += sprintf(w, "%08X %08X+", a, v);
         }
         len = (unsigned)(w - text);
      }
      else
      {
         /* Well-formed CodeBreaker lines with random opcodes, mostly in RAM. */
         char *w = text;
         unsigned lines = 1 + rnd() % 12;
         for (i = 0; i < lines && (size_t)(w - text) < sizeof(text) - 40; i++)
         {
            u32 a = (rnd() % 8 ? (rnd() & 1 ? 0x02000000u : 0x03000000u)
                               : (rnd() & 0x0F000000u)) | (rnd() & 0x00FFFFFEu);
            if (mode == 3 && !(rnd() % 4))
               w += sprintf(w, "%08lX:%0*lX\n", (unsigned long)(a & ~3u),
                            2 << (rnd() % 3),
                            (unsigned long)(rnd() & 0xFF));
            else
               w += sprintf(w, "%lX%07lX %04lX\n", (unsigned long)(rnd() & 15),
                            (unsigned long)(a & 0x0FFFFFFFu),
                            (unsigned long)(rnd() & (rnd() % 3 ? 0x1F : 0xFFFF)));
         }
         len = (unsigned)(w - text);
      }
      text[len] = '\0';
      /* The core, directly... */
      cheat_parse(rnd() % (MAX_CHEATS + 2), text);
      cheat_check(text);
      io_registers[REG_P1] = (u16)(rnd() & 0x3FF);
      frame();
      /* ...and as a file, on both systems, with a state made of noise. */
      if (!(round % 8))
      {
         fe_cheats_load_text(text, len, (int)(rnd() & 1));
         fe_cheats_attach(1);
         for (i = 0; i < fe_cheats_count(); i++)
            fe_cheats_set(i, 1);
         frame();
         fe_cheats_gb_frame(gb_poke, NULL);
         fe_cheats_state_parse(text, len);
         {
            static char st[FE_CHEATS_STATE_MAX];
            size_t n = fe_cheats_state_serialise(st, sizeof(st));
            if (n < sizeof(st))
               fe_cheats_state_parse(st, n);
         }
         frame();
         fe_cheats_gb_frame(gb_poke, NULL);
         fe_cheats_reset();
      }
      if (!(round % 64))
         cheat_clear();
   }
   assert(gb_refused == 0);
}

/* The example that ships in the release's cheats/ folder (and that
 * docs/CHEATS.md quotes) must itself be a valid list: its comment block is
 * all comment, and its two cheats are usable and do what they say.  Run from
 * the repository root, as tools/run_host_tests.py does. */
static void test_example(void)
{
   FILE *f = fopen("assets/cheats/README.txt", "rb");
   size_t n;
   assert(f && "run from the repository root");
   n = fread(fe_cheats_buffer(), 1, FE_CHEATS_FILE_MAX, f);
   fclose(f);
   reset();
   assert(fe_cheats_parse(n, FE_CHEATS_SYS_GBA, 0) == FE_CHEATS_LOADED);
   assert(fe_cheats_count() == 2 && fe_cheats_usable_count() == 2);
   assert(!strcmp(fe_cheats_get(0)->name, "Infinite money"));
   assert(!strcmp(fe_cheats_get(1)->name, "Money: exactly 5000"));
   fe_cheats_attach(1);
   assert(fe_cheats_set(0, 1) == 0);
   frame();
   assert(ew32(0x25BC4) == 999999);
   assert(fe_cheats_set(1, 1) == 0);
   frame();
   assert(ew32(0x25BC4) == 5000);       /* later in the list wins */
   fe_cheats_reset();
}

int main(void)
{
   test_core();
   test_gs_cipher();
   test_gs_operations();
   test_gs_refusals();
   test_cheat_work_budget();
   test_gs_files();
   test_example();
   test_plain();
   test_shapes();
   test_cht();
   test_gb();
   test_fuzz();
   puts("PASS cheats: GS1/AR3 encrypted and raw vectors, reseeds, RAM effects, "
        "conditionals, transactional rejection, work limits, rewind, "
        "flush trigger, link suspension, cheat files, .on state, GB, fuzz");
   return 0;
}
