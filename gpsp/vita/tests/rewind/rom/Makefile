# tests/rewind/rom/Makefile -- the cartridges the rewind tests run.
#
#   rwtest.gb, rwtest.gbc   rwtest.asm, with rgbds
#   rwbusy.gba, rwheavy.gba busy.c at two noise levels, with arm-none-eabi-gcc
#                           (crt0.S and gba.ld are hostcore/testrom's)
#
# None is committed: the repository ignores cartridge images on purpose.
CROSS ?= arm-none-eabi-
RGBDS ?=
T     := ../../../hostcore/testrom
CFLAGS := -mcpu=arm7tdmi -mthumb -mthumb-interwork -O2 -ffreestanding \
          -fno-builtin -Wall -Wextra

all: rwtest.gb rwtest.gbc rwbusy.gba rwheavy.gba

rwtest.o: rwtest.asm
	$(RGBDS)rgbasm -o $@ $<
rwtest.gb: rwtest.o
	$(RGBDS)rgblink -o $@ $<
	$(RGBDS)rgbfix -v -p 255 -m 3 -r 2 -t GBADHOCRW $@
rwtest.gbc: rwtest.o
	$(RGBDS)rgblink -o $@ $<
	$(RGBDS)rgbfix -v -c -p 255 -m 3 -r 2 -t GBADHOCRW $@

rwbusy.gba: busy.c $(T)/crt0.S $(T)/gba.ld
	$(CROSS)gcc $(CFLAGS) -DNOISE=2048 -nostdlib -nostartfiles -T $(T)/gba.ld \
	  -o rwbusy.elf $(T)/crt0.S busy.c -lgcc
	$(CROSS)objcopy -O binary rwbusy.elf $@
rwheavy.gba: busy.c $(T)/crt0.S $(T)/gba.ld
	$(CROSS)gcc $(CFLAGS) -DNOISE=16384 -nostdlib -nostartfiles -T $(T)/gba.ld \
	  -o rwheavy.elf $(T)/crt0.S busy.c -lgcc
	$(CROSS)objcopy -O binary rwheavy.elf $@

clean:
	rm -f rwtest.o rwtest.gb rwtest.gbc rwbusy.elf rwbusy.gba rwheavy.elf rwheavy.gba
.PHONY: all clean
