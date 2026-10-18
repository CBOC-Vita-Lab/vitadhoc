/* Explicit GameShark v1/v2 and Action Replay v3 RAM-only subset.
 * Included by cheats.c after its tokenizer, record pool and RAM validators.
 * Format references: mGBA src/gba/cheats/{gameshark,parv3}.c;
 * docs/CHEATS.md specifies every accepted operation and limitation.
 * All cipher state is local to compilation; no per-frame decryption. */
#ifndef GPSP_CHEATS_GS_H
#define GPSP_CHEATS_GS_H
#include "cheats_gs_cipher.h"

/* Private normalized records (not CodeBreaker). Top nibble:
 * 0/1/2 write 8/16/32; 3/4/5 contiguous fill + count record;
 * 6/7/8 compare 8/16/32 + {comparison, number of operations to skip}.
 * Payloads are never dispatched. Every skip counts complete operations. */
enum { GS_EQ, GS_NE, GS_LE, GS_GE, GS_ULT, GS_UGT, GS_AND };

static unsigned cheat_gs_len(const cheat_type *ch, unsigned i)
{
   return (ch->codes[i].address >> 28) >= 3 ? 2 : 1;
}

static void process_cheat_gs(const cheat_type *ch)
{
   unsigned i = 0;
   while (i < ch->cheat_count)
   {
      u32 a = ch->codes[i].address & 0x0FFFFFFFu;
      u32 v = ch->codes[i].value;
      unsigned op = ch->codes[i].address >> 28;
      unsigned width = 1u << (op % 3), count = 1;
      i++;
      if (op >= 6)
      {
         u32 got;
         int pass = 0;
         unsigned cond = ch->codes[i].address;
         unsigned skip = ch->codes[i++].value;
         if (width == 1)
         {
            u8 *tag, *p = cheat_ram(a, &tag);
            got = p ? *p : 0;
         }
         else got = width == 2 ? cheat_r16(a) : cheat_r32(a);
         switch (cond)
         {
         case GS_EQ: pass = got == v; break;
         case GS_NE: pass = got != v; break;
         case GS_LE: pass = got <= v; break;
         case GS_GE: pass = got >= v; break;
         case GS_ULT: pass = got < v; break;
         case GS_UGT: pass = got > v; break;
         case GS_AND: pass = (got & v) != 0; break;
         }
         if (!pass)
            while (skip-- && i < ch->cheat_count)
               i += cheat_gs_len(ch, i);
         continue;
      }
      if (op >= 3) count = ch->codes[i++].value;
      while (count--)
      {
         if (width == 1) cheat_w8(a, v);
         else if (width == 2) cheat_w16(a, v);
         else cheat_w32(a, v);
         a += width;
      }
   }
}

static cheat_error cheat_gs_emit(cheat_type *ch, u32 a, u32 v,
                                unsigned width, unsigned count,
                                int cond, unsigned skip)
{
   unsigned op = width == 1 ? 0 : width == 2 ? 1 : 2;
   u32 last;
   if (!count) return CheatErrorUnsupported;
   if (count > CHEAT_FRAME_MAX) return CheatErrorWork;
   if (!cheat_in_ram(a)) return CheatErrorRegion;
   if (a & (width - 1)) return CheatErrorAlign;
   /* Refuse the whole operation if even its final byte leaves this RAM
    * region. Mirrored work RAM remains legal, like existing raw codes. */
   last = a + count * width - 1;
   if (last < a || (last >> 24) != (a >> 24)) return CheatErrorRegion;
   if (width < 4 && v >= (1u << (width * 8))) return CheatErrorSyntax;
   /* The whole shared pool, not each individual fill, has one operation
    * budget. Include condition reads in the worst case. As with records,
    * disabled/replaced slots release their budget at the next rebuild. */
   if (count > CHEAT_FRAME_MAX - cheat_work_used - ch->work)
      return CheatErrorWork;
   ch->work += count;
   if (cond >= 0) op += 6;
   else if (count > 1) op += 3;
   if (!cheat_push(ch, a | (op << 28), v)) return CheatErrorTooBig;
   if (cond >= 0)
   {
      if (!cheat_push(ch, (u32)cond, skip)) return CheatErrorTooBig;
   }
   else if (count > 1 && !cheat_push(ch, 0, count)) return CheatErrorTooBig;
   return CheatNoError;
}

static cheat_error cheat_gs_compile(cheat_type *ch, const char *s, unsigned family)
{
   static const u32 initial[2][4] = {
      { 0x09F4FBBDu, 0x9681884Au, 0x352027E9u, 0xF3DEE5A7u },
      { 0x7AA9648Fu, 0x7FAE6994u, 0xC0EFAAD5u, 0x42712C57u }
   };
   int ar3 = family >= CHEAT_AR3;
   int encrypted = family == CHEAT_GS1 || family == CHEAT_AR3;
   u32 seeds[4];
   const char *t, *t2;
   unsigned n, n2, i;
   memcpy(seeds, initial[ar3], sizeof(seeds));
   ch->gs_format = true;
   while (cheat_token(&s, &t, &n))
   {
      u32 a, v, addr;
      unsigned width, count = 1, skip = 1, used;
      int cond = -1;
      cheat_error err;
      if (cheat_family_prefix(t, n, &used)) return CheatErrorMixed;
      if (memchr(t, ':', n)) return CheatErrorMixed;
      if (n == 16)
      {
         if (!cheat_hex(t, 8, &a) || !cheat_hex(t + 8, 8, &v))
            return CheatErrorSyntax;
      }
      else if (n == 8)
      {
         if (!cheat_hex(t, 8, &a) || !cheat_token(&s, &t2, &n2))
            return CheatErrorSyntax;
         if (n2 == 4 && cheat_all_hex(t2, n2)) return CheatErrorMixed;
         if (n2 != 8 || !cheat_hex(t2, 8, &v)) return CheatErrorSyntax;
      }
      else return CheatErrorSyntax;
      if (encrypted) cheat_gs_decrypt(&a, &v, seeds);
      if (a == 0xDEADFACEu)
      {
         if (v > 0xFFFF) return CheatErrorUnsupported;
         cheat_gs_reseed(seeds, (u16)v, ar3);
         continue;
      }
      if (!ar3)
      {
         unsigned type = a >> 28;
         addr = a & 0x0FFFFFFFu;
         if (type <= 2) width = 1u << type;
         else if (type == 0xD)
         {
            static const int conditions[4] = { GS_EQ, GS_NE, GS_LE, GS_GE };
            if (v & 0xFFCF0000u) return CheatErrorUnsupported;
            cond = conditions[v >> 20];
            v &= 0xFFFF;
            width = 2;
         }
         else return CheatErrorUnsupported;
      }
      else
      {
         unsigned compare = (a >> 27) & 7;
         unsigned action = a >> 30;
         unsigned w = (a >> 25) & 3;
         /* Metadata IDs, hook, I/O, pointer, add, block/else, special
          * multi-line commands, reserved bits and width=8 are refused. */
         if (!a || v == 0x001DC0DEu || (a & 0x01000000u) || w == 3)
            return CheatErrorUnsupported;
         width = 1u << w;
         addr = (a & 0xFFFFFu) | ((a << 4) & 0x0F000000u);
         if (compare)
         {
            if (action > 1) return CheatErrorUnsupported;
            skip = action + 1;
            switch (compare)
            {
            case 1: cond = GS_EQ; break;
            case 2: cond = GS_NE; break;
            case 5: cond = GS_ULT; break;
            case 6: cond = GS_UGT; break;
            case 7: cond = GS_AND; break;
            default: return CheatErrorUnsupported; /* signed LT / GT */
            }
         }
         else
         {
            if (action) return CheatErrorUnsupported;
            if (width < 4)
            {
               count = (v >> (width * 8)) + 1;
               v &= (1u << (width * 8)) - 1;
            }
         }
      }
      err = cheat_gs_emit(ch, addr, v, width, count, cond, skip);
      if (err != CheatNoError) return err;
   }
   if (!ch->cheat_count) return CheatErrorSyntax;
   /* Reject incomplete conditionals, including next-two with only one
    * following operation. Reseeds consume no operation or skip slot. */
   for (i = 0; i < ch->cheat_count; i += cheat_gs_len(ch, i))
      if ((ch->codes[i].address >> 28) >= 6)
      {
         unsigned j = i + 2, left = ch->codes[i + 1].value;
         while (left && j < ch->cheat_count)
         {
            j += cheat_gs_len(ch, j);
            left--;
         }
         if (left) return CheatErrorNotSupported;
      }
   return CheatNoError;
}
#endif
