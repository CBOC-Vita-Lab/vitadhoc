/* vita/gba_instance_b.ld -- the partial link of GBA link instance B
 * (docs/GBA-LINK-MIRROR.md sections 1 and 19; the Vita twin of
 * psp/gbalink_core_b.ld).  Only .data and .bss are placed; every other input
 * section keeps its own name (ld -r orphans), exactly as without a script.
 * The markers are renamed gbab_* with every other global.
 *
 *   bdata_start .. bdata_end        B's initialised data: copied once at boot
 *                                   into bdata_pristine (fe_gbadual_boot),
 *                                   restored before every load of B
 *   bregion_start .. bregion_lend   B's zero-initialised state: zeroed before
 *                                   every load of B, so each game and each
 *                                   cable session starts B from a fresh
 *                                   boot's bytes
 *   bdata_pristine .. _pristine_end the pristine .data copy and the end of its
 *                                   room: exactly SIZEOF(.data), which
 *                                   fe_gbadual_boot checks against
 *                                   bdata_end - bdata_start before it copies
 *                                   (frontend-common/fe_gbadual_image.h; the
 *                                   build checks it as well, corebuild.mk)
 *
 * The PSP pads the .bss block to whole MiB because it LENDS it to the ROM
 * cache between sessions.  A Vita has the memory to keep B resident, nothing
 * is lent (fe_host_config.gba_link_nolend), so there is no padding here: a
 * block smaller than 1 MiB would simply never be carved.
 */
SECTIONS
{
  .data 0 : {
    bdata_start = .;
    *(.data .data.*)
    bdata_end = .;
  }
  .bss 0 : {
    bregion_start = .;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(64);
    bregion_lend = .;
    bdata_pristine = .;
    . += SIZEOF(.data);
    bdata_pristine_end = .;
    . = ALIGN(64);
  }
}
