/* gba.ld -- the cheat test cartridge: code and constants in ROM, .data and
 * .bss in internal RAM, and the three things ROM-patch codes aim at pinned
 * at 0x08002000 (crt0.S, section .patch). */
ENTRY(_start)
MEMORY
{
  rom   (rx)  : ORIGIN = 0x08000000, LENGTH = 1M
  iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 0x4000
}
SECTIONS
{
  .text : { KEEP(*(.init)) *(.text .text.*) *(.rodata .rodata.*) . = ALIGN(4); } > rom
  ASSERT(. <= 0x08002000, "code runs into the .patch section")
  .patch 0x08002000 : { KEEP(*(.patch)) . = ALIGN(4); } > rom
  __data_lma = LOADADDR(.data);
  .data : { __data_start = .; *(.data .data.*) . = ALIGN(4); __data_end = .; } > iwram AT > rom
  .bss (NOLOAD) : { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __bss_end = .; } > iwram
  /DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.note*) }
}
