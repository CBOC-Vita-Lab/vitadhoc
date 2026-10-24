/* linktest.ld -- GBA cartridge layout for the link test ROM. */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
   rom   (rx)  : ORIGIN = 0x08000000, LENGTH = 1M
   ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
   iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 0x7000   /* stacks above */
}

SECTIONS
{
   .text :
   {
      KEEP(*(.header))
      *(EXCLUDE_FILE(*iwram.o) .text EXCLUDE_FILE(*iwram.o) .text.*)
      *(EXCLUDE_FILE(*iwram.o) .rodata EXCLUDE_FILE(*iwram.o) .rodata.*)
      *(.glue_7 .glue_7t .v4_bx .vfp11_veneer)
      . = ALIGN(4);
      /* the EWRAM routine's image: position independent, copied at run time */
      KEEP(*(.ewcode))
      . = ALIGN(4);
   } > rom

   /* Code and data that run from IWRAM; crt0 copies the image out of ROM. */
   .iwram :
   {
      __iwram_start = .;
      KEEP(*(.iwram .iwram.*))
      *iwram.o(.text .text.* .rodata .rodata.*)
      . = ALIGN(4);
      *(.data .data.*)
      . = ALIGN(4);
      __iwram_end = .;
   } > iwram AT > rom
   __iwram_lma = LOADADDR(.iwram);

   .bss (NOLOAD) :
   {
      __bss_start = .;
      *(.bss .bss.* COMMON)
      . = ALIGN(4);
      __bss_end = .;
   } > iwram

   /* The result block the host tool reads: exactly at 0x02000000. */
   .results (NOLOAD) : { KEEP(*(.results)) } > ewram

   /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.ARM.exidx*) *(.note*) }
}
