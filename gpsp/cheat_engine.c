/* cheat_engine.c -- see cheat_engine.h.
 *
 * Ported from mGBA (Copyright (c) 2013-2016 Jeffrey Pfau, MPL-2.0):
 *   src/core/cheats.c           mCheatRefresh          -> cheat_vm_run
 *   src/gba/cheats/codebreaker.c                       -> cb_*
 *   src/gba/cheats/gameshark.c                         -> gsa1_*, cheat_gsa_*
 *   src/gba/cheats/parv3.c                             -> par3_*
 *   src/gba/cheats.c            GBACheatAddressIsReal, the autodetection
 *   src/gb/cheats.c             Game Genie / GameShark -> cheat_gb_compile
 * Differences from mGBA are marked "GBAdhoc:".
 */
#include "cheat_engine.h"
#include "cheat_engine_tables.h"

#include <string.h>
#ifdef CE_DEBUG
#include <stdio.h>
#endif

#define COMPLETE (-1)
#define ROR32(x, n) (((uint32_t)(x) >> (n)) | ((uint32_t)(x) << (32 - (n))))

/* ======================================================================== */
/* The machine (mGBA mCheatRefresh)                                         */
/* ======================================================================== */

static int32_t sext(uint32_t v, int width)
{
   if (width == 1)
      return (int8_t)v;
   if (width == 2)
      return (int16_t)v;
   return (int32_t)v;
}

void cheat_vm_run(const cheat_op *ops, unsigned n, const cheat_bus *bus,
                  int button)
{
   unsigned else_loc = 0, end_loc = 0, i;

   for (i = 0; i < n; ++i)
   {
      const cheat_op *c = &ops[i];
      uint32_t value = 0;
      uint32_t operand = c->operand;
      uint32_t remaining = c->repeat;
      uint32_t address = c->address;
      int assign = 0;
      int condition = 1;
      unsigned cond_remaining = 0, neg_remaining = 0;
#define RD() bus->read(bus->ud, address, c->width)
#define COND(expr)                                                           \
   do {                                                                      \
      condition = (expr) ? 1 : 0;                                            \
      cond_remaining = c->repeat;                                            \
      neg_remaining = c->negative_repeat;                                    \
      remaining = 1;                                                         \
   } while (0)

      for (; remaining; --remaining)
      {
         switch (c->type)
         {
         case CE_ASSIGN:
            value = operand;
            assign = 1;
            break;
         case CE_ASSIGN_INDIRECT:
            value = operand;
            address = bus->read(bus->ud, address, 4) + (uint32_t)c->address_offset;
            assign = 1;
            break;
         case CE_AND:
            value = RD() & operand;
            assign = 1;
            break;
         case CE_ADD:
            value = RD() + operand;
            assign = 1;
            break;
         case CE_SUB:
            value = RD() - operand;
            assign = 1;
            break;
         case CE_OR:
            value = RD() | operand;
            assign = 1;
            break;
         case CE_IF_EQ:
            COND(RD() == operand);
            break;
         case CE_IF_NE:
            COND(RD() != operand);
            break;
         /* GBAdhoc: mGBA compares zero-extended reads as int32, which is
          * signed only at width 4; the Action Replay's "signed" tests are
          * signed at their own width. */
         case CE_IF_LT:
            COND(sext(RD(), c->width) < sext(operand, c->width));
            break;
         case CE_IF_GT:
            COND(sext(RD(), c->width) > sext(operand, c->width));
            break;
         case CE_IF_ULT:
            COND(RD() < operand);
            break;
         case CE_IF_UGT:
            COND(RD() > operand);
            break;
         case CE_IF_ULE:
            COND(RD() <= operand);
            break;
         case CE_IF_UGE:
            COND(RD() >= operand);
            break;
         case CE_IF_AND:
            COND(RD() & operand);
            break;
         case CE_IF_LAND:
            COND(RD() && operand);
            break;
         case CE_IF_NAND:
            COND(!(RD() & operand));
            break;
         case CE_IF_BUTTON:
            COND(button);
            break;
         case CE_NEVER:
         default:
            COND(0);
            break;
         }

         if (assign)
            bus->write(bus->ud, address, c->width, value);

         address += (uint32_t)c->address_offset;
         operand += (uint32_t)c->operand_offset;
      }
#undef RD
#undef COND

      if (else_loc && i == else_loc)
      {
         i = end_loc;
         end_loc = 0;
      }
      if (cond_remaining > 0 && !condition)
         i += cond_remaining;
      else if (neg_remaining > 0)
      {
         else_loc = i + cond_remaining;
         end_loc = else_loc + neg_remaining;
      }
   }
}

/* ======================================================================== */
/* Text -> lines                                                            */
/* ======================================================================== */

static int hexval(int c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

/* What the libretro database and people put between the parts of a code. */
static int is_sep(int c)
{
   return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '+' ||
          c == ',' || c == ';';
}

/* "01??C0D5", "3300238C 00XX": a digit the player is meant to choose. */
static int is_placeholder(int c)
{
   return c == '?' || c == '*' || c == 'X' || c == 'x' || c == 'Y' ||
          c == 'y' || c == 'Z' || c == 'z';
}

/* Classify the whole text before parsing: CE_OK, CE_E_PLACEHOLDER or
 * CE_E_FORMAT.  `extra` lists the non-hex characters the kind allows inside
 * a token (':' for GBA, "-:$" for GB). */
static int text_check(const char *s, const char *extra)
{
   int digits = 0, place = 0;
   for (; *s; ++s)
   {
      int c = (unsigned char)*s;
      if (is_sep(c))
         continue;
      if (hexval(c) >= 0)
         digits++;
      else if (is_placeholder(c))
         place++;
      else if (!strchr(extra, c))
         return CE_E_FORMAT;
   }
   if (!digits && !place)
      return CE_E_FORMAT;
   /* A word such as "XXX" alone is not a code with a hole in it. */
   if (place)
      return digits ? CE_E_PLACEHOLDER : CE_E_FORMAT;
   return CE_OK;
}

typedef struct gline
{
   uint32_t a, b;
   uint8_t  bdigits;     /* 2, 4 or 8 */
   uint8_t  colon;       /* VBA "AAAAAAAA:VV" */
} gline;

#define GBA_MAX_LINES 700    /* 4096 characters of "8+4" lines is 315 */

/* One entry's lines while it is being read.  Shared by the three entry
 * points (none calls another): the engine is for one thread, the one that
 * owns the cheat list. */
static gline ce_lines[GBA_MAX_LINES];

static int hexrun(const char *s, unsigned n, uint32_t *out)
{
   uint32_t v = 0;
   unsigned i;
   for (i = 0; i < n; i++)
   {
      int h = hexval((unsigned char)s[i]);
      if (h < 0)
         return 0;
      v = (v << 4) | (uint32_t)h;
   }
   *out = v;
   return 1;
}

/* Tokens of 8, 4 and 2 digits (and the run-together 12, 16 and 10) become
 * lines "8+4" (CodeBreaker), "8+8" (GameShark / Action Replay), "8+2" (a
 * raw byte) and "8:n". */
static int gba_lines(const char *s, gline *L, unsigned cap, unsigned *n_out)
{
   unsigned n = 0;
   int pending = 0;
   uint32_t first = 0;
   int rc = text_check(s, ":");

   if (rc != CE_OK)
      return rc;

   while (*s)
   {
      const char *t;
      unsigned len;
      uint32_t v;
      gline g;

      while (*s && is_sep((unsigned char)*s))
         s++;
      if (!*s)
         break;
      t = s;
      while (*s && !is_sep((unsigned char)*s))
         s++;
      len = (unsigned)(s - t);

      memset(&g, 0, sizeof(g));
      if (memchr(t, ':', len))
      {
         const char *colon = (const char *)memchr(t, ':', len);
         unsigned l1 = (unsigned)(colon - t), l2 = len - l1 - 1;
         if (pending || l1 < 1 || l1 > 8 ||
             (l2 != 2 && l2 != 4 && l2 != 8) ||
             !hexrun(t, l1, &g.a) || !hexrun(colon + 1, l2, &g.b))
            return CE_E_FORMAT;
         g.bdigits = (uint8_t)l2;
         g.colon = 1;
      }
      else if (!pending)
      {
         if (len == 8)
         {
            if (!hexrun(t, 8, &first))
               return CE_E_FORMAT;
            pending = 1;
            continue;
         }
         if (len != 10 && len != 12 && len != 16)
            return CE_E_FORMAT;
         if (!hexrun(t, 8, &g.a) || !hexrun(t + 8, len - 8, &g.b))
            return CE_E_FORMAT;
         g.bdigits = (uint8_t)(len - 8);
      }
      else
      {
         if ((len != 2 && len != 4 && len != 8) || !hexrun(t, len, &v))
            return CE_E_FORMAT;
         g.a = first;
         g.b = v;
         g.bdigits = (uint8_t)len;
         pending = 0;
      }
      if (n >= cap)
         return CE_E_TOO_LONG;
      L[n++] = g;
   }
   if (pending || !n)
      return CE_E_FORMAT;
   *n_out = n;
   return CE_OK;
}

/* ======================================================================== */
/* The builder                                                              */
/* ======================================================================== */

typedef struct gba_build
{
   cheat_out *out;
   int      err;                 /* sticky CE_E_* */
   int      incomplete;          /* op waiting for its parameter line */
   int      incomplete_patch;    /* patch waiting for its value line */
   int      current_block;       /* PARv3 open "if" block */
   int      remaining;           /* CodeBreaker halfwords / GSA addresses */
   uint32_t list_value;
   uint32_t gsa_seeds[4];
   uint32_t cb_master, cb_rng, cb_seeds[4];
   uint8_t  cb_table[0x30];
   long     score;               /* 8+8 kinds: summed plausibility */
   unsigned scored;
   unsigned plausible, implausible;   /* CodeBreaker */
   unsigned raw_lines, kind_lines;
   int      cb_cond_new;         /* CodeBreaker "if" just made ...          */
   int      cb_cond_wait;        /* ... and the one whose code is being made */
} gba_build;

static cheat_op ce_dummy_op;
static cheat_patch ce_dummy_patch;

static void build_init(gba_build *b, cheat_out *out)
{
   memset(b, 0, sizeof(*b));
   b->out = out;
   out->n_ops = 0;
   out->n_patches = 0;
   out->flags = 0;
   b->incomplete = COMPLETE;
   b->incomplete_patch = COMPLETE;
   b->current_block = COMPLETE;
   b->cb_cond_new = COMPLETE;
   b->cb_cond_wait = COMPLETE;
}

/* GBAdhoc: a CodeBreaker "if" guards the NEXT CODE, and a slide or a super
 * code is several operations here.  mGBA skips one operation (and so runs
 * the rest of a super code's data unconditionally -- the bug gpSP's own
 * cheats.c had fixed).  Called when a code begins and at the end: the "if"
 * before the code that just ended now knows how long that code was. */
static void cb_cond_close(gba_build *b)
{
   if (b->cb_cond_wait != COMPLETE)
   {
      unsigned span = b->out->n_ops - (unsigned)b->cb_cond_wait - 1;
      if (span)
         b->out->ops[b->cb_cond_wait].repeat = span;
      b->cb_cond_wait = COMPLETE;
   }
   b->cb_cond_wait = b->cb_cond_new;
   b->cb_cond_new = COMPLETE;
}

static cheat_op *op_append(gba_build *b)
{
   cheat_op *c;
   if (b->out->n_ops >= b->out->cap_ops)
   {
      b->err = CE_E_TOO_LONG;
      memset(&ce_dummy_op, 0, sizeof(ce_dummy_op));
      return &ce_dummy_op;
   }
   c = &b->out->ops[b->out->n_ops++];
   memset(c, 0, sizeof(*c));
   return c;
}

static cheat_patch *patch_append(gba_build *b)
{
   cheat_patch *p;
   b->out->flags |= CE_F_ROM_PATCH;
   if (b->out->n_patches >= b->out->cap_patches)
   {
      b->err = CE_E_TOO_LONG;
      memset(&ce_dummy_patch, 0, sizeof(ce_dummy_patch));
      return &ce_dummy_patch;
   }
   p = &b->out->patches[b->out->n_patches++];
   memset(p, 0, sizeof(*p));
   return p;
}

static cheat_op *op_new(gba_build *b, int type, int width, uint32_t address,
                        uint32_t operand)
{
   cheat_op *c = op_append(b);
   c->type = (uint8_t)type;
   c->width = (uint8_t)width;
   c->address = address;
   c->operand = operand;
   c->repeat = 1;
   return c;
}

static int op_index(const gba_build *b)
{
   return (int)b->out->n_ops - 1;
}

/* mGBA GBACheatAddressIsReal */
static int addr_is_real(uint32_t address)
{
   uint32_t off = address & 0x00FFFFFF;
   switch (address >> 24)
   {
   case 0x0:
      return -0x80;
   case 0x2:
      return off > 0x40000 ? -0x40 : 0x20;
   case 0x3:
      return off > 0x8000 ? -0x40 : 0x20;
   case 0x4:
      return off > 0x400 ? -0x80 : 0x10;
   case 0x5:
      return off > 0x400 ? -0x80 : -0x8;
   case 0x6:
      return off > 0x18000 ? -0x80 : -0x8;
   case 0x7:
      return off > 0x400 ? -0x80 : -0x8;
   case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
      return -0x8;
   case 0xE: case 0xF:
      return off > 0x10000 ? -0x80 : -0x8;
   default:
      return -0xC0;
   }
}

/* GBAdhoc: how believable an address is as the target of a GameShark /
 * Action Replay write or test.  mGBA's table (addr_is_real) takes ROM and
 * backup-memory addresses as merely "less likely" (-8), which lets one line
 * of noise in ten read as a code; a cheat device writes RAM, sometimes an
 * I/O register, rarely video memory, never the cartridge. */
static int addr_score_ex(uint32_t address, int lenient)
{
   int real = addr_is_real(address);
   /* Work RAM repeats through its whole 16 MiB region, and published
    * Action Replay codes do name the mirrors (0x020E5EE8 for 0x02025EE8).
    * Only believed in a list known to be that device's: among noise, "any
    * address in two regions of sixteen" is no evidence at all. */
   if (lenient && ((address >> 24) == 2 || (address >> 24) == 3))
      return 0x20;
   if (real > 0)
      return real;
   if (real == -0x8 && (address >> 24) >= 5 && (address >> 24) <= 7)
      return real;
   return -0x80;
}

static int ce_lenient;          /* set around one candidate's scoring */

static int addr_score(uint32_t address)
{
   return addr_score_ex(address, ce_lenient);
}

/* Work RAM a raw "address value" line can name. */
static int is_work_ram(uint32_t a)
{
   return (a >= 0x02000000 && a < 0x02040000) ||
          (a >= 0x03000000 && a < 0x03008000);
}

static int is_rom(uint32_t a)
{
   return a >= 0x08000000 && a < 0x0E000000;
}

/* A raw line: write (or, in ROM, patch) `width` bytes. */
static void raw_add(gba_build *b, uint32_t address, uint32_t value, int width)
{
   b->raw_lines++;
   if (is_rom(address))
   {
      cheat_patch *p = patch_append(b);
      p->address = address;
      p->value = value;
      p->width = (uint8_t)width;
   }
   else
      op_new(b, CE_ASSIGN, width, address, value);
}

/* ======================================================================== */
/* CodeBreaker (mGBA codebreaker.c)                                         */
/* ======================================================================== */

static void cb_load(uint8_t *buffer, uint32_t op1, uint16_t op2)
{
   buffer[0] = (uint8_t)(op1 >> 24);
   buffer[1] = (uint8_t)(op1 >> 16);
   buffer[2] = (uint8_t)(op1 >> 8);
   buffer[3] = (uint8_t)op1;
   buffer[4] = (uint8_t)(op2 >> 8);
   buffer[5] = (uint8_t)op2;
}

static void cb_store(const uint8_t *buffer, uint32_t *op1, uint16_t *op2)
{
   *op1 = ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
          ((uint32_t)buffer[2] << 8) | buffer[3];
   *op2 = (uint16_t)((buffer[4] << 8) | buffer[5]);
}

static void cb_decrypt(const gba_build *b, uint32_t *op1, uint16_t *op2)
{
   uint8_t buffer[6];
   uint32_t master = b->cb_master;
   int i;

   cb_load(buffer, *op1, *op2);
   for (i = (int)sizeof(b->cb_table) - 1; i >= 0; --i)
   {
      unsigned offset_x = (unsigned)i >> 3;
      unsigned offset_y = b->cb_table[i] >> 3;
      int bit_x = i & 7;
      int bit_y = b->cb_table[i] & 7;
      uint8_t x = (buffer[offset_x] >> bit_x) & 1;
      uint8_t y = (buffer[offset_y] >> bit_y) & 1;
      uint8_t x2 = buffer[offset_x] & (uint8_t)~(1 << bit_x);
      uint8_t y2;
      if (y)
         x2 |= (uint8_t)(1 << bit_x);
      buffer[offset_x] = x2;
      /* after the store above: x and y may be the same byte */
      y2 = buffer[offset_y] & (uint8_t)~(1 << bit_y);
      if (x)
         y2 |= (uint8_t)(1 << bit_y);
      buffer[offset_y] = y2;
   }
   cb_store(buffer, op1, op2);

   *op1 ^= b->cb_seeds[0];
   *op2 ^= (uint16_t)b->cb_seeds[1];

   cb_load(buffer, *op1, *op2);
   for (i = 0; i < 5; ++i)
      buffer[i] ^= (uint8_t)((master >> 8) ^ buffer[i + 1]);
   buffer[5] ^= (uint8_t)(master >> 8);
   for (i = 5; i > 0; --i)
      buffer[i] ^= (uint8_t)(master ^ buffer[i - 1]);
   buffer[0] ^= (uint8_t)master;
   cb_store(buffer, op1, op2);

   *op1 ^= b->cb_seeds[2];
   *op2 ^= (uint16_t)b->cb_seeds[3];
}

static uint32_t cb_rand(gba_build *b)
{
   /* Roll the LCG three times to get enough bits of entropy */
   uint32_t roll = b->cb_rng * 0x41C64E6D + 0x3039;
   uint32_t roll2 = roll * 0x41C64E6D + 0x3039;
   uint32_t roll3 = roll2 * 0x41C64E6D + 0x3039;
   uint32_t mix = (roll << 14) & 0xC0000000;
   mix |= (roll2 >> 1) & 0x3FFF8000;
   mix |= (roll3 >> 16) & 0x7FFF;
   b->cb_rng = roll3;
   return mix;
}

static unsigned cb_swap_index(gba_build *b)
{
   uint32_t roll = cb_rand(b);
   uint32_t count = sizeof(b->cb_table);
   uint32_t bit = 1, mask;

   if (roll == count)
      roll = 0;
   if (roll < count)
      return roll;

   while (count < 0x10000000 && count < roll)
   {
      count <<= 4;
      bit <<= 4;
   }
   while (count < 0x80000000 && count < roll)
   {
      count <<= 1;
      bit <<= 1;
   }

   for (;;)
   {
      mask = 0;
      if (roll >= count)
         roll -= count;
      if (roll >= count >> 1)
      {
         roll -= count >> 1;
         mask |= ROR32(bit, 1);
      }
      if (roll >= count >> 2)
      {
         roll -= count >> 2;
         mask |= ROR32(bit, 2);
      }
      if (roll >= count >> 3)
      {
         roll -= count >> 3;
         mask |= ROR32(bit, 3);
      }
      if (!roll || !(bit >> 4))
         break;
      bit >>= 4;
      count >>= 4;
   }

   mask &= 0xE0000000;
   if (!mask || !(bit & 7))
      return roll;
   if (mask & ROR32(bit, 3))
      roll += count >> 3;
   if (mask & ROR32(bit, 2))
      roll += count >> 2;
   if (mask & ROR32(bit, 1))
      roll += count >> 1;
   return roll;
}

static void cb_reseed(gba_build *b, uint32_t op1, uint16_t op2)
{
   unsigned i;

   b->cb_rng = (op2 & 0xFF) ^ 0x1111;
   for (i = 0; i < sizeof(b->cb_table); ++i)
      b->cb_table[i] = (uint8_t)i;
   for (i = 0; i < 0x50; ++i)
   {
      unsigned x = cb_swap_index(b);
      unsigned y = cb_swap_index(b);
      uint8_t swap;
      /* GBAdhoc: mGBA indexes the table with whatever comes back; keep the
       * index inside it whatever the input code is. */
      x %= sizeof(b->cb_table);
      y %= sizeof(b->cb_table);
      swap = b->cb_table[x];
      b->cb_table[x] = b->cb_table[y];
      b->cb_table[y] = swap;
   }

   b->cb_rng = 0x4EFAD1C3;
   for (i = 0; i < ((op1 >> 24) & 0xF); ++i)
      b->cb_rng = cb_rand(b);
   b->cb_seeds[2] = cb_rand(b);
   b->cb_seeds[3] = cb_rand(b);

   b->cb_rng = (uint32_t)(op2 >> 8) ^ 0xF254;
   for (i = 0; i < (unsigned)(op2 >> 8); ++i)
      b->cb_rng = cb_rand(b);
   b->cb_seeds[0] = cb_rand(b);
   b->cb_seeds[1] = cb_rand(b);

   b->cb_master = op1;
}

static uint32_t swap16(uint32_t h)
{
   return ((h >> 8) | (h << 8)) & 0xFFFF;
}

/* 1 = accepted, 0 = not (b->err says why). */
static int cb_add(gba_build *b, uint32_t op1, uint16_t op2)
{
   unsigned type;
   uint32_t address;
   cheat_op *cheat = NULL;
   int bad = 0;

   if (b->err)
      return 0;
   b->kind_lines++;
   if (b->cb_master)
   {
      cb_decrypt(b, &op1, &op2);
      b->out->flags |= CE_F_DECRYPTED;
   }
   type = op1 >> 28;
   address = op1 & 0x0FFFFFFF;

   if (b->incomplete != COMPLETE)
   {
      cheat_op *inc = &b->out->ops[b->incomplete];
      if (b->remaining)
      {
         /* "super code" data: three halfwords a line, bytes in order */
         inc->operand = swap16(op1 >> 16);
         --b->remaining;
         if (b->remaining)
         {
            cheat = op_new(b, CE_ASSIGN, 2, inc->address + 2,
                           swap16(op1 & 0xFFFF));
            --b->remaining;
            inc = cheat;
         }
         if (b->remaining)
         {
            cheat = op_new(b, CE_ASSIGN, 2, inc->address + 2, swap16(op2));
            --b->remaining;
            inc = cheat;
         }
         if (!b->remaining)
            b->incomplete = COMPLETE;
         else
         {
            cheat = op_new(b, CE_ASSIGN, 2, inc->address + 2, 0);
            b->incomplete = op_index(b);
         }
      }
      else
      {
         /* slide parameters: iiiicccc ssss */
         inc->repeat = op1 & 0xFFFF;
         inc->address_offset = op2;
         inc->operand_offset = (int32_t)(op1 >> 16);
         b->incomplete = COMPLETE;
      }
      return !b->err;
   }

   /* How believable is this line?  Used to tell plain codes from encrypted
    * ones (and from encrypted ones decrypted with the wrong seeds).  A line
    * that names no memory is counted and skipped, never run: one such line
    * and the entry is refused as encrypted. */
   switch (type)
   {
   case 0x0:
      if ((op1 >> 16) != 0 && !is_work_ram(op1))
         bad = 1;
      break;
   case 0x1:
      if (!(address < 0x02000000 && op2 < 0x100))
         bad = 1;
      break;
   case 0x9:
      break;
   case 0xD:
      if ((op1 & 0x0FFFFF0F) != 0 || ((op1 >> 4) & 0xF) > 2)
         bad = 1;
      break;
   default:
   {
      /* work RAM, I/O, palette, VRAM, OAM -- inside their sizes */
      uint32_t a = address & ~1u;
      int real = addr_is_real(a);
      if (!(real > 0 || (real == -0x8 && (a >> 24) >= 5 && (a >> 24) <= 7)))
         bad = 1;
      break;
   }
   }
   if (bad)
      b->implausible++;
   else if (type != 0x9)
      b->plausible++;
   cb_cond_close(b);

   if (bad && type != 0x0 && type != 0x4 && type != 0x5)
      return 1;

   switch (type)
   {
   case 0x0:   /* game id -- or, GBAdhoc, a raw "address value" line */
      if ((op1 >> 16) == 0)
      {
         b->out->flags |= CE_F_MASTER;
         return 1;
      }
      if (is_work_ram(op1))
      {
         b->kind_lines--;
         raw_add(b, op1, op2, 2);
      }
      return !b->err;
   case 0x1:   /* master code: where the device hooks the game.  Codes run
                * once a frame here, so the address is not needed. */
      b->out->flags |= CE_F_MASTER;
      return 1;
   case 0x2:
      cheat = op_new(b, CE_OR, 2, address, op2);
      break;
   case 0x3:
      cheat = op_new(b, CE_ASSIGN, 1, address, op2 & 0xFF);
      break;
   case 0x4:   /* slide: parameters on the next line */
      cheat = op_new(b, CE_ASSIGN, 2, address, op2);
      b->incomplete = op_index(b);
      break;
   case 0x5:   /* super code: op2 halfwords of data follow */
      if (!op2)
         return 1;
      cheat = op_new(b, CE_ASSIGN, 2, address, 0);
      b->incomplete = op_index(b);
      b->remaining = op2;
      break;
   case 0x6:
      cheat = op_new(b, CE_AND, 2, address, op2);
      break;
   case 0x7:
      cheat = op_new(b, CE_IF_EQ, 2, address, op2);
      b->cb_cond_new = op_index(b);
      break;
   case 0x8:
      cheat = op_new(b, CE_ASSIGN, 2, address, op2);
      break;
   case 0x9:   /* every following line is encrypted with these seeds */
      cb_reseed(b, op1, op2);
      b->out->flags |= CE_F_MASTER;
      return 1;
   case 0xA:
      cheat = op_new(b, CE_IF_NE, 2, address, op2);
      b->cb_cond_new = op_index(b);
      break;
   case 0xB:
      cheat = op_new(b, CE_IF_UGT, 2, address, op2);
      b->cb_cond_new = op_index(b);
      break;
   case 0xC:
      cheat = op_new(b, CE_IF_ULT, 2, address, op2);
      b->cb_cond_new = op_index(b);
      break;
   case 0xD:   /* buttons.  0x20 as mGBA and GBATEK (run the next code while
                * all of them are held); 0x10 and 0x00 as gpSP / VBA-M. */
      if (bad)
         return 1;
      switch ((op1 >> 4) & 0xF)
      {
      case 2:
         cheat = op_new(b, CE_IF_NAND, 2, 0x04000130, op2);
         break;
      case 1:
         cheat = op_new(b, CE_IF_AND, 2, 0x04000130, op2);
         break;
      case 0:
         cheat = op_new(b, CE_IF_NE, 2, 0x04000130, op2);
         break;
      }
      b->cb_cond_new = op_index(b);
      break;
   case 0xE:   /* add; an odd address means 32 bits, value sign-extended */
      if (address & 1)
         cheat = op_new(b, CE_ADD, 4, address & ~1u,
                        (uint32_t)(int32_t)(int16_t)op2);
      else
         cheat = op_new(b, CE_ADD, 2, address, op2);
      break;
   case 0xF:
      cheat = op_new(b, CE_IF_AND, 2, address, op2);
      b->cb_cond_new = op_index(b);
      break;
   }
   (void)cheat;
   return !b->err;
}

/* ======================================================================== */
/* GameShark Advance / Action Replay v1-v2 (mGBA gameshark.c)               */
/* ======================================================================== */

const uint32_t cheat_gsa1_default_seeds[4] =
   { 0x09F4FBBD, 0x9681884A, 0x352027E9, 0xF3DEE5A7 };
const uint32_t cheat_par3_default_seeds[4] =
   { 0x7AA9648F, 0x7FAE6994, 0xC0EFAAD5, 0x42712C57 };

/* http://en.wikipedia.org/wiki/Tiny_Encryption_Algorithm */
void cheat_gsa_encrypt(uint32_t *op1, uint32_t *op2, const uint32_t seeds[4])
{
   uint32_t sum = 0;
   int i;
   for (i = 0; i < 32; ++i)
   {
      sum += 0x9E3779B9;
      *op1 += ((*op2 << 4) + seeds[0]) ^ (*op2 + sum) ^ ((*op2 >> 5) + seeds[1]);
      *op2 += ((*op1 << 4) + seeds[2]) ^ (*op1 + sum) ^ ((*op1 >> 5) + seeds[3]);
   }
}

void cheat_gsa_decrypt(uint32_t *op1, uint32_t *op2, const uint32_t seeds[4])
{
   uint32_t sum = 0xC6EF3720;
   int i;
   for (i = 0; i < 32; ++i)
   {
      *op2 -= ((*op1 << 4) + seeds[2]) ^ (*op1 + sum) ^ ((*op1 >> 5) + seeds[3]);
      *op1 -= ((*op2 << 4) + seeds[0]) ^ (*op2 + sum) ^ ((*op2 >> 5) + seeds[1]);
      sum -= 0x9E3779B9;
   }
}

static void gsa_reseed(uint32_t *seeds, uint16_t params, const uint8_t *t1,
                       const uint8_t *t2)
{
   int x, y;
   int s0 = params >> 8;
   int s1 = params & 0xFF;
   for (y = 0; y < 4; ++y)
   {
      for (x = 0; x < 4; ++x)
      {
         uint8_t z = (uint8_t)(t1[(s0 + x) & 0xFF] + t2[(s1 + y) & 0xFF]);
         seeds[y] <<= 8;
         seeds[y] |= z;
      }
   }
}

static int gsa1_probability(uint32_t op1, uint32_t op2)
{
   int probability = 0;
   uint32_t address = op1 & 0x0FFFFFFF;
   if (op2 == 0x001DC0DE)
      return 0x100;
   /* GBAdhoc: the seed change, as in the Action Replay's table below */
   if (op1 == 0xDEADFACE && !(op2 & 0xFFFF0000))
      return 0x100;
   switch (op1 >> 28)
   {
   case 0x0:
      probability += 0x20;
      if (op2 & 0xFFFFFF00)
         probability -= 0x10;
      probability += addr_score(address);
      break;
   case 0x1:
      probability += 0x20;
      if (op2 & 0xFFFF0000)
         probability -= 0x10;
      probability += addr_score(address);
      break;
   case 0x2:
      probability += 0x20;
      probability += addr_score(address);
      break;
   /* GBAdhoc: the next four are stricter than mGBA's.  A line decrypted
    * with the wrong seeds is noise, and noise must not read as a ROM patch
    * or a hook one time in sixteen. */
   case 0x3:
      probability += (op1 & 0x0FFF0000) ? -0x40 : 0x20;
      break;
   case 0x6:
      probability += (op2 & 0xFFFF0000) ? -0x40 : 0x20;
      break;
   case 0x8:
      switch (op1 & 0x00F00000)
      {
      case 0x00100000:
      case 0x00200000:
         probability += 0x10 + addr_score(op1 & 0x0F0FFFFF);
         break;
      case 0x00F00000:
         probability += (op1 & 0x0F0FFFFF) ? -0x40 : 0x10;
         break;
      default:
         probability -= 0x40;
      }
      break;
   case 0xD:
      probability += 0x20;
      if (op2 & 0xFFCF0000)
         probability -= 0x10;
      probability += addr_score(address);
      break;
   case 0xE:
      probability += 0x20;
      probability += addr_score(op2);
      if (op1 & 0x0F000000)
         probability -= 0x10;
      break;
   case 0xF:
      probability += (op2 & 0xFFFF0000) ? -0x40 : 0x20;
      break;
   default:
      probability -= 0x40;
   }
   return probability;
}

static int gsa1_add_raw(gba_build *b, uint32_t op1, uint32_t op2)
{
   unsigned type = op1 >> 28;
   cheat_op *cheat;
   cheat_patch *patch;

   if (b->err)
      return 0;

   /* GBAdhoc: the address list of a type-3 code.  mGBA's first entry
    * writes to the VALUE as if it were an address and so drops the last
    * address of the list. */
   if (b->remaining > 0)
   {
      op_new(b, CE_ASSIGN, 4, op1, b->list_value);
      --b->remaining;
      if (b->remaining > 0)
      {
         op_new(b, CE_ASSIGN, 4, op2, b->list_value);
         --b->remaining;
      }
      return !b->err;
   }

   if (op2 == 0x001DC0DE)        /* the game's id line */
   {
      b->out->flags |= CE_F_MASTER;
      return 1;
   }

   switch (type)
   {
   case 0x0:
      op_new(b, CE_ASSIGN, 1, op1 & 0x0FFFFFFF, op2 & 0xFF);
      break;
   case 0x1:
      op_new(b, CE_ASSIGN, 2, op1 & 0x0FFFFFFF, op2 & 0xFFFF);
      break;
   case 0x2:
      op_new(b, CE_ASSIGN, 4, op1 & 0x0FFFFFFF, op2);
      break;
   case 0x3:
      if ((op1 >> 16) & 0xFF)
      {
         b->err = CE_E_UNSUPPORTED;
         return 0;
      }
      b->remaining = (int)(op1 & 0xFFFF);
      b->list_value = op2;
      break;
   case 0x6:
      patch = patch_append(b);
      patch->address = 0x08000000 | ((op1 & 0xFFFFFF) << 1);
      patch->value = op2 & 0xFFFF;
      patch->width = 2;
      break;
   case 0x8:
      switch (op1 & 0x00F00000)
      {
      case 0x00100000:
         op_new(b, CE_IF_BUTTON, 0, 0, 0);
         op_new(b, CE_ASSIGN, 1, op1 & 0x0F0FFFFF, op2 & 0xFF);
         break;
      case 0x00200000:
         op_new(b, CE_IF_BUTTON, 0, 0, 0);
         op_new(b, CE_ASSIGN, 2, op1 & 0x0F0FFFFF, op2 & 0xFFFF);
         break;
      default:               /* 0x00F00000: slow motion, and the rest */
         b->err = CE_E_UNSUPPORTED;
         return 0;
      }
      b->out->flags |= CE_F_BUTTON;
      break;
   case 0xD:
      if (op1 == 0xDEADFACE)
      {
         gsa_reseed(b->gsa_seeds, (uint16_t)op2, ce_gsa1T1, ce_gsa1T2);
         b->out->flags |= CE_F_MASTER;
         return 1;
      }
      cheat = op_new(b, CE_IF_EQ, 2, op1 & 0x0FFFFFFF, op2 & 0xFFFF);
      switch ((op2 >> 20) & 0xF)
      {
      case 0: cheat->type = CE_IF_EQ; break;
      case 1: cheat->type = CE_IF_NE; break;
      case 2: cheat->type = CE_IF_ULE; break;
      case 3: cheat->type = CE_IF_UGE; break;
      default:
         b->err = CE_E_UNSUPPORTED;
         return 0;
      }
      break;
   case 0xE:
      cheat = op_new(b, CE_IF_EQ, 2, op2 & 0x0FFFFFFF, op1 & 0xFFFF);
      cheat->repeat = (op1 >> 16) & 0xFF;
      break;
   case 0xF:                  /* hook */
      b->out->flags |= CE_F_MASTER;
      return 1;
   default:
      b->err = CE_E_UNSUPPORTED;
      return 0;
   }
   return !b->err;
}

/* ======================================================================== */
/* Pro Action Replay v3 (mGBA parv3.c)                                      */
/* ======================================================================== */

#define PAR3_COND          0x38000000u
#define PAR3_WIDTH         0x06000000u
#define PAR3_ACTION        0xC0000000u
#define PAR3_BASE          0xC0000000u
#define PAR3_WIDTH_BASE    25

static uint32_t par_addr(uint32_t x)
{
   return (x & 0xFFFFF) | ((x << 4) & 0x0F000000);
}

static uint32_t width_mask(int width)
{
   return width >= 4 ? 0xFFFFFFFFu : (0xFFFFFFFFu >> ((4 - width) * 8));
}

static void par_end_block(gba_build *b)
{
   unsigned size = b->out->n_ops - (unsigned)b->current_block - 1;
   cheat_op *cur = &b->out->ops[b->current_block];
   if (cur->repeat)
      cur->negative_repeat = size - cur->repeat;
   else
      cur->repeat = size;
   b->current_block = COMPLETE;
}

static void par_else_block(gba_build *b)
{
   unsigned size = b->out->n_ops - (unsigned)b->current_block - 1;
   b->out->ops[b->current_block].repeat = size;
}

static int par3_add_cond(gba_build *b, uint32_t op1, uint32_t op2)
{
   uint32_t condition = op1 & PAR3_COND;
   int width = 1 << ((op1 & PAR3_WIDTH) >> PAR3_WIDTH_BASE);
   cheat_op *cheat;

   if ((op1 & PAR3_ACTION) == 0xC0000000u)
   {
      /* "turn every code off": not supported (nor by mGBA) */
      b->err = CE_E_UNSUPPORTED;
      return 0;
   }

   cheat = op_append(b);
   cheat->address = par_addr(op1);
   cheat->width = (uint8_t)width;
   cheat->operand = op2 & width_mask(width);

   switch (op1 & PAR3_ACTION)
   {
   case 0x00000000u:
      cheat->repeat = 1;
      break;
   case 0x40000000u:
      cheat->repeat = 2;
      break;
   case 0x80000000u:
      cheat->repeat = 0;
      if (b->current_block != COMPLETE)
         par_end_block(b);
      b->current_block = op_index(b);
      break;
   }

   switch (condition)
   {
   case 0x08000000u: cheat->type = CE_IF_EQ; break;
   case 0x10000000u: cheat->type = CE_IF_NE; break;
   case 0x18000000u: cheat->type = CE_IF_LT; break;
   case 0x20000000u: cheat->type = CE_IF_GT; break;
   case 0x28000000u: cheat->type = CE_IF_ULT; break;
   case 0x30000000u: cheat->type = CE_IF_UGT; break;
   case 0x38000000u: cheat->type = CE_IF_AND; break;
   default:          cheat->type = CE_IF_LAND; cheat->operand = 0; break;
   }

   if (width > 4)
   {
      cheat->width = 0;
      cheat->type = CE_NEVER;
   }
   return !b->err;
}

static int par3_add_special(gba_build *b, uint32_t op2)
{
   int rom_patch = 0;
   int width = 0;
   cheat_op *cheat;

   switch (op2 & 0xFF000000)
   {
   case 0x08000000u:           /* slow motion */
      b->err = CE_E_UNSUPPORTED;
      return 0;
   case 0x10000000u: width = 1; goto button;
   case 0x12000000u: width = 2; goto button;
   case 0x14000000u: width = 4;
   button:
      op_new(b, CE_IF_BUTTON, 0, 0, 0);
      cheat = op_new(b, CE_ASSIGN, width, par_addr(op2), 0);
      (void)cheat;
      b->incomplete = op_index(b);
      b->out->flags |= CE_F_BUTTON;
      break;
   case 0x18000000u:
   case 0x1A000000u:
   case 0x1C000000u:
   case 0x1E000000u:
      rom_patch = 1;
      break;
   case 0x40000000u:           /* end if */
      if (b->current_block != COMPLETE)
      {
         par_end_block(b);
         return 1;
      }
      b->err = CE_E_FORMAT;
      return 0;
   case 0x60000000u:           /* else */
      if (b->current_block != COMPLETE)
      {
         par_else_block(b);
         return 1;
      }
      b->err = CE_E_FORMAT;
      return 0;
   case 0x80000000u: width = 1; goto fill;
   case 0x82000000u: width = 2; goto fill;
   case 0x84000000u: width = 4;
   fill:
      cheat = op_new(b, CE_ASSIGN, width, par_addr(op2), 0);
      (void)cheat;
      b->incomplete = op_index(b);
      break;
   default:                    /* 00000000 00000000: end of the code list */
      break;
   }
   if (rom_patch)
   {
      cheat_patch *patch = patch_append(b);
      patch->address = 0x08000000 | ((op2 & 0xFFFFFF) << 1);
      patch->width = 2;
      b->incomplete_patch = (int)b->out->n_patches - 1;
   }
   return !b->err;
}

static int par3_add_raw(gba_build *b, uint32_t op1, uint32_t op2)
{
   int width;
   cheat_op *cheat;

   if (b->err)
      return 0;
   if (b->incomplete_patch != COMPLETE)
   {
      b->out->patches[b->incomplete_patch].value = op1 & 0xFFFF;
      b->incomplete_patch = COMPLETE;
      return 1;
   }
   if (b->incomplete != COMPLETE)
   {
      cheat_op *inc = &b->out->ops[b->incomplete];
      inc->operand = op1 & width_mask(inc->width);
      if (b->incomplete > 0 &&
          b->out->ops[b->incomplete - 1].type == CE_IF_BUTTON)
      {
         b->incomplete = COMPLETE;
         return 1;
      }
      inc->operand_offset = (int32_t)(op2 >> 24);
      inc->repeat = (op2 >> 16) & 0xFF;
      inc->address_offset = (int32_t)((op2 & 0xFFFF) * inc->width);
      b->incomplete = COMPLETE;
      return 1;
   }

   if (op2 == 0x001DC0DE)
   {
      b->out->flags |= CE_F_MASTER;
      return 1;
   }
   if (op1 == 0x00000000)
      return par3_add_special(b, op2);
   if (op1 == 0xDEADFACE)
   {
      gsa_reseed(b->gsa_seeds, (uint16_t)op2, ce_par3T1, ce_par3T2);
      b->out->flags |= CE_F_MASTER;
      return 1;
   }
   if (op1 >> 24 == 0xC4)       /* hook */
   {
      b->out->flags |= CE_F_MASTER;
      return 1;
   }
   if (op1 & PAR3_COND)
      return par3_add_cond(b, op1, op2);

   width = 1 << ((op1 & PAR3_WIDTH) >> PAR3_WIDTH_BASE);
   if ((op1 & 0x01000000) && (op1 & 0xFE000000) != 0xC6000000)
   {
      /* Bit 24 means nothing outside C6/C7.  mGBA refuses a code that has
       * it set, VBA-M ignores the bit; published codes with it exist
       * (German Pokemon Emerald).  GBAdhoc: mGBA's way, except in a list
       * known to be an Action Replay's. */
      if (!ce_lenient)
      {
         b->err = CE_E_UNSUPPORTED;
         return 0;
      }
      op1 &= ~0x01000000u;
   }
   if ((op1 & PAR3_BASE) == 0xC0000000u ? (op1 >> 24) < 0xC6 : width > 4)
   {
      b->err = CE_E_UNSUPPORTED;
      return 0;
   }

   if (op1 == 0x80000000 && !op2)
      return 1;                 /* padding: add nothing to nowhere */

   cheat = op_append(b);
   cheat->address = par_addr(op1);
   cheat->repeat = 1;

   switch (op1 & PAR3_BASE)
   {
   case 0x00000000u:
      cheat->type = CE_ASSIGN;
      cheat->address_offset = width;
      if (width < 4)
         cheat->repeat = (op2 >> (width * 8)) + 1;
      break;
   case 0x40000000u:
      cheat->type = CE_ASSIGN_INDIRECT;
      if (width < 4)
         cheat->address_offset = (int32_t)((op2 >> (width * 8)) * (unsigned)width);
      break;
   case 0x80000000u:
      cheat->type = CE_ADD;
      break;
   case 0xC0000000u:
      /* GBAdhoc: C6 writes 16 bits and C7 32 bits to an I/O register
       * (mGBA: 8 and 16). */
      width = (op1 & 0x01000000) ? 4 : 2;
      cheat->type = CE_ASSIGN;
      cheat->address = 0x04000000 | (op1 & 0x00FFFFFF);
      break;
   }
   cheat->width = (uint8_t)width;
   cheat->operand = op2 & width_mask(width);
   return !b->err;
}

static int par3_probability(uint32_t op1, uint32_t op2)
{
   int probability = 0;
   int width;
   if (op2 == 0x001DC0DE)
      return 0x100;
   if (op1 == 0xDEADFACE && !(op2 & 0xFFFF0000))
      return 0x100;
   if (!op1)
   {
      uint32_t address = par_addr(op2);
      probability += 0x40;
      switch (op2 & 0xFE000000)
      {
      case 0x80000000u:
      case 0x82000000u:
      case 0x84000000u:
         probability += addr_score(address);
         break;
      case 0x18000000u:
      case 0x1A000000u:
      case 0x1C000000u:
      case 0x1E000000u:
         break;
      case 0x00000000u:
      case 0x08000000u:
      case 0x10000000u:
      case 0x12000000u:
      case 0x14000000u:
      case 0x40000000u:
      case 0x60000000u:
         if (op2 & 0x01000000)
            probability -= 0x40;
         break;
      default:
         probability -= 0x40;
         break;
      }
      return probability;
   }
   width = (int)((op1 & PAR3_WIDTH) >> (PAR3_WIDTH_BASE - 3));
   if (op1 & PAR3_COND)
   {
      probability += 0x20;
      if (width >= 24)
         return 0;
      if (op2 & ~((1u << width) - 1))
         probability -= 0x10;
      /* GBAdhoc: a condition tests memory too */
      if (addr_score(par_addr(op1)) <= 0)
         probability -= 0x80;
   }
   else
   {
      uint32_t address = par_addr(op1);
      /* GBAdhoc: "80000000 00000000", add nothing to nowhere, pads some
       * published codes; it says nothing about the entry either way. */
      if ((op1 & PAR3_BASE) == 0x80000000u && !op2 && !(op1 & 0x00FFFFFF))
         return 0;
      probability += 0x20;
      /* GBAdhoc: of the Cx codes only the hook and the two I/O writes
       * exist, each with a shape */
      if ((op1 & PAR3_BASE) == 0xC0000000u)
      {
         switch (op1 >> 24)
         {
         case 0xC4:
            return (op2 & 0xFFFF0000) ? -0x40 : probability;
         case 0xC6:
         case 0xC7:
            return (op1 & 0x00FFFFFF) < 0x400 ? probability + 0x10 : -0x40;
         default:
            return -0x40;
         }
      }
      switch (op1 & PAR3_BASE)
      {
      case 0x80000000u:
         if (width < 24 && (op2 & ~((1u << width) - 1)))
            probability -= 0x10;
         /* fall through */
      case 0x00000000u:
      case 0x40000000u:
         probability += addr_score(address);
         if ((op1 & 0x01000000) && !ce_lenient)
            return 0;
         break;
      case 0xC0000000u:
         break;
      }
   }
   return probability;
}

/* ======================================================================== */
/* One entry, one interpretation                                            */
/* ======================================================================== */

enum
{
   V_GSA1 = 1, V_GSA1_RAW = 2, V_PAR3 = 3, V_PAR3_RAW = 4, V_CB = 5
};

typedef struct candidate
{
   int version;
   const uint32_t *gsa_seeds;         /* V_GSA1 / V_PAR3 */
   int cb_set;                        /* V_CB: index in ctx, -1 = plain */
   int sure;                          /* the list has this device's id line */
} candidate;

static void build_finish(gba_build *b)
{
   unsigned i, n;

   if (b->err)
      return;
   if (b->incomplete != COMPLETE || b->incomplete_patch != COMPLETE ||
       b->remaining > 0)
   {
      b->err = CE_E_TRUNCATED;   /* the parameter line is missing */
      return;
   }
   cb_cond_close(b);
   /* GBAdhoc: an "if" block nobody closed guards the rest of the entry. */
   if (b->current_block != COMPLETE)
      par_end_block(b);

   /* GBAdhoc: a condition never reaches past its own entry (entries are
    * run one after the other from one array). */
   n = b->out->n_ops;
   for (i = 0; i < n; i++)
   {
      cheat_op *c = &b->out->ops[i];
      unsigned room = n - 1 - i;
      if (c->type < CE_IF_EQ)
         continue;
      if (c->repeat > room)
         c->repeat = room;
      if (c->negative_repeat > room - c->repeat)
         c->negative_repeat = room - c->repeat;
   }
   if (!b->out->n_ops && !b->out->n_patches)
      b->out->flags |= CE_F_NOOP;
}

static void run_candidate(gba_build *b, cheat_out *out, const gline *L,
                          unsigned n, const candidate *cand,
                          const cheat_gba_ctx *ctx, int raw32)
{
   unsigned i;

   build_init(b, out);
   ce_lenient = cand->sure;
   if (cand->gsa_seeds)
      memcpy(b->gsa_seeds, cand->gsa_seeds, sizeof(b->gsa_seeds));
   if (cand->version == V_CB && cand->cb_set >= 0 && ctx)
   {
      b->cb_master = ctx->cb[cand->cb_set].master;
      memcpy(b->cb_seeds, ctx->cb[cand->cb_set].seeds, sizeof(b->cb_seeds));
      memcpy(b->cb_table, ctx->cb[cand->cb_set].table, sizeof(b->cb_table));
   }

   for (i = 0; i < n && !b->err; i++)
   {
      uint32_t op1 = L[i].a, op2 = L[i].b;
      int in_data = b->incomplete != COMPLETE ||
                    b->incomplete_patch != COMPLETE || b->remaining > 0;

      /* Lines that are raw whatever the entry's kind. */
      if (L[i].colon || L[i].bdigits == 2)
      {
         if (in_data)
         {
            b->err = CE_E_FORMAT;
            break;
         }
         raw_add(b, op1, op2, L[i].bdigits / 2);
         continue;
      }

      if (L[i].bdigits == 4)
      {
         if (cand->version != V_CB)
         {
            b->err = CE_E_FORMAT;
            break;
         }
         cb_add(b, op1, (uint16_t)op2);
         continue;
      }

      /* 8+8 */
      if (cand->version == V_CB)
      {
         b->err = CE_E_FORMAT;
         break;
      }
      if (raw32 && !in_data)
      {
         raw_add(b, op1, op2, raw32);
         continue;
      }
      b->kind_lines++;
      if (cand->version == V_GSA1 || cand->version == V_PAR3)
      {
         cheat_gsa_decrypt(&op1, &op2, b->gsa_seeds);
         b->out->flags |= CE_F_DECRYPTED;
      }
      if (cand->version == V_GSA1 || cand->version == V_GSA1_RAW)
      {
         if (!in_data)
         {
            b->score += gsa1_probability(op1, op2);
            b->scored++;
         }
         gsa1_add_raw(b, op1, op2);
      }
      else
      {
         if (!in_data)
         {
            b->score += par3_probability(op1, op2);
            b->scored++;
         }
         par3_add_raw(b, op1, op2);
      }
   }
   build_finish(b);
   ce_lenient = 0;
}

int cheat_gba_compile_as(const char *code, int version, cheat_out *out)
{
   gline *L = ce_lines;
   unsigned n = 0;
   gba_build b;
   candidate cand;
   int rc = gba_lines(code, L, GBA_MAX_LINES, &n);

   out->n_ops = out->n_patches = 0;
   out->flags = 0;
   if (rc != CE_OK)
      return rc;
   memset(&cand, 0, sizeof(cand));
   cand.version = version;
   cand.cb_set = -1;
   if (version == V_GSA1)
      cand.gsa_seeds = cheat_gsa1_default_seeds;
   else if (version == V_PAR3)
      cand.gsa_seeds = cheat_par3_default_seeds;
   run_candidate(&b, out, L, n, &cand, NULL, 0);
   return b.err;
}

/* ======================================================================== */
/* Seeds from the whole list                                                */
/* ======================================================================== */

void cheat_gba_ctx_init(cheat_gba_ctx *ctx)
{
   memset(ctx, 0, sizeof(*ctx));
   memcpy(ctx->gsa1[0], cheat_gsa1_default_seeds, sizeof(ctx->gsa1[0]));
   memcpy(ctx->par3[0], cheat_par3_default_seeds, sizeof(ctx->par3[0]));
   ctx->n_gsa1 = 1;
   ctx->n_par3 = 1;
}

static void ctx_add_gsa(uint32_t sets[CE_SEEDSETS][4], unsigned *n,
                        const uint32_t *seeds)
{
   unsigned i;
   for (i = 0; i < *n; i++)
      if (!memcmp(sets[i], seeds, 4 * sizeof(uint32_t)))
         return;
   if (*n >= CE_SEEDSETS)
      return;
   memcpy(sets[(*n)++], seeds, 4 * sizeof(uint32_t));
}

void cheat_gba_ctx_scan(cheat_gba_ctx *ctx, const char *code)
{
   gline *L = ce_lines;
   unsigned n = 0, i, k;

   if (gba_lines(code, L, GBA_MAX_LINES, &n) != CE_OK)
      return;

   for (i = 0; i < n; i++)
   {
      if (L[i].colon)
         continue;
      if (L[i].bdigits == 8)
      {
         /* DEADFACE 0000xxxx, encrypted with a set already known (the
          * master code is encrypted with the device's default seeds) or
          * plain. */
         uint32_t seeds[4];
         unsigned n1 = ctx->n_gsa1, n3 = ctx->n_par3;
         for (k = 0; k < n1; k++)
         {
            uint32_t o1 = L[i].a, o2 = L[i].b;
            cheat_gsa_decrypt(&o1, &o2, ctx->gsa1[k]);
            if (o2 == 0x001DC0DE)
               ctx->gsa1_sure[k] = 1;
            if (o1 == 0xDEADFACE && !(o2 & 0xFFFF0000))
            {
               memset(seeds, 0, sizeof(seeds));
               gsa_reseed(seeds, (uint16_t)o2, ce_gsa1T1, ce_gsa1T2);
               ctx_add_gsa(ctx->gsa1, &ctx->n_gsa1, seeds);
            }
         }
         for (k = 0; k < n3; k++)
         {
            uint32_t o1 = L[i].a, o2 = L[i].b;
            cheat_gsa_decrypt(&o1, &o2, ctx->par3[k]);
            if (o2 == 0x001DC0DE)
               ctx->par3_sure[k] = 1;
            if (o1 == 0xDEADFACE && !(o2 & 0xFFFF0000))
            {
               memset(seeds, 0, sizeof(seeds));
               gsa_reseed(seeds, (uint16_t)o2, ce_par3T1, ce_par3T2);
               ctx_add_gsa(ctx->par3, &ctx->n_par3, seeds);
            }
         }
         if (L[i].a == 0xDEADFACE && !(L[i].b & 0xFFFF0000))
         {
            memset(seeds, 0, sizeof(seeds));
            gsa_reseed(seeds, (uint16_t)L[i].b, ce_gsa1T1, ce_gsa1T2);
            ctx_add_gsa(ctx->gsa1, &ctx->n_gsa1, seeds);
            memset(seeds, 0, sizeof(seeds));
            gsa_reseed(seeds, (uint16_t)L[i].b, ce_par3T1, ce_par3T2);
            ctx_add_gsa(ctx->par3, &ctx->n_par3, seeds);
         }
      }
      else if (L[i].bdigits == 4)
      {
         /* 9xxxxxxx yyyy.  Only where it can be one: as the entry's first
          * line, or after nothing but game-id and hook lines -- one line in
          * sixteen of an ENCRYPTED code begins with a 9 too. */
         unsigned t = L[i].a >> 28;
         if (t == 0x9)
         {
            gba_build b;
            cheat_out scratch;
            memset(&scratch, 0, sizeof(scratch));
            build_init(&b, &scratch);
            cb_reseed(&b, L[i].a, (uint16_t)L[i].b);
            for (k = 0; k < ctx->n_cb; k++)
               if (ctx->cb[k].master == b.cb_master &&
                   !memcmp(ctx->cb[k].seeds, b.cb_seeds, sizeof(b.cb_seeds)))
                  break;
            if (k == ctx->n_cb && ctx->n_cb < CE_SEEDSETS)
            {
               ctx->cb[k].master = b.cb_master;
               memcpy(ctx->cb[k].seeds, b.cb_seeds, sizeof(b.cb_seeds));
               memcpy(ctx->cb[k].table, b.cb_table, sizeof(b.cb_table));
               ctx->n_cb++;
            }
            break;            /* what follows it is encrypted */
         }
         if (!((t == 0x0 && (L[i].a >> 16) == 0) ||
               (t == 0x1 && (L[i].a & 0x0FFFFFFF) < 0x02000000 &&
                L[i].b < 0x100)))
            break;
      }
   }
}

/* ======================================================================== */
/* One entry: pick the interpretation                                       */
/* ======================================================================== */

int cheat_gba_compile(const char *code, const cheat_gba_ctx *ctx,
                      cheat_out *out, int *kind)
{
   gline *L = ce_lines;
   static cheat_gba_ctx default_ctx;
   unsigned n = 0, i;
   unsigned n4 = 0, n8 = 0, nraw = 0;
   gba_build b;
   candidate cands[2 * CE_SEEDSETS + 2];
   unsigned n_cands = 0;
   int best = -1, raw32 = 0;
   int rc;

   if (kind)
      *kind = CE_KIND_UNKNOWN;
   out->n_ops = out->n_patches = 0;
   out->flags = 0;

   rc = gba_lines(code, L, GBA_MAX_LINES, &n);
   if (rc != CE_OK)
      return rc;
   if (!ctx)
   {
      cheat_gba_ctx_init(&default_ctx);
      ctx = &default_ctx;
   }

   for (i = 0; i < n; i++)
   {
      if (L[i].colon || L[i].bdigits == 2)
         nraw++;
      else if (L[i].bdigits == 4)
         n4++;
      else
         n8++;
   }
   if (n4 && n8)
      return CE_E_FORMAT;       /* two devices' codes in one entry */

   memset(cands, 0, sizeof(cands));
   if (!n4 && !n8)
   {
      cands[0].version = V_CB;  /* raw lines only: any parser will do */
      cands[0].cb_set = -1;
      run_candidate(&b, out, L, n, &cands[0], ctx, 0);
      if (kind)
         *kind = CE_KIND_GBA_RAW;
      return b.err;
   }

   if (n4)
   {
      /* CodeBreaker: plain, or decrypted with one of the game's seed sets.
       * Plain wins when every line of it makes sense. */
      long best_val = 0;
      int best_err = CE_E_FORMAT;
      cands[n_cands].version = V_CB;
      cands[n_cands++].cb_set = -1;
      for (i = 0; i < ctx->n_cb; i++)
      {
         cands[n_cands].version = V_CB;
         cands[n_cands++].cb_set = (int)i;
      }
      for (i = 0; i < n_cands; i++)
      {
         long val;
         run_candidate(&b, out, L, n, &cands[i], ctx, 0);
         val = (long)b.plausible - 4 * (long)b.implausible;
         if (b.err)
            val -= 1000;
         if (best < 0 || val > best_val)
         {
            best = (int)i;
            best_val = val;
            best_err = b.err;
         }
         if (!b.err && !b.implausible)
            break;               /* first clean reading, plain first */
      }
      run_candidate(&b, out, L, n, &cands[best], ctx, 0);
      (void)best_err;
      if (kind)
         *kind = (b.kind_lines || !b.raw_lines) ? CE_KIND_GBA_CODEBREAKER
                                                : CE_KIND_GBA_RAW;
      if (b.implausible)
      {
         /* Addresses that are not memory: an encrypted code whose seed
          * code (9xxxxxxx yyyy, in the game's master code) is not in the
          * list. */
         out->n_ops = out->n_patches = 0;
         return CE_E_ENCRYPTED;
      }
      if (b.err)
      {
         out->n_ops = out->n_patches = 0;
         return b.err;
      }
      return CE_OK;
   }

   /* 8+8.  GBAdhoc: a plain "work RAM address, value" entry is a raw write
    * -- of 32 bits when a value needs them, else of one byte (which is
    * also what a raw GameShark line 0aaaaaaa 000000vv means). */
   {
      int all_ram = 1, wide = 0;
      for (i = 0; i < n; i++)
      {
         if (L[i].colon || L[i].bdigits == 2)
            continue;
         if (!is_work_ram(L[i].a))
            all_ram = 0;
         if (L[i].b > 0xFF)
            wide = 1;
      }
      if (all_ram)
         raw32 = wide ? 4 : 1;
   }
   if (raw32)
   {
      cands[0].version = V_GSA1_RAW;
      run_candidate(&b, out, L, n, &cands[0], ctx, raw32);
      if (kind)
         *kind = CE_KIND_GBA_RAW;
      return b.err;
   }

   /* mGBA's order, and like there a later reading must be strictly more
    * plausible to replace an earlier one. */
   for (i = 0; i < ctx->n_gsa1; i++)
   {
      cands[n_cands].version = V_GSA1;
      cands[n_cands].sure = ctx->gsa1_sure[i];
      cands[n_cands++].gsa_seeds = ctx->gsa1[i];
   }
   for (i = 0; i < ctx->n_par3; i++)
   {
      cands[n_cands].version = V_PAR3;
      cands[n_cands].sure = ctx->par3_sure[i];
      cands[n_cands++].gsa_seeds = ctx->par3[i];
   }
   cands[n_cands++].version = V_GSA1_RAW;
   cands[n_cands++].version = V_PAR3_RAW;

   {
      /* The most plausible reading, by average per code line.  One that
       * does not parse still competes: an unsupported code type must be
       * reported as that, not replaced by a noise reading that "works". */
      long best_score = 0;
      unsigned best_scored = 1;
      int best_err = 0;
      for (i = 0; i < n_cands; i++)
      {
         long s;
         unsigned c, k, effect = 0;
         int err;
         run_candidate(&b, out, L, n, &cands[i], ctx, 0);
         s = b.score;
         c = b.scored ? b.scored : 1;
         err = b.err;
         /* A reading that parses must DO something (or be a master code):
          * tests guarding nothing are what noise looks like. */
         for (k = 0; k < out->n_ops; k++)
            if (out->ops[k].type < CE_IF_EQ)
               effect = 1;
         if (!err && !effect && !out->n_patches && !(out->flags & CE_F_MASTER))
            err = CE_E_FORMAT;
#ifdef CE_DEBUG
         fprintf(stderr, "cand %u v%d: score %ld/%u err %d ops %u patches %u\n",
                 i, cands[i].version, s, c, err, out->n_ops, out->n_patches);
#endif
         /* compare averages: s/c > best_score/best_scored; a tie goes to
          * the reading that parses, then to the earlier one */
         if (best < 0 || s * (long)best_scored > best_score * (long)c ||
             (s * (long)best_scored == best_score * (long)c && best_err && !err))
         {
            best = (int)i;
            best_score = s;
            best_scored = c;
            best_err = err;
         }
      }
      if (best_score <= 0 || best_err)
      {
         out->n_ops = out->n_patches = 0;
         out->flags = 0;
         /* no reading of it names real memory: not a code at all */
         return best_score > 0 ? best_err : CE_E_FORMAT;
      }
      run_candidate(&b, out, L, n, &cands[best], ctx, 0);
      if (kind)
         *kind = (cands[best].version == V_PAR3 ||
                  cands[best].version == V_PAR3_RAW) ? CE_KIND_GBA_AR3
                                                     : CE_KIND_GBA_GAMESHARK;
   }
   return CE_OK;
}

/* ======================================================================== */
/* Game Boy                                                                 */
/* ======================================================================== */

static int gb_emit(cheat_gb_code *out, unsigned cap, unsigned *n,
                   const cheat_gb_code *c)
{
   if (out)
   {
      if (*n >= cap)
         return CE_E_TOO_LONG;
      out[*n] = *c;
   }
   (*n)++;
   return CE_OK;
}

/* VVA-AAA(-CCC): digits d[0..5] (and d[6..8]). */
static int gb_genie(const uint8_t *d, unsigned nd, cheat_gb_code *c)
{
   unsigned op1 = (unsigned)(d[0] << 8 | d[1] << 4 | d[2]);
   unsigned op2 = (unsigned)(d[3] << 8 | d[4] << 4 | d[5]);
   unsigned address;

   memset(c, 0, sizeof(*c));
   address = (op1 & 0xF) << 8;
   address |= (op2 >> 4) & 0xFF;
   address |= ((op2 & 0xF) ^ 0xF) << 12;
   /* The Game Genie sits on the cartridge's ROM and compares fifteen
    * address lines (Gambatte masks the same way); the database has codes
    * with the sixteenth digit set either way. */
   c->rom = 1;
   c->address = (uint16_t)(address & 0x7FFF);
   c->value = (uint8_t)(op1 >> 4);
   if (nd == 9)
   {
      unsigned op3 = (unsigned)(d[6] << 8 | d[7] << 4 | d[8]);
      uint32_t value = ((uint32_t)(op3 & 0xF00) << 20) | (op3 & 0xF);
      value = ROR32(value, 2);
      value |= value >> 24;
      value ^= 0xBA;
      c->compare = (uint8_t)value;
      c->has_compare = 1;
   }
   return CE_OK;
}

/* One token of a Game Boy entry. */
typedef struct gb_token
{
   uint8_t dig[12];
   uint8_t ndig;
   uint8_t dashes;
   int8_t  colon;        /* digits before the ':' or -1 */
   uint8_t xploder;      /* began with '$' */
} gb_token;

static const char *gb_next(const char *s, gb_token *t, int *err)
{
   const char *p;
   memset(t, 0, sizeof(*t));
   t->colon = -1;
   while (*s && is_sep((unsigned char)*s))
      s++;
   if (!*s)
      return NULL;
   p = s;
   if (*p == '$')
   {
      t->xploder = 1;
      p++;
   }
   for (; *p && !is_sep((unsigned char)*p); p++)
   {
      int h = hexval((unsigned char)*p);
      if (*p == '-')
         t->dashes++;
      else if (*p == ':' && t->colon < 0)
         t->colon = (int8_t)t->ndig;
      else if (h < 0 || t->ndig >= sizeof(t->dig))
         *err = CE_E_FORMAT;
      else
         t->dig[t->ndig++] = (uint8_t)h;
   }
   return p;
}

int cheat_gb_compile(const char *code, cheat_gb_code *out, unsigned cap,
                     unsigned *n_out, int *kind)
{
   /* Game Genie digits gathered across tokens: the database writes a code
    * as "AAA-BBB-CCC", as "AAA-BBB" + "CCC", and as "AAA+BBB+CCC". */
   uint8_t gg[9];
   unsigned ngg = 0;
   unsigned n = 0, n_genie = 0;
   unsigned tokens = 0, bare3 = 0, chunk = 9;
   int err = 0;
   int rc = text_check(code, "-:$");
   const char *s;
   gb_token t;
   cheat_gb_code c;

   if (kind)
      *kind = CE_KIND_UNKNOWN;
   if (n_out)
      *n_out = 0;
   if (rc != CE_OK)
   {
      /* say what the code with the hole in it is */
      if (rc == CE_E_PLACEHOLDER && kind)
         *kind = strchr(code, '-') ? CE_KIND_GB_GAMEGENIE : CE_KIND_GB_GAMESHARK;
      return rc;
   }

   /* A list written with '+' alone is cut in threes, or in twos when only
    * that divides it. */
   for (s = code; (s = gb_next(s, &t, &err)) != NULL; )
   {
      tokens++;
      if (t.ndig == 3 && !t.dashes && t.colon < 0 && !t.xploder)
         bare3++;
   }
   if (err)
      return CE_E_FORMAT;
   if (bare3 == tokens && bare3 % 3 && !(bare3 % 2))
      chunk = 6;

#define GG_FLUSH()                                                           \
   do {                                                                      \
      if (ngg)                                                               \
      {                                                                      \
         if (ngg != 6 && ngg != 9)                                           \
            return CE_E_FORMAT;                                              \
         rc = gb_genie(gg, ngg, &c);                                         \
         if (!rc)                                                            \
            rc = gb_emit(out, cap, &n, &c);                                  \
         if (rc)                                                             \
            return rc;                                                       \
         n_genie++;                                                          \
         ngg = 0;                                                            \
      }                                                                      \
   } while (0)

   for (s = code; (s = gb_next(s, &t, &err)) != NULL; )
   {
      if (t.xploder)
      {
         /* Xploder / Xplorer GB ("$0DVVLLHH"): the value is offset by
          * 0x21 and each address byte goes through a substitution that
          * could not be reconstructed (some fifty of its 256 entries can be
          * read off the database's Xploder / GameShark twins; no formula
          * fits them).
          *
          * LOOKED FOR AGAIN, NOT FOUND (the extras pass): no emulator whose
          * source is at hand decodes these -- mGBA's and VBA-M's Game Boy
          * cheat code know GameShark and Game Genie only, as do Gambatte,
          * SameBoy and TGB Dual -- and neither GameHacking.org's wiki page
          * for the device nor GBATEK (which documents the GBA Xploder, a
          * different cipher) gives the table.  The one place it certainly
          * is: the cartridge's own firmware, of which a dump is public
          * (github.com/LibreShark/sharkdumps, gbc/firmware/gbc-xploder-*).
          * That is a commercial ROM, so it was not fetched; whoever may read
          * it finds a 256-byte table that the fifty known pairs identify. */
         if (kind)
            *kind = CE_KIND_GB_GAMESHARK;
         return t.ndig == 8 ? CE_E_UNSUPPORTED : CE_E_FORMAT;
      }

      if (t.colon >= 0 || (!t.dashes && t.ndig == 8))
      {
         GG_FLUSH();
         memset(&c, 0, sizeof(c));
         if (t.colon >= 0)
         {
            /* "AAAA:VV" */
            if (t.colon != 4 || t.ndig != 6 || t.dashes)
               return CE_E_FORMAT;
            c.type = 0x01;
            c.address = (uint16_t)(t.dig[0] << 12 | t.dig[1] << 8 |
                                   t.dig[2] << 4 | t.dig[3]);
            c.value = (uint8_t)(t.dig[4] << 4 | t.dig[5]);
         }
         else
         {
            /* GameShark TTVVLLHH */
            c.type = (uint8_t)(t.dig[0] << 4 | t.dig[1]);
            c.value = (uint8_t)(t.dig[2] << 4 | t.dig[3]);
            c.address = (uint16_t)(t.dig[4] << 4 | t.dig[5] |
                                   t.dig[6] << 12 | t.dig[7] << 8);
         }
         if (t.colon < 0 && (c.type == 0x0C || c.type == 0x0D))
         {
            /* an Xploder code without its '$' */
            if (kind)
               *kind = CE_KIND_GB_GAMESHARK;
            return CE_E_UNSUPPORTED;
         }
         if (c.address < 0x8000)
            return CE_E_FORMAT;   /* it writes RAM: a ROM address is a typo */
         rc = gb_emit(out, cap, &n, &c);
         if (rc)
            return rc;
         continue;
      }

      /* Game Genie: one, two or three groups of three digits */
      if (!t.ndig || t.ndig % 3 || t.ndig > 9 ||
          t.dashes != t.ndig / 3u - 1)   /* "010F214D1" is a typo, not a code */
         return CE_E_FORMAT;
      if (t.ndig > 3)
         GG_FLUSH();              /* a multi-group token starts a code */
      else if (ngg >= chunk)
         GG_FLUSH();
      if (ngg + t.ndig > 9)
         return CE_E_FORMAT;
      memcpy(gg + ngg, t.dig, t.ndig);
      ngg += t.ndig;
      if (ngg == 9)
         GG_FLUSH();
   }
   GG_FLUSH();
#undef GG_FLUSH

   if (!n)
      return CE_E_FORMAT;
   if (kind)
      *kind = n_genie ? CE_KIND_GB_GAMEGENIE : CE_KIND_GB_GAMESHARK;
   if (n_out)
      *n_out = n;
   return CE_OK;
}
