# tools/gbamirror/Makefile -- desktop prototype of the GBA link mirror.
#
# Linux/WSL only.  Builds the unix core (x86 dynarec) with GBA_LINK=1,
# partially links every core object into one relocatable, and makes instance
# B by renaming every global that relocatable defines to gbab_<name> (the
# same move as the GB link's TGB Dual instance B).  gl_host links both.
#
#   make           -> gl_host
#   make CORE_EXTRA=-DGBALINK_HALT_BLOCK_EXACT
#                  -> gl_host with extra core flags (the opt-in in
#                     gbalink_core.c that makes a halted parent's transfer end
#                     independent of the lookahead; tools/linktest/README.md)
#   Makefile.armhf -> the same tool on the ARM dynarec, run under qemu-arm
CORE_DIR := ../..
CC       ?= gcc
CFLAGS   := -O2 -g -Wall -I$(CORE_DIR)/libretro/libretro-common/include
CORE_EXTRA ?=

# CORE_DEFS: extra core defines for a variant, e.g.
#   make CORE_DEFS="-DSCHED_FAST=1 -DSCHED_FAST_DEFAULT=1"
CORE_DEFS ?=

all: gl_host

core_a.o: FORCE
	$(MAKE) -C $(CORE_DIR) platform=unix GBA_LINK=1 clean
	$(MAKE) -C $(CORE_DIR) platform=unix GBA_LINK=1 STATIC_LINKING=1 \
	  EXTRA_CFLAGS="$(CORE_EXTRA)" CODE_DEFINES="$(CORE_DEFS)" -j$$(nproc)
	cd $(CORE_DIR) && ld -r -o tools/gbamirror/core_a.o \
	  $$(find . -name '*.o' \
	       -not -path './sdl/*' -not -path './psp/*' \
	       -not -path './frontend-common/*' -not -path './netdrv/*' \
	       -not -path './tools/*' -not -path './tests/*' \
	       -not -path './gbcore/*' -not -path './gbalink/*' \
	       -not -path './mips/*' -not -path './arm/*' \
	       -not -path './3ds/*' -not -path './jni/*')

# Instance B: every DEFINED global (including weak ones) renamed gbab_*;
# undefined references (libc, the link bus) are left alone, so B shares
# them with A.
core_b.o: core_a.o
	nm -g --defined-only core_a.o | awk 'NF == 3 { print $$3 " gbab_" $$3 }' > core_b.syms
	objcopy --redefine-syms=core_b.syms core_a.o core_b.o

# The real autopilot engine, twice: slot 0's copy as built, slot 1's with
# its API renamed gbab_fe_autopilot_* and its three host hooks pointed at
# slot 1's (gl_host.c defines both sets).  Same script semantics on both
# machines as on the PSP harness, RAM predicates included.
FE := $(CORE_DIR)/frontend-common
AP_CFLAGS := -O2 -g -Wall -DGPSP_PERF_RIG -I$(FE) -I$(CORE_DIR)/netdrv \
             -I$(CORE_DIR)/libretro/libretro-common/include
ap_a.o: $(FE)/fe_autopilot.c
	$(CC) $(AP_CFLAGS) -c -o $@ $<
ap_b.o: ap_a.o
	nm -g --defined-only ap_a.o | awk 'NF == 3 { print $$3 " gbab_" $$3 }' > ap_b.syms
	printf '%s\n' 'fe_host_input_inject gbab_fe_host_input_inject' \
	  'fe_host_mem_read gbab_fe_host_mem_read' \
	  'fe_host_sram_crc_now gbab_fe_host_sram_crc_now' >> ap_b.syms
	objcopy --redefine-syms=ap_b.syms ap_a.o ap_b.o
fe_evt.o: $(FE)/fe_evt.c
	$(CC) $(AP_CFLAGS) -c -o $@ $<

gl_host: gl_host.c core_a.o core_b.o ap_a.o ap_b.o fe_evt.o $(CORE_DIR)/gbalink/gbalink_bus.c $(CORE_DIR)/gbalink/gbalink_pair.c $(CORE_DIR)/gbalink/gbalink_script.c
	$(CC) $(CFLAGS) -o $@ gl_host.c $(CORE_DIR)/gbalink/gbalink_bus.c \
	  $(CORE_DIR)/gbalink/gbalink_pair.c $(CORE_DIR)/gbalink/gbalink_script.c \
	  core_a.o core_b.o ap_a.o ap_b.o fe_evt.o -lm -lpthread -lstdc++

clean:
	rm -f gl_host core_a.o core_b.o core_b.syms ap_a.o ap_b.o ap_b.syms fe_evt.o

FORCE:
.PHONY: all clean FORCE
