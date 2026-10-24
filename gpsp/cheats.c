/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/* GBAdhoc: the cheat machine of this core instance.
 *
 * gpSP's own cheats.c understood plain CodeBreaker only, twenty entries of
 * it.  The decoding now lives in cheat_engine.c (ported from mGBA; plain
 * and encrypted CodeBreaker, GameShark Advance / Action Replay v1-v3, raw
 * lines); this file is what runs the result against THIS instance's
 * memory:
 *
 *   - once a frame, at the frame boundary inside update_gba (vcount 228 ->
 *     0), where the old code ran when a game had no master hook.  Codes are
 *     never run at a master code's hook address any more: cheat_master_hook
 *     stays ~0, so the translator emits no hook calls, and the ROM cache is
 *     not flushed to plant one.  A cheat device hooks because it has no
 *     other way to run; an emulator has the frame boundary.
 *
 *   - RAM writes go through write_memoryN.  A write that changes bytes the
 *     dynarec has translated code from (the SMC tags beside both work RAMs)
 *     flushes the RAM translation cache.  That is safe exactly here:
 *     update_gba is about to return "frame complete", the stub goes back to
 *     the frontend, and the next frame starts with a block lookup.
 *
 *   - ROM patches never touch the cartridge image.  The 32 KiB page a patch
 *     lands in is copied, the copy is patched, and this instance's read map
 *     is pointed at the copy.  Instance B of the link mirror borrows
 *     instance A's ROM pages (gba_memory.c gamepak_share_*), so patching in
 *     place would cheat on the partner's machine too; with copies B keeps
 *     reading the cartridge.  Switching a patch off is restoring the map.
 *     Either way the translation caches are flushed, because the translator
 *     reads opcodes and literal pools at translation time.
 *
 * The second GBA instance (every global here renamed gbab_*) carries a copy
 * of this file with an empty program: it costs one test a frame.
 */

#include "common.h"
#include "cheat_engine.h"

/* Kept for the translators (cpu_threaded.c tests `pc == cheat_master_hook`)
 * and main.c; never anything but "no hook". */
u32 cheat_master_hook = 0xffffffff;

/* ---- the program ----------------------------------------------------- */

static cheat_op    *prog_ops;
static unsigned    *prog_len;         /* operations per entry */
static unsigned     prog_entries;
static cheat_patch *prog_patches;
static unsigned     prog_n_patches;
static int          cheat_button_down;

/* ---- ROM pages ---------------------------------------------------------- */

#define CHEAT_ROM_PAGES 64            /* 2 MiB of patched pages at most */
#define ROM_MAP_BASE    (0x08000000 >> 15)

typedef struct
{
  u32 phys;                           /* 32 KiB page of the cartridge */
  u8 *orig;                           /* what the loader mapped there */
  u8 *mine;                           /* the private copy */
  u8  active;
} cheat_rom_page;

static cheat_rom_page rom_pages[CHEAT_ROM_PAGES];
static unsigned rom_page_count;
static unsigned rom_pages_active;
static unsigned rom_failed;           /* patches not applied, last install */
/* The patches were lifted for instance B's sake (cheat_rom_share_begin) and
 * go back at the next frame boundary. */
static int rom_reapply;

static cheat_rom_page *rom_page_get(u32 phys)
{
  unsigned i;
  cheat_rom_page *p;
  u8 *cur;

  for (i = 0; i < rom_page_count; i++)
    if (rom_pages[i].phys == phys)
      return &rom_pages[i];
  if (rom_page_count >= CHEAT_ROM_PAGES)
    return NULL;
  cur = memory_map_read[ROM_MAP_BASE + phys];
  if (!cur)
    return NULL;
  p = &rom_pages[rom_page_count];
  p->mine = (u8 *)malloc(0x8000);
  if (!p->mine)
    return NULL;
  p->phys = phys;
  p->orig = cur;
  p->active = 0;
  rom_page_count++;
  return p;
}

static int rom_poke(u32 offset, u8 value)
{
  cheat_rom_page *p = rom_page_get(offset >> 15);
  if (!p)
    return -1;
  if (!p->active)
  {
    memcpy(p->mine, p->orig, 0x8000);
    gamepak_map_page(p->phys, p->mine);
    p->active = 1;
    rom_pages_active++;
  }
  p->mine[offset & 0x7FFF] = value;
  return 0;
}

/* Returns 1 if a page went back to the cartridge's. */
static int rom_unpatch(void)
{
  unsigned i;
  int any = 0;
  for (i = 0; i < rom_page_count; i++)
  {
    cheat_rom_page *p = &rom_pages[i];
    if (!p->active)
      continue;
    /* Only undo what is still ours: a reload rebuilt the map. */
    if (memory_map_read[ROM_MAP_BASE + p->phys] == p->mine)
      gamepak_map_page(p->phys, p->orig);
    p->active = 0;
    any = 1;
  }
  rom_pages_active = 0;
  return any;
}

/* Returns 1 if anything was patched. */
static int rom_patch(void)
{
  unsigned i, b;
  u32 size = gamepak_size;
  /* A 1 MiB Classic NES cartridge is held as four copies. */
  u32 step = (gamepak_mini_materialized && size > 0x100000) ? 0x100000 : size;
  int any = 0;

  rom_failed = 0;
  if (!prog_n_patches)
    return 0;
  if (!size || gamepak_must_swap())
  {
    /* A paged cartridge: pages come and go under the patch.  The Vita holds
     * every cartridge whole; this is the PSP's and the small hosts' case. */
    rom_failed = prog_n_patches;
    return 0;
  }
  for (i = 0; i < prog_n_patches; i++)
  {
    const cheat_patch *pt = &prog_patches[i];
    int failed = 0;
    for (b = 0; b < pt->width; b++)
    {
      u32 off = ((pt->address & 0x01FFFFFF) + b) % step;
      u8 value = (u8)(pt->value >> (8 * b));
      for (; off < size; off += step)
      {
        if (rom_poke(off, value) < 0)
          failed = 1;
        else
          any = 1;
      }
    }
    rom_failed += failed;
  }
  return any;
}

static void cheat_flush_code(void)
{
#ifdef HAVE_DYNAREC
  if (dynarec_enable)
    flush_dynarec_caches();
#endif
}

/* gba_memory.c, memory_unload_gamepak: the cartridge and its map are gone. */
void cheat_gamepak_unloaded(void)
{
  unsigned i;
  for (i = 0; i < rom_page_count; i++)
    free(rom_pages[i].mine);
  rom_page_count = 0;
  rom_pages_active = 0;
  /* The program outlives the cartridge: fe_host reloads the SAME game around
   * some link sessions.  Its ROM patches go back in at the first frame
   * boundary of whatever is loaded next (a frontend that loads another game
   * replaces the program first: retro_cheat_reset, cheats_load). */
  rom_reapply = prog_n_patches != 0;
}

/* gba_memory.c, gamepak_share_export: instance B is about to copy this
 * instance's ROM map.  It must copy the cartridge, not our patched pages. */
void cheat_rom_share_begin(void)
{
  if (rom_unpatch())
    rom_reapply = 1;
}

unsigned gba_cheats_rom_pages(void)
{
  return rom_pages_active;
}

/* ---- the bus -------------------------------------------------------- */

static int smc_hit;

static uint32_t bus_read(void *ud, uint32_t address, int width)
{
  (void)ud;
  switch (width)
  {
    case 1:  return read_memory8(address) & 0xFF;
    case 2:  return read_memory16(address & ~1u) & 0xFFFF;
    default: return read_memory32(address & ~3u);
  }
}

static void bus_write(void *ud, uint32_t address, int width, uint32_t value)
{
  const u8 *tags = NULL;
  int i;
  (void)ud;

  if (width == 2)
    address &= ~1u;
  else if (width == 4)
    address &= ~3u;

  switch (address >> 24)
  {
    case 0x02:
      tags = &ewram[0x40000 + (address & 0x3FFFF)];
      break;
    case 0x03:
      tags = &iwram[address & 0x7FFF];
      break;
    case 0x04: case 0x05: case 0x06: case 0x07:
      break;
    default:
      /* BIOS, cartridge, backup memory, nothing: a cheat device cannot
       * write there, and for the cartridge it must not (GPIO, the EEPROM
       * and flash state machines).  ROM is patched, not written. */
      return;
  }

  if (tags)
  {
    /* The same value again is the common case (a code holds a counter),
     * and when the bytes are code it would cost a cache flush a frame. */
    if (bus_read(NULL, address, width) ==
        (width == 4 ? value : (value & ((1u << (width * 8)) - 1))))
      return;
  }

  switch (width)
  {
    case 1:  write_memory8(address, (u8)value);   break;
    case 2:  write_memory16(address, (u16)value); break;
    default: write_memory32(address, value);      break;
  }

  if (tags)
    for (i = 0; i < width; i++)
      if (tags[i])
        smc_hit = 1;
}

/* ---- once a frame -------------------------------------------------------- */

void process_cheats(void)
{
  static const cheat_bus bus = { bus_read, bus_write, NULL };
  const cheat_op *ops = prog_ops;
  unsigned i;

  if (rom_reapply)
  {
    rom_reapply = 0;
    if (rom_patch())
      cheat_flush_code();
  }
  if (!prog_entries)
    return;

  for (i = 0; i < prog_entries; i++)
  {
    cheat_vm_run(ops, prog_len[i], &bus, cheat_button_down);
    ops += prog_len[i];
  }

#ifdef HAVE_DYNAREC
  if (smc_hit && dynarec_enable)
    flush_translation_cache_ram();
#endif
  smc_hit = 0;
}

/* ---- installing a program ------------------------------------------------ */

static int same_patches(const cheat_patch *p, unsigned n)
{
  unsigned i;
  if (n != prog_n_patches)
    return 0;
  for (i = 0; i < n; i++)
    if (p[i].address != prog_patches[i].address ||
        p[i].value != prog_patches[i].value ||
        p[i].width != prog_patches[i].width)
      return 0;
  return 1;
}

int gba_cheats_set(const cheat_op *ops, const unsigned *entry_ops,
                   unsigned n_entries, const cheat_patch *patches,
                   unsigned n_patches)
{
  unsigned i, n_ops = 0;
  cheat_op *new_ops = NULL;
  unsigned *new_len = NULL;
  cheat_patch *new_patches = NULL;
  int rom_changed;

  for (i = 0; i < n_entries; i++)
    n_ops += entry_ops[i];

  if (n_ops)
    new_ops = (cheat_op *)malloc(n_ops * sizeof(cheat_op));
  if (n_entries)
    new_len = (unsigned *)malloc(n_entries * sizeof(unsigned));
  if (n_patches)
    new_patches = (cheat_patch *)malloc(n_patches * sizeof(cheat_patch));
  if ((n_ops && !new_ops) || (n_entries && !new_len) ||
      (n_patches && !new_patches))
  {
    /* Out of memory: run nothing rather than half of it. */
    free(new_ops);
    free(new_len);
    free(new_patches);
    n_entries = n_patches = n_ops = 0;
    new_ops = NULL;
    new_len = NULL;
    new_patches = NULL;
  }
  if (n_ops)
    memcpy(new_ops, ops, n_ops * sizeof(cheat_op));
  if (n_entries)
    memcpy(new_len, entry_ops, n_entries * sizeof(unsigned));
  if (n_patches)
    memcpy(new_patches, patches, n_patches * sizeof(cheat_patch));

  /* The same patches as before and all of them in place: leave the ROM (and
   * the translation caches) alone.  rom_pages_active tells "in place": a
   * reload or a reset of the map takes it to zero. */
  rom_changed = !(same_patches(new_patches, n_patches) &&
                  (n_patches == 0 || rom_pages_active) && !rom_reapply);

  free(prog_ops);
  free(prog_len);
  free(prog_patches);
  prog_ops = new_ops;
  prog_len = new_len;
  prog_entries = n_entries;
  prog_patches = new_patches;
  prog_n_patches = n_patches;

  if (rom_changed)
  {
    int was = rom_unpatch();
    int now = rom_patch();
    rom_reapply = 0;
    if (was || now)
      cheat_flush_code();
  }
  return (int)rom_failed;
}

void gba_cheats_button(int down)
{
  cheat_button_down = down ? 1 : 0;
}

/* ---- the libretro interface (retro_cheat_set / retro_cheat_reset) -------- */

static char *slot_code[MAX_CHEATS];
static bool  slot_active[MAX_CHEATS];

#define LEGACY_OPS     4096
#define LEGACY_PATCHES 256

static cheat_error legacy_rebuild(unsigned check_index)
{
  cheat_gba_ctx ctx;
  cheat_op *ops = (cheat_op *)malloc(LEGACY_OPS * sizeof(cheat_op));
  cheat_patch *patches =
     (cheat_patch *)malloc(LEGACY_PATCHES * sizeof(cheat_patch));
  unsigned len[MAX_CHEATS];
  unsigned n_ops = 0, n_patches = 0, n_entries = 0, i;
  cheat_error result = CheatNoError;

  if (!ops || !patches)
  {
    free(ops);
    free(patches);
    return CheatErrorTooBig;
  }

  cheat_gba_ctx_init(&ctx);
  for (i = 0; i < MAX_CHEATS; i++)
    if (slot_code[i])
      cheat_gba_ctx_scan(&ctx, slot_code[i]);

  for (i = 0; i < MAX_CHEATS; i++)
  {
    cheat_out out;
    int rc;
    if (!slot_code[i] || !slot_active[i])
      continue;
    out.ops = ops + n_ops;
    out.cap_ops = LEGACY_OPS - n_ops;
    out.patches = patches + n_patches;
    out.cap_patches = LEGACY_PATCHES - n_patches;
    out.n_ops = out.n_patches = 0;
    out.flags = 0;
    rc = cheat_gba_compile(slot_code[i], &ctx, &out, NULL);
    if (rc != CE_OK)
    {
      slot_active[i] = false;
      if (i == check_index)
        result = rc == CE_E_ENCRYPTED ? CheatErrorEncrypted :
                 rc == CE_E_TOO_LONG  ? CheatErrorTooBig :
                                        CheatErrorNotSupported;
      continue;
    }
    len[n_entries++] = out.n_ops;
    n_ops += out.n_ops;
    n_patches += out.n_patches;
  }
  gba_cheats_set(ops, len, n_entries, patches, n_patches);
  free(ops);
  free(patches);
  return result;
}

void cheat_clear(void)
{
  unsigned i;
  for (i = 0; i < MAX_CHEATS; i++)
  {
    free(slot_code[i]);
    slot_code[i] = NULL;
    slot_active[i] = false;
  }
  gba_cheats_set(NULL, NULL, 0, NULL, 0);
}

void cheat_disable(unsigned index)
{
  if (index >= MAX_CHEATS || !slot_active[index])
    return;
  slot_active[index] = false;
  legacy_rebuild(MAX_CHEATS);
}

cheat_error cheat_parse(unsigned index, const char *code)
{
  size_t len;
  char *copy;

  if (index >= MAX_CHEATS)
    return CheatErrorTooMany;
  /* A rejected replacement must not leave the previous code running. */
  free(slot_code[index]);
  slot_code[index] = NULL;
  slot_active[index] = false;

  len = strlen(code);
  if (len >= 8192)
  {
    legacy_rebuild(MAX_CHEATS);
    return CheatErrorTooBig;
  }
  copy = (char *)malloc(len + 1);
  if (!copy)
  {
    legacy_rebuild(MAX_CHEATS);
    return CheatErrorTooBig;
  }
  memcpy(copy, code, len + 1);
  slot_code[index] = copy;
  slot_active[index] = true;
  return legacy_rebuild(index);
}
