# hostcore/testrom/Makefile -- build gbadhoc-test.gba with a bare-metal ARM
# toolchain (arm-none-eabi-gcc; no devkitPro needed).  The image is not
# committed: the repository ignores *.gba on purpose.
CROSS ?= arm-none-eabi-
CFLAGS := -mcpu=arm7tdmi -mthumb -mthumb-interwork -O2 -ffreestanding \
          -fno-builtin -Wall -Wextra

gbadhoc-test.gba: crt0.S main.c gba.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -nostartfiles -T gba.ld -o gbadhoc-test.elf \
	  crt0.S main.c -lgcc
	$(CROSS)objcopy -O binary gbadhoc-test.elf $@
	@ls -l $@

# The September 2026 gpSP fixes (app_tests.py, cpufix): see cpufix.c.
.PHONY: cpufix
cpufix: gbadhoc-cpufix.gba
gbadhoc-cpufix.gba: crt0.S cpufix.c gba.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -nostartfiles -T gba.ld -o gbadhoc-cpufix.elf \
	  crt0.S cpufix.c -lgcc
	$(CROSS)objcopy -O binary gbadhoc-cpufix.elf $@
	@ls -l $@

# Known sounds for tools/audio_measure.py (audio.c).
audio: gbadhoc-audio.gba
.PHONY: audio
gbadhoc-audio.gba: crt0.S audio.c gba.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -nostartfiles -T gba.ld -o gbadhoc-audio.elf \
	  crt0.S audio.c -lgcc
	$(CROSS)objcopy -O binary gbadhoc-audio.elf $@
	@ls -l $@

# The wide picture's "Mode 7" floor (app_tests.py, wide): see wide7.c.
wide: gbadhoc-wide.gba
gbadhoc-wide.gba: crt0.S wide7.c gba.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -nostartfiles -T gba.ld -o gbadhoc-wide.elf \
	  crt0.S wide7.c -lgcc
	$(CROSS)objcopy -O binary gbadhoc-wide.elf $@
	@ls -l $@

# The automatic wide mode's side-scroller (app_tests.py, wide_auto): scroll.c.
scroll: gbadhoc-scroll.gba
gbadhoc-scroll.gba: crt0.S scroll.c gba.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -nostartfiles -T gba.ld -o gbadhoc-scroll.elf \
	  crt0.S scroll.c -lgcc
	$(CROSS)objcopy -O binary gbadhoc-scroll.elf $@
	@ls -l $@

# ...and its world seen from above (app_tests.py, wide_top): topdown.c.
top: gbadhoc-top.gba
gbadhoc-top.gba: crt0.S topdown.c gba.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -nostartfiles -T gba.ld -o gbadhoc-top.elf \
	  crt0.S topdown.c -lgcc
	$(CROSS)objcopy -O binary gbadhoc-top.elf $@
	@ls -l $@

# The sensor cartridges (app_tests.py, sensors): sensors.c with a tilt, a
# gyro and a light-sensor game's code in the header (cart_sensor.h).
sensors: gbadhoc-tilt.gba gbadhoc-gyro.gba gbadhoc-solar.gba
gbadhoc-tilt.gba gbadhoc-gyro.gba gbadhoc-solar.gba: crt0.S sensors.c gba.ld
gbadhoc-tilt.gba:  SENSOR_HDR := -DROM_TITLE='"SENSORTILT\0\0"' -DROM_CODE='"KYGE"'
gbadhoc-gyro.gba:  SENSOR_HDR := -DROM_TITLE='"SENSORGYRO\0\0"' -DROM_CODE='"RZWE"'
gbadhoc-solar.gba: SENSOR_HDR := -DROM_TITLE='"SENSORSOLAR\0"' -DROM_CODE='"U3IE"'
gbadhoc-tilt.gba gbadhoc-gyro.gba gbadhoc-solar.gba:
	$(CROSS)gcc $(CFLAGS) $(SENSOR_HDR) -nostdlib -nostartfiles -T gba.ld \
	  -o $(@:.gba=.elf) crt0.S sensors.c -lgcc
	$(CROSS)objcopy -O binary $(@:.gba=.elf) $@
	@ls -l $@

# The GB battery-save cartridge.  Optional: needs rgbds, which is not in the
# distribution's packages (git clone https://github.com/gbdev/rgbds, any
# release; make RGBDS=/path/to/rgbds/ gb).
RGBDS ?=
gb: gbsave.gb gblink.gb gblocal.gb cgb
gbsave.gb: gbsave.asm
	$(RGBDS)rgbasm -o gbsave.o gbsave.asm
	$(RGBDS)rgblink -o $@ gbsave.o
	$(RGBDS)rgbfix -v -p 255 -m 3 -r 2 -t GBADHOCSAVE $@
	@rm -f gbsave.o

# The GB link-cable cartridge (hostcore/link_tests.py): see gblink.asm.
gblink.gb: gblink.asm
	$(RGBDS)rgbasm -o gblink.o gblink.asm
	$(RGBDS)rgblink -o $@ gblink.o
	$(RGBDS)rgbfix -v -p 255 -m 3 -r 2 -t GBLINKTEST $@
	@rm -f gblink.o

# The GB link-cable cartridge of the local link's tests (local_tests.py):
# see gblocal.asm.  Not the network tests' gblink.gb -- that one stops after
# 200 bytes; this one never stops, shows what it does and counts its boots.
gblocal.gb: gblocal.asm
	$(RGBDS)rgbasm -o gblocal.o gblocal.asm
	$(RGBDS)rgblink -o $@ gblocal.o
	$(RGBDS)rgbfix -v -p 255 -m 3 -r 2 -t GBADHOCLINK $@
	@rm -f gblocal.o

# The same two cartridges with the Game Boy Color flag set (header $143 =
# $80, "works on both"): a .gbc file that really runs as a Color, to link with
# the plain .gb ones (Red <-> Gold).  The -cgbfast ones also switch to double
# speed at power on, as a Color game does.
cgb: gblink-cgb.gbc gblocal-cgb.gbc gblink-cgbfast.gbc gblocal-cgbfast.gbc
gblink-cgb.gbc: gblink.asm
	$(RGBDS)rgbasm -o gblink-cgb.o gblink.asm
	$(RGBDS)rgblink -o $@ gblink-cgb.o
	$(RGBDS)rgbfix -v -c -p 255 -m 3 -r 2 -t GBLINKTEST $@
	@rm -f gblink-cgb.o
gblink-cgbfast.gbc: gblink.asm
	$(RGBDS)rgbasm -DDOUBLE -o gblink-cgbfast.o gblink.asm
	$(RGBDS)rgblink -o $@ gblink-cgbfast.o
	$(RGBDS)rgbfix -v -c -p 255 -m 3 -r 2 -t GBLINKTEST $@
	@rm -f gblink-cgbfast.o
gblocal-cgb.gbc: gblocal.asm
	$(RGBDS)rgbasm -o gblocal-cgb.o gblocal.asm
	$(RGBDS)rgblink -o $@ gblocal-cgb.o
	$(RGBDS)rgbfix -v -c -p 255 -m 3 -r 2 -t GBADHOCLINK $@
	@rm -f gblocal-cgb.o
gblocal-cgbfast.gbc: gblocal.asm
	$(RGBDS)rgbasm -DDOUBLE -o gblocal-cgbfast.o gblocal.asm
	$(RGBDS)rgblink -o $@ gblocal-cgbfast.o
	$(RGBDS)rgbfix -v -c -p 255 -m 3 -r 2 -t GBADHOCLINK $@
	@rm -f gblocal-cgbfast.o

clean:
	rm -f gbadhoc-test.elf gbadhoc-test.gba gbsave.gb gbsave.o gblink.gb gblink.o \
	      gblocal.gb gblocal.o *.gbc
.PHONY: clean gb cgb

.PHONY: wide scroll top gb sensors

# XLAT_ITERATIVE (app_tests.py, chain): a chain of thousands of first-visit
# blocks, chain.c.
.PHONY: chain
chain: gbadhoc-chain.gba
gbadhoc-chain.gba: crt0.S chain.c gba.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -nostartfiles -T gba.ld -o gbadhoc-chain.elf \
	  crt0.S chain.c -lgcc
	$(CROSS)objcopy -O binary gbadhoc-chain.elf $@
	@ls -l $@

# OBJ_EXACT (app_tests.py, objprio): sprites out of priority order, objprio.c.
.PHONY: objprio
objprio: gbadhoc-objprio.gba
gbadhoc-objprio.gba: crt0.S objprio.c gba.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -nostartfiles -T gba.ld -o gbadhoc-objprio.elf \
	  crt0.S objprio.c -lgcc
	$(CROSS)objcopy -O binary gbadhoc-objprio.elf $@
	@ls -l $@

# Self-modifying RAM code (app_tests.py, smc; SMC_RANGE): see smc.c.
.PHONY: smc
smc: gbadhoc-smc.gba
gbadhoc-smc.gba: crt0.S smc.c gba.ld
	$(CROSS)gcc $(CFLAGS) -nostdlib -nostartfiles -T gba.ld -o gbadhoc-smc.elf \
	  crt0.S smc.c -lgcc
	$(CROSS)objcopy -O binary gbadhoc-smc.elf $@
	@ls -l $@
