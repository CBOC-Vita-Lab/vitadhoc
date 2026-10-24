# vita/corebuild.mk -- build the emulator cores OUT OF TREE for one target.
#
# Included by gpsp/vita/Makefile (arm-vita-eabi) and by
# gpsp/vita/hostcore/Makefile (x86-64 and armhf Linux twins).  The root
# Makefile compiles every object next to its source, so two targets built
# from one checkout overwrite each other's .o files (sdl/Makefile and
# tools/gbamirror work around that with `make clean` first; ADR-0005 calls it
# the stale-arch trap).  Here every object lives under $(B), per target.
#
# The includer sets:
#   GP              path of the gpsp/ directory
#   B               build directory for this target
#   CC CXX AR LD NM OBJCOPY
#   LD_R            partial-link command (LD plus any -m emulation)
#   CPU_ARCH        arm | x86_32      HAVE_DYNAREC  1 | 0
#   CORE_ARCH_FLAGS target/arch compiler flags for the GBA core
#   CORE_PLAT_DEFS  platform defines for the core (-DVITA, -DMMAP_JIT_CACHE...)
#   FE_FLAGS        compiler flags for frontend-common, netdrv, gbalink, gbcore
#   CORE_OPT        optimisation for the GBA core (default -O3 -DNDEBUG)
# and gets back:
#   CORE_A          instance A of the GBA core, an archive
#   CORE_B          instance B: the same objects, every global renamed gbab_*
#   CORE_R          the picture renderer again, globals renamed gbar_* (the
#                   render thread, render_thread.h)
#   GB_OBJS         TGB Dual twice (gbcore_* and gbcoreb_*) plus the pair glue
#   FE_OBJS         frontend-common + netdrv core + gbalink bus
#   FLAGS_STAMP     prerequisite that changes when a core flag does
#   FE_STAMP        ...when a frontend-layer flag (FE_FLAGS) does
#   APP_STAMP       prerequisite that changes when APP_FLAGS or the git
#                   revision does (for the includer's own objects)
#
# docs: GBA-LINK-MIRROR.md 1 (instance B), GB-LINK.md 1 (the GB pair).

include $(GP)/vita/coreflags.mk

CORE_OPT ?= -O3 -DNDEBUG
CORE_DIR := $(GP)
include $(GP)/Makefile.common

GIT_VERSION := $(shell git -C $(GP) rev-parse --short HEAD 2>/dev/null || echo unknown)

CORE_DEFS := -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H -D__LIBRETRO__ \
             -DINLINE=inline -DFRONTEND_SUPPORTS_RGB565 \
             $(GBADHOC_CORE_DEFS) $(CORE_PLAT_DEFS)
ifeq ($(HAVE_DYNAREC),1)
CORE_DEFS += -DHAVE_DYNAREC
endif
ifeq ($(CPU_ARCH),arm)
CORE_DEFS += -DARM_ARCH
else ifeq ($(CPU_ARCH),x86_32)
CORE_DEFS += -DX86_ARCH
endif
ifeq ($(GBA_LINK),1)
CORE_DEFS += -DGBA_LINK
endif

CORE_CFLAGS   := $(CORE_ARCH_FLAGS) $(CORE_OPT) -Wall $(CORE_DEFS) $(INCFLAGS) \
                 -MMD -MP $(EXTRA_CFLAGS)
CORE_CXXFLAGS := $(CORE_CFLAGS) -fno-rtti -fno-exceptions -std=c++11

CORE_OBJS := $(patsubst $(GP)/%.c,$(B)/core/%.o,$(SOURCES_C)) \
             $(patsubst $(GP)/%.S,$(B)/core/%.o,$(SOURCES_ASM)) \
             $(patsubst $(GP)/%.cc,$(B)/core/%.o,$(SOURCES_CC))

CORE_A := $(B)/libgpsp_a.a
CORE_B := $(B)/gpsp_b.o

# ---- flag tracking -------------------------------------------------------
# make does not see a changed -D.  A core built with stale objects after a
# flag change silently keeps the old emulator (docs: "the franken-build
# trap", ADR-0035).  Every object depends on this stamp, which is rewritten
# only when the flag text differs.
FLAGS_STAMP := $(B)/.flags
FLAGS_TEXT   = $(CC) | $(CORE_CFLAGS)
$(shell mkdir -p $(B))
$(FLAGS_STAMP): FORCE
	$(file >$@.new,$(FLAGS_TEXT))
	@cmp -s $@.new $@ || cp $@.new $@
	@rm -f $@.new
# The frontend layer (frontend-common, netdrv, gbalink, gbcore) has its own
# stamp: its flags carry netdrv's sizing (-DND_WINDOW...), which the GBA core
# never sees, so changing it rebuilds a dozen files and not the core.
FE_STAMP := $(B)/.feflags
$(FE_STAMP): FORCE
	$(file >$@.new,$(CC) | $(FE_FLAGS) | $(FE_INC) | $(TGB_CFLAGS))
	@cmp -s $@.new $@ || cp $@.new $@
	@rm -f $@.new
# The same for the includer's own objects: APP_FLAGS (theirs to define) and
# the git revision.  Kept apart from the core's stamp so that a new commit
# rebuilds the app and the one core file that carries the revision string,
# not every core object.
APP_STAMP := $(B)/.appflags
$(APP_STAMP): FORCE
	$(file >$@.new,$(CC) | $(APP_FLAGS) | $(GIT_VERSION))
	@cmp -s $@.new $@ || cp $@.new $@
	@rm -f $@.new
FORCE:
.PHONY: FORCE

# ---- the GBA core, instance A ---------------------------------------------
# libretro.c is the only core file that uses GIT_VERSION (the library
# version string).
$(B)/core/libretro/libretro.o: CORE_FILE_FLAGS := -DGIT_VERSION=\"$(GIT_VERSION)\"
$(B)/core/libretro/libretro.o: $(APP_STAMP)

$(B)/core/cpu_threaded.o: $(GP)/cpu_threaded.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -Wno-unused-variable -Wno-unused-label -c -o $@ $<

# CORE_FILE_FLAGS: empty; a target-specific value adjusts one object (the
# Vita simulation in hostcore/Makefile un-defines VITA for two files).
$(B)/core/%.o: $(GP)/%.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) $(CORE_FILE_FLAGS) -c -o $@ $<

$(B)/core/%.o: $(GP)/%.cc $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CORE_CXXFLAGS) -c -o $@ $<

# -I$(GP): bios_data.S .incbin's "bios/open_gba_bios.bin" relative to gpsp/.
$(B)/core/%.o: $(GP)/%.S $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CORE_ARCH_FLAGS) $(CORE_DEFS) $(INCFLAGS) -I$(GP) -c -o $@ $<

$(CORE_A): $(CORE_OBJS)
	@rm -f $@
	$(AR) rcs $@ $(CORE_OBJS)

# ---- instance B -------------------------------------------------------------
# The whole core partially linked into one relocatable, then every global it
# DEFINES renamed gbab_<name>.  What it leaves undefined -- libc, the cable
# bus (gbalink/gbalink_bus.c), and on the Vita the kernel stubs and
# gpsp_vita_jit_region() -- is shared with instance A.  The script only
# marks B's .data and .bss so fe_gbadual can put B back to a fresh boot's
# bytes before every load (vita/gba_instance_b.ld).
ifeq ($(GBA_LINK),1)
$(CORE_B): $(CORE_A) $(GP)/vita/gba_instance_b.ld
	$(LD_R) -r -d -T $(GP)/vita/gba_instance_b.ld -o $@.tmp --whole-archive $(CORE_A)
	$(NM) -g --defined-only $@.tmp | awk 'NF == 3 { print $$3 " gbab_" $$3 }' > $@.syms
	$(OBJCOPY) --redefine-syms=$@.syms $@.tmp $@
	@rm -f $@.tmp
else
CORE_B :=
endif

# ---- the render thread's renderer (render_thread.h) --------------------------
# gpsp/video.cc a second time, as the PSP's Media Engine builds it
# (ME_PRX_BUILD: no capture side), with its memory (vita/render_data.c);
# every global the pair defines renamed gbar_<name>.  It leaves only libc
# undefined.
CORE_R := $(B)/gpsp_r.o
$(B)/render/video_r.o: $(GP)/video.cc $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CORE_CXXFLAGS) -DME_PRX_BUILD -c -o $@ $<
$(B)/render/render_data.o: $(GP)/vita/render_data.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -c -o $@ $<
$(CORE_R): $(B)/render/video_r.o $(B)/render/render_data.o
	$(LD_R) -r -o $@.tmp $^
	$(NM) -g --defined-only $@.tmp | awk 'NF == 3 { print $$3 " gbar_" $$3 }' > $@.syms
	$(OBJCOPY) --redefine-syms=$@.syms $@.tmp $@
	@rm -f $@.tmp

# ---- TGB Dual (GB/GBC), two instances ------------------------------------
# Flags as psp/Makefile: GNU89 inline (non-static `inline` whose address is
# taken), no strict aliasing (VRAM/ROM read through word pointers).
TGB_OPT    ?= -O3
TGB_CFLAGS  = $(TGB_OPT) -fgnu89-inline -fno-strict-aliasing -Wno-unused-parameter
# The Super Game Boy's border is shown on the Vita (gbcore.h gbcore_sgb_border);
# the PSP keeps its 112 KiB (tgbdual/LOCAL-PATCHES.md).
FE_FLAGS += -DTGB_SGB_BORDER
TGB_OBJS   := $(addprefix $(B)/gbcore/tgbdual/,apu.o cpu.o gb.o lcd.o mbc.o rom.o sgb.o)
GB_CORE_OBJS := $(B)/gbcore/gbcore_tgbdual.o $(TGB_OBJS)
GB_OBJS    := $(B)/gbcore/gbcore_dual.o $(B)/gbcore/tgbdual/tgb_shared.o \
              $(B)/gbcore/gbcore_tgbdual_linked.o $(B)/gbcore/gbcore_tgbdual_linked_b.o

$(TGB_OBJS) $(B)/gbcore/tgbdual/tgb_shared.o: FE_EXTRA := $(TGB_CFLAGS)

# One relocatable whose only globals are the gbcore_* API (TGB exports ~780
# names such as `vram` and `ram`, which the GBA core defines too).
$(B)/gbcore/gbcore_tgbdual_linked.o: $(GB_CORE_OBJS) $(GP)/gbcore/gbcore_instance.ld
	$(LD_R) -r -T $(GP)/gbcore/gbcore_instance.ld -o $@.tmp $(GB_CORE_OBJS)
	$(OBJCOPY) -w --keep-global-symbol='gbcore_*' $@.tmp $@
	@rm -f $@.tmp

# Instance B: the same bytes with the API renamed gbcoreb_* (fails if
# anything but gbcore_* is still global).
$(B)/gbcore/gbcore_tgbdual_linked_b.o: $(B)/gbcore/gbcore_tgbdual_linked.o
	$(NM) -g --defined-only $< | awk '$$3 !~ /^gbcore_/ { bad = 1 } \
	  { print $$3 " gbcoreb_" substr($$3, 8) } END { exit bad }' > $@.syms
	$(OBJCOPY) --redefine-syms=$@.syms $< $@

# ---- frontend-common, netdrv core, the cable bus ----------------------------
FE_INC  := -I$(GP)/frontend-common -I$(GP)/netdrv \
           -I$(GP)/libretro/libretro-common/include -I$(GP)
FE_OBJS := $(addprefix $(B)/frontend-common/,fe_host.o fe_evt.o fe_util.o \
             fe_gblink.o fe_linkmode.o fe_favs.o fe_autopilot.o \
             netpacket_host.o fe_share.o) \
           $(B)/netdrv/gb_link.o $(B)/netdrv/netdrv.o
ifeq ($(GBA_LINK),1)
FE_FLAGS += -DGBA_LINK
FE_OBJS += $(B)/frontend-common/fe_gbadual.o $(B)/frontend-common/fe_autopilot_b.o \
           $(addprefix $(B)/gbalink/,gbalink_bus.o gbalink_pair.o gbalink_script.o)
endif

$(B)/frontend-common/%.o: $(GP)/frontend-common/%.c $(FE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(FE_FLAGS) $(FE_INC) $(FE_EXTRA) -MMD -MP -c -o $@ $<
$(B)/netdrv/%.o: $(GP)/netdrv/%.c $(FE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(FE_FLAGS) $(FE_INC) $(FE_EXTRA) -MMD -MP -c -o $@ $<
$(B)/gbalink/%.o: $(GP)/gbalink/%.c $(FE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(FE_FLAGS) $(FE_INC) $(FE_EXTRA) -MMD -MP -c -o $@ $<
$(B)/gbcore/%.o: $(GP)/gbcore/%.c $(FE_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(FE_FLAGS) $(FE_INC) -I$(GP)/gbcore $(FE_EXTRA) -MMD -MP -c -o $@ $<

# The partner machine's autopilot (fe_gbadual.c calls gbab_fe_autopilot_*):
# fe_autopilot.o again, its API renamed and its three host hooks pointed at
# the partner's (psp/Makefile fe_autopilot_b.o).
$(B)/frontend-common/fe_autopilot_b.o: $(B)/frontend-common/fe_autopilot.o
	$(NM) -g --defined-only $< | awk 'NF == 3 { print $$3 " gbab_" $$3 }' > $@.syms
	printf '%s\n' 'fe_host_input_inject gbab_fe_host_input_inject' \
	  'fe_host_mem_read gbab_fe_host_mem_read' \
	  'fe_host_sram_crc_now gbab_fe_host_sram_crc_now' >> $@.syms
	$(OBJCOPY) --redefine-syms=$@.syms $< $@

CORE_DEPS := $(CORE_OBJS:.o=.d) $(GB_CORE_OBJS:.o=.d) $(FE_OBJS:.o=.d)
-include $(CORE_DEPS)
