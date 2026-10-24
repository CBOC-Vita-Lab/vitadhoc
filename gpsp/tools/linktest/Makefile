# tools/linktest/Makefile -- the link cable test ROM (README.md).
#
# Needs only arm-none-eabi-gcc/binutils and python3: own start-up code and
# linker script, no devkitPro, no libc.  The built linktest.gba is committed,
# so nothing here has to run to use the rigs in ../gbamirror.
CROSS  ?= arm-none-eabi-
CC     := $(CROSS)gcc
ARCH   := -mcpu=arm7tdmi -mthumb-interwork
CFLAGS := $(ARCH) -O2 -Wall -ffreestanding -fno-builtin \
          -fno-tree-loop-distribute-patterns -fno-common

# make FLAGPROBE=1: a second ROM whose interrupt entry records the flags it
# is entered with (crt0.s).  Not part of the normal rig; README.md.
ifeq ($(FLAGPROBE),1)
ASDEF := -Wa,--defsym,FLAGPROBE=1
OUT   := linktest_flagprobe
else
ASDEF :=
OUT   := linktest
endif

all: $(OUT).gba

crt0.o: crt0.s FORCE
	$(CC) $(ARCH) $(ASDEF) -c -o $@ $<
main.o: main.c linktest.h
	$(CC) $(CFLAGS) -mthumb -c -o $@ $<
# ARM code, run from IWRAM: everything outside it is a long call away
iwram.o: iwram.c linktest.h
	$(CC) $(CFLAGS) -marm -mlong-calls -c -o $@ $<

linktest.elf: crt0.o main.o iwram.o linktest.ld
	$(CC) $(ARCH) -mthumb -nostdlib -nostartfiles -Wl,-T,linktest.ld \
	  -Wl,-Map,linktest.map -o $@ crt0.o main.o iwram.o -lgcc

# Raw image, the header complement check filled in (GBATEK: byte BDh =
# -(sum of A0h..BCh) - 19h), padded to a multiple of 4.
$(OUT).gba: linktest.elf
	$(CROSS)objcopy -O binary $< $@
	python3 -c "import sys; b=bytearray(open(sys.argv[1],'rb').read()); \
	  b[0xBD]=(-(sum(b[0xA0:0xBD])+0x19))&0xFF; b+=bytes(-len(b)%4); \
	  open(sys.argv[1],'wb').write(b)" $@

clean:
	rm -f crt0.o main.o iwram.o linktest.elf linktest.map

FORCE:
.PHONY: all clean FORCE
