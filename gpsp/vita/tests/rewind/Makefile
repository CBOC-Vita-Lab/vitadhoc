# gpsp/vita/tests/rewind/Makefile -- rewind's own tests (the app-level ones
# are ../../hostcore/rewind_tests.py).
#
#   make unit            rewind_unit: ../../rewind_vita.c against a machine
#                        whose every moment is known, x86-64 under ASan +
#                        UBSan, the two-thread arrangement under TSan, and
#                        armhf under qemu-arm
#   make roms            the cartridges of rom/ (rgbds, arm-none-eabi-gcc)
#   make bench A=a B=b   the squeeze's size and speed on two state dumps
GP  := ../../..
INC := -I$(GP)/vita -I$(GP)/psp -I$(GP)/frontend-common -I$(GP)/netdrv \
       -I$(GP)/libretro/libretro-common/include -I$(GP)
SRC := rewind_unit.c $(GP)/vita/rewind_vita.c
CFLAGS := -O2 -g -Wall -Wextra -std=gnu11 -DVITA $(INC)
ARM := arm-linux-gnueabihf-gcc -marm -march=armv7-a -mfpu=neon \
       -mfloat-abi=hard -static
QEMU ?= qemu-arm -L /usr/arm-linux-gnueabihf
SEEDS ?= 1 2 3

all: unit

build/unit_san: $(SRC) $(GP)/vita/rewind_vita.h
	@mkdir -p build
	gcc $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	  -o $@ $(SRC) -lpthread
build/unit_tsan: $(SRC) $(GP)/vita/rewind_vita.h
	@mkdir -p build
	gcc $(CFLAGS) -fsanitize=thread -o $@ $(SRC) -lpthread
build/unit: $(SRC) $(GP)/vita/rewind_vita.h
	@mkdir -p build
	gcc $(CFLAGS) -o $@ $(SRC) -lpthread
build/unit_armhf: $(SRC) $(GP)/vita/rewind_vita.h
	@mkdir -p build
	$(ARM) $(CFLAGS) -o $@ $(SRC) -lpthread

unit: build/unit_san build/unit_tsan build/unit build/unit_armhf
	@set -e; for s in $(SEEDS); do \
	  build/unit_san $$s 200000 0; \
	  build/unit_san $$s 200000 1; \
	  build/unit_tsan $$s 60000 1; \
	  $(QEMU) build/unit_armhf $$s 120000 0; \
	  $(QEMU) build/unit_armhf $$s 120000 1; \
	done
	@echo "rewind unit tests: passed"

roms:
	$(MAKE) -C rom

bench: build/unit
	build/unit bench $(A) $(B)

clean:
	rm -rf build
	$(MAKE) -C rom clean
.PHONY: all unit roms bench clean
