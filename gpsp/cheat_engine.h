/* cheat_engine.h -- cheat-code decoders and the little machine that runs them.
 *
 * Self-contained C (no emulator headers): the same file is built into the
 * GBA core, into the Vita app's unit tests and into the database tools.
 * Not reentrant (static scratch): call it from one thread.
 *
 *   text  --cheat_gba_compile-->  cheat_op[] + cheat_patch[]  --cheat_vm_run-->  bus
 *   text  --cheat_gb_compile--->  cheat_gb_code[]              (applied by gbcore)
 *
 * GBA kinds decoded: CodeBreaker / GameShark SP (plain and encrypted, with
 * its 9xxxxxxx seed code), GameShark / Action Replay v1-v2 ("GSA", TEA
 * encrypted or raw), Pro Action Replay v3 (encrypted or raw), raw
 * address/value and VBA "AAAAAAAA:VV" lines.  GB/GBC kinds: GameShark
 * (TTVVLLHH) and Game Genie (VVA-AAA[-CCC]).
 *
 * ORIGIN.  The decoders, the seed tables, the op model and the interpreter
 * loop are ported from mGBA (src/gba/cheats/{codebreaker,gameshark,parv3}.c,
 * src/gba/cheats.c, src/gb/cheats.c, src/core/cheats.c; Jeffrey Pfau,
 * MPL-2.0).  The CodeBreaker button and add-32 forms follow gpSP's own
 * cheats.c (VBA-M's semantics).  What is new here: detection per ENTRY over
 * all of its lines (mGBA decides on the first line and keeps the answer for
 * the rest of the file), seed sets collected from the whole list so the
 * order of entries does not matter, the libretro '+' separated spelling,
 * placeholders, the GSA v1 address-list fix, and signed PARv3 compares.
 */
#ifndef GPSP_CHEAT_ENGINE_H
#define GPSP_CHEAT_ENGINE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- results ----------------------------------------------------------- */

/* Numerically the same as CHEAT_KIND_* in vita/cheats_vita.h. */
enum
{
   CE_KIND_UNKNOWN = 0,
   CE_KIND_GBA_RAW,
   CE_KIND_GBA_CODEBREAKER,
   CE_KIND_GBA_GAMESHARK,
   CE_KIND_GBA_AR3,
   CE_KIND_GB_GAMESHARK,
   CE_KIND_GB_GAMEGENIE
};

enum
{
   CE_OK = 0,
   CE_E_FORMAT = -1,        /* not a code                                   */
   CE_E_UNSUPPORTED = -2,   /* a code type this engine does not run         */
   CE_E_TOO_LONG = -3,      /* more lines / operations than the caller has  */
   CE_E_PLACEHOLDER = -4,   /* "01??C0D5": the player must fill in a value  */
   CE_E_ENCRYPTED = -5,     /* CodeBreaker lines that only make sense after
                             * a 9xxxxxxx seed code nobody supplied         */
   CE_E_TRUNCATED = -6      /* the last code's parameter line is missing    */
};

/* cheat_out.flags */
#define CE_F_ROM_PATCH  0x01   /* patches cartridge ROM                     */
#define CE_F_BUTTON     0x02   /* waits for the cheat device's button       */
#define CE_F_MASTER     0x04   /* has master / hook / game-id / seed lines  */
#define CE_F_NOOP       0x08   /* nothing to run (an "enable code")         */
#define CE_F_DECRYPTED  0x10   /* was decrypted                             */

/* ---- the machine --------------------------------------------------------- */

enum
{
   CE_ASSIGN = 0,
   CE_ASSIGN_INDIRECT,
   CE_AND,
   CE_ADD,
   CE_SUB,
   CE_OR,
   CE_IF_EQ,
   CE_IF_NE,
   CE_IF_LT,          /* signed, at the operation's width */
   CE_IF_GT,
   CE_IF_ULT,
   CE_IF_UGT,
   CE_IF_ULE,
   CE_IF_UGE,
   CE_IF_AND,
   CE_IF_LAND,
   CE_IF_NAND,
   CE_IF_BUTTON,
   CE_NEVER
};

typedef struct cheat_op
{
   uint8_t  type;
   uint8_t  width;              /* 1, 2 or 4 bytes */
   uint16_t pad;
   uint32_t address;
   uint32_t operand;
   uint32_t repeat;             /* writes: how many; conditions: how many
                                 * following ops they guard                 */
   uint32_t negative_repeat;    /* conditions: length of the "else" part    */
   int32_t  address_offset;
   int32_t  operand_offset;
} cheat_op;

typedef struct cheat_patch
{
   uint32_t address;            /* 0x08000000.. */
   uint32_t value;
   uint8_t  width;              /* 1, 2 or 4 */
} cheat_patch;

/* Where a compiled entry goes.  The caller owns the arrays; n_* are reset
 * by the compiler.  ops == NULL with cap_ops == 0 is not allowed: give it
 * a scratch array to validate. */
typedef struct cheat_out
{
   cheat_op    *ops;
   unsigned     n_ops, cap_ops;
   cheat_patch *patches;
   unsigned     n_patches, cap_patches;
   unsigned     flags;          /* CE_F_* */
} cheat_out;

typedef struct cheat_bus
{
   uint32_t (*read)(void *ud, uint32_t address, int width);
   void     (*write)(void *ud, uint32_t address, int width, uint32_t value);
   void      *ud;
} cheat_bus;

/* Run one entry's operations once (a frame).  `button`: the cheat device's
 * own button is held. */
void cheat_vm_run(const cheat_op *ops, unsigned n, const cheat_bus *bus,
                  int button);

/* ---- GBA ----------------------------------------------------------------- */

#define CE_SEEDSETS 4

/* The seeds a game's codes are encrypted with.  A cheat device learns them
 * from the game's master code; here they are collected from every entry of
 * the game's list (cheat_gba_ctx_scan), enabled or not and in any order,
 * and each entry is then decoded with whichever set makes sense of it. */
typedef struct cheat_gba_ctx
{
   unsigned n_gsa1;                     /* [0] is the device's default */
   uint32_t gsa1[CE_SEEDSETS][4];
   unsigned n_par3;
   uint32_t par3[CE_SEEDSETS][4];
   /* A line of the list decrypts with this set to the device's game-id
    * line (xxxxxxxx 001DC0DE): the list IS that device's, and its codes are
    * then read with less suspicion (mirrored RAM addresses, as published
    * Action Replay codes have them, count as RAM). */
   uint8_t  gsa1_sure[CE_SEEDSETS], par3_sure[CE_SEEDSETS];
   unsigned n_cb;                       /* CodeBreaker has no default set */
   struct
   {
      uint32_t master;
      uint32_t seeds[4];
      uint8_t  table[0x30];
   } cb[CE_SEEDSETS];
} cheat_gba_ctx;

void cheat_gba_ctx_init(cheat_gba_ctx *ctx);
/* Look for seed-changing lines (CodeBreaker 9xxxxxxx yyyy, GSA / PARv3
 * DEADFACE 0000xxxx) in one entry's text. */
void cheat_gba_ctx_scan(cheat_gba_ctx *ctx, const char *code);

/* Compile one entry.  `ctx` may be NULL (default seeds only).  Returns CE_OK
 * and fills out/kind, or an error (kind is then the best guess or
 * CE_KIND_UNKNOWN). */
int cheat_gba_compile(const char *code, const cheat_gba_ctx *ctx,
                      cheat_out *out, int *kind);

/* For tests and tools: the raw crypto primitives. */
void cheat_gsa_decrypt(uint32_t *op1, uint32_t *op2, const uint32_t seeds[4]);
void cheat_gsa_encrypt(uint32_t *op1, uint32_t *op2, const uint32_t seeds[4]);
extern const uint32_t cheat_gsa1_default_seeds[4];
extern const uint32_t cheat_par3_default_seeds[4];
/* Compile with one fixed interpretation (the mGBA test-suite uses these):
 * version 1 GSAv1, 2 GSAv1 raw, 3 PARv3, 4 PARv3 raw, 5 CodeBreaker. */
int cheat_gba_compile_as(const char *code, int version, cheat_out *out);

/* ---- GB / GBC ------------------------------------------------------------ */

typedef struct cheat_gb_code
{
   uint8_t  rom;          /* 0: RAM write every frame, 1: ROM patch          */
   uint8_t  type;         /* GameShark's first byte (bank selector)          */
   uint8_t  value;
   uint8_t  compare;
   uint8_t  has_compare;
   uint8_t  pad;
   uint16_t address;
} cheat_gb_code;

/* One entry ('+'-joined codes).  out may be NULL with cap 0 to validate. */
int cheat_gb_compile(const char *code, cheat_gb_code *out, unsigned cap,
                     unsigned *n, int *kind);

/* ---- the GBA core's side (gpsp/cheats.c) --------------------------------- */

/* Replace what the core runs every frame: `n_entries` entries, entry i being
 * the next entry_ops[i] operations of `ops`, and the ROM patches of all of
 * them.  Everything is copied.  Call between frames (it may flush the
 * translation caches).  Returns the number of ROM patches that could not be
 * applied (the cartridge is paged, or the address is outside it). */
int  gba_cheats_set(const cheat_op *ops, const unsigned *entry_ops,
                    unsigned n_entries, const cheat_patch *patches,
                    unsigned n_patches);
void gba_cheats_button(int down);
/* Diagnostics for tests: ROM pages currently replaced by patched copies. */
unsigned gba_cheats_rom_pages(void);

#ifdef __cplusplus
}
#endif

#endif /* GPSP_CHEAT_ENGINE_H */
