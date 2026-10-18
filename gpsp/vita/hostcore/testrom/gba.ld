/* gba.ld -- a GBA cartridge: code and constants in ROM, .data and .bss in
 * the 32 KiB internal RAM. */
ENTRY(_start)
MEMORY
{
  rom   (rx)  : ORIGIN = 0x08000000, LENGTH = 1M
  iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 0x7000
}
SECTIONS
{
  .text : { KEEP(*(.init)) *(.text .text.*) *(.rodata .rodata.*) . = ALIGN(4); } > rom
  __data_lma = LOADADDR(.data);
  .data : { __data_start = .; *(.data .data.*) . = ALIGN(4); __data_end = .; } > iwram AT > rom
  .bss (NOLOAD) : { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __bss_end = .; } > iwram
  /DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.note*) }
}
