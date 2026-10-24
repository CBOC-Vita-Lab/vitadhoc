# tests/cheats/rom/Makefile -- the two cartridges the end-to-end cheat tests
# run (never committed: the repository ignores *.gba and *.gb on purpose).
#   cheatrom.gba   arm-none-eabi-gcc, no devkitPro needed
#   gbcheat.gb     rgbds (make RGBDS=/path/to/rgbds/)
CROSS ?= arm-none-eabi-
RGBDS ?=
CFLAGS := -mcpu=arm7tdmi -mthumb -mthumb-interwork -O2 -ffreestanding \
          -fno-builtin -Wall -Wextra

all: cheatrom.gba gbcheat.gb

cheatrom.gba: crt0.S main.c gba.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -nostartfiles -T gba.ld -o cheatrom.elf \
	  crt0.S main.c -lgcc
	$(CROSS)objcopy -O binary cheatrom.elf $@

gbcheat.gb: gbcheat.asm
	$(RGBDS)rgbasm -o gbcheat.o gbcheat.asm
	$(RGBDS)rgblink -o $@ gbcheat.o
	$(RGBDS)rgbfix -v -c -p 255 -m 1 -t GBCHEAT $@
	@rm -f gbcheat.o

clean:
	rm -f cheatrom.elf cheatrom.gba gbcheat.gb gbcheat.o
.PHONY: all clean
