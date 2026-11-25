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
#
# HOST_OS=macos (hostcore/Makefile ARCH=macos): Mach-O has no `ld -r -T`
# script and no `objcopy -w`, so every partial link below has a twin that
# compiles the objects of an instance with its writable data in sections of
# its own (macho_sect.h, the section$start$ symbols mark them) and renames the
# globals with llvm-objcopy (tools/build/macho-group.sh).  The ELF rules are
# unchanged.  The includer sets NM and OBJCOPY to llvm-nm and llvm-objcopy,
# SYM_US to "_", AR, and links the MACHO_ARCHIVES it gets back with
# -Wl,-force_load (the whole archive: what the relocatables were).

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
# macOS only (see the header): the group-rename helper and the archives the
# includer links whole.
MACHO_GROUP := $(GP)/../tools/build/macho-group.sh
MACHO_ARCHIVES :=

# ---- flag tracking -------------------------------------------------------
# (the stamps name the compiler WITHOUT the ccache prefix: turning ccache on or
# off must not make every object look stale.)
# make does not see a changed -D.  A core built with stale objects after a
# flag change silently keeps the old emulator (docs: "the franken-build
# trap", ADR-0035).  Every object depends on this stamp, which is rewritten
# only when the flag text differs.
# (written with printf, not $(file ...): macOS ships GNU make 3.81.)
shq = '$(subst ','\'',$(1))'
FLAGS_STAMP := $(B)/.flags
FLAGS_TEXT   = $(call ccplain,$(CC)) | $(CORE_CFLAGS)
$(shell mkdir -p $(B))
$(FLAGS_STAMP): FORCE
	@printf '%s\n' $(call shq,$(FLAGS_TEXT)) > $@.new
	@cmp -s $@.new $@ || cp $@.new $@
	@rm -f $@.new
# The frontend layer (frontend-common, netdrv, gbalink, gbcore) has its own
# stamp: its flags carry netdrv's sizing (-DND_WINDOW...), which the GBA core
# never sees, so changing it rebuilds a dozen files and not the core.
FE_STAMP := $(B)/.feflags
$(FE_STAMP): FORCE
	@printf '%s\n' $(call shq,$(call ccplain,$(CC)) | $(FE_FLAGS) | $(FE_INC) | $(TGB_CFLAGS)) > $@.new
	@cmp -s $@.new $@ || cp $@.new $@
	@rm -f $@.new
# The same for the includer's own objects: APP_FLAGS (theirs to define) and
# the git revision.  Kept apart from the core's stamp so that a new commit
# rebuilds the app and the one core file that carries the revision string,
# not every core object.
APP_STAMP := $(B)/.appflags
$(APP_STAMP): FORCE
	@printf '%s\n' $(call shq,$(call ccplain,$(CC)) | $(APP_FLAGS) | $(GIT_VERSION)) > $@.new
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

# The .incbin is invisible to make: rebuild when the BIOS image changes.
$(B)/core/bios_data.o: $(GP)/bios/open_gba_bios.bin

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
ifneq ($(HOST_OS),macos)
READELF ?= $(subst objcopy,readelf,$(OBJCOPY))
$(CORE_B): $(CORE_A) $(GP)/vita/gba_instance_b.ld $(GP)/vita/check_instance_b.sh
	$(LD_R) -r -d -T $(GP)/vita/gba_instance_b.ld -o $@.tmp --whole-archive $(CORE_A)
	sh $(GP)/vita/check_instance_b.sh $(NM) $(READELF) $@.tmp
	$(NM) -g --defined-only $@.tmp | awk 'NF == 3 { print $$3 " gbab_" $$3 }' > $@.syms
	$(OBJCOPY) --redefine-syms=$@.syms $@.tmp $@
	@rm -f $@.tmp
else
# Mach-O: the same sources compiled again with B's .data and .bss in the
# sections __gbab_d and __gbab_z (the markers fe_gbadual.c reads are
# section$start$/section$end$ of those two), then every global renamed
# gbab_*.  (No fe_gbadual_boot size check by script here: fe_gbadual_boot
# checks the same markers at run time.)
CORE_B   := $(B)/gpsp_b.a
CORE_B_D := $(B)/coreb
CORE_B_OBJS := $(patsubst $(GP)/%.c,$(CORE_B_D)/%.o,$(SOURCES_C)) \
               $(patsubst $(GP)/%.S,$(CORE_B_D)/%.o,$(SOURCES_ASM)) \
               $(patsubst $(GP)/%.cc,$(CORE_B_D)/%.o,$(SOURCES_CC))
$(CORE_B_D)/sect.h: FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' '#pragma clang section bss="__DATA,__gbab_z,zerofill" data="__DATA,__gbab_d"' > $@.new
	@cmp -s $@.new $@ || cp $@.new $@
	@rm -f $@.new
$(CORE_B_D)/libretro/libretro.o: CORE_FILE_FLAGS := -DGIT_VERSION=\"$(GIT_VERSION)\"
$(CORE_B_D)/libretro/libretro.o: $(APP_STAMP)
$(CORE_B_D)/%.o: $(GP)/%.c $(FLAGS_STAMP) $(CORE_B_D)/sect.h
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) $(CORE_FILE_FLAGS) -include $(CORE_B_D)/sect.h -c -o $@ $<
$(CORE_B_D)/%.o: $(GP)/%.cc $(FLAGS_STAMP) $(CORE_B_D)/sect.h
	@mkdir -p $(dir $@)
	$(CXX) $(CORE_CXXFLAGS) -include $(CORE_B_D)/sect.h -c -o $@ $<
$(CORE_B_D)/%.o: $(GP)/%.S $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CORE_ARCH_FLAGS) $(CORE_DEFS) $(INCFLAGS) -I$(GP) -c -o $@ $<
$(CORE_B_D)/bios_data.o: $(GP)/bios/open_gba_bios.bin
$(CORE_B): $(CORE_B_OBJS) $(MACHO_GROUP)
	sh $(MACHO_GROUP) $(NM) $(OBJCOPY) '$(SYM_US)' $@.d gbab_ '' '' '' $(CORE_B_OBJS)
	@rm -f $@
	$(AR) rcs $@ $@.d/*.o
	@rm -rf $@.d
MACHO_ARCHIVES += $(CORE_B)
-include $(CORE_B_OBJS:.o=.d)
endif
else
CORE_B :=
endif

# ---- the render thread's renderer (render_thread.h) --------------------------
# gpsp/video.cc a second time, as the PSP's Media Engine builds it
# (ME_PRX_BUILD: no capture side), with its memory (vita/render_data.c);
# every global the pair defines renamed gbar_<name>.  It leaves only libc
# undefined.
ifneq ($(HOST_OS),macos)
CORE_R := $(B)/gpsp_r.o
else
CORE_R := $(B)/gpsp_r.a
endif
$(B)/render/video_r.o: $(GP)/video.cc $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(CORE_CXXFLAGS) -DME_PRX_BUILD -c -o $@ $<
$(B)/render/render_data.o: $(GP)/vita/render_data.c $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) -c -o $@ $<
ifneq ($(HOST_OS),macos)
$(CORE_R): $(B)/render/video_r.o $(B)/render/render_data.o
	$(LD_R) -r -o $@.tmp $^
	$(NM) -g --defined-only $@.tmp | awk 'NF == 3 { print $$3 " gbar_" $$3 }' > $@.syms
	$(OBJCOPY) --redefine-syms=$@.syms $@.tmp $@
	@rm -f $@.tmp
else
$(CORE_R): $(B)/render/video_r.o $(B)/render/render_data.o $(MACHO_GROUP)
	sh $(MACHO_GROUP) $(NM) $(OBJCOPY) '$(SYM_US)' $@.d gbar_ '' '' '' $(B)/render/video_r.o $(B)/render/render_data.o
	@rm -f $@
	$(AR) rcs $@ $@.d/*.o
	@rm -rf $@.d
MACHO_ARCHIVES += $(CORE_R)
endif

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

ifneq ($(HOST_OS),macos)
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
else
# Mach-O: each instance is compiled with its own data sections (__tgba_* and
# __tgbb_*, which tgb_image_* of gbcore_tgbdual.c name), every global but
# gbcore_* renamed private; B's API is then renamed gbcoreb_*.
GB_OBJS := $(filter-out %_linked.o %_linked_b.o,$(GB_OBJS))
define TGB_INST
TGBI_$(1) := $$(patsubst $$(B)/gbcore/%,$$(B)/gbcore_$(1)/%,$$(GB_CORE_OBJS))
$$(B)/gbcore_$(1)/sect.h: FORCE
	@mkdir -p $$(dir $$@)
	@printf '%s\n' '#pragma clang section bss="__DATA,__tgb$(1)_z,zerofill" data="__DATA,__tgb$(1)_d"' \
	  '#define TGB_SECT_D "__tgb$(1)_d"' '#define TGB_SECT_Z "__tgb$(1)_z"' > $$@.new
	@cmp -s $$@.new $$@ || cp $$@.new $$@
	@rm -f $$@.new
$$(B)/gbcore_$(1)/%.o: $$(GP)/gbcore/%.c $$(FE_STAMP) $$(B)/gbcore_$(1)/sect.h
	@mkdir -p $$(dir $$@)
	$$(CC) $$(FE_FLAGS) $$(FE_INC) -I$$(GP)/gbcore $$(FE_EXTRA) -include $$(B)/gbcore_$(1)/sect.h -MMD -MP -c -o $$@ $$<
$$(patsubst $$(B)/gbcore/%,$$(B)/gbcore_$(1)/%,$$(TGB_OBJS)): FE_EXTRA := $$(TGB_CFLAGS)
-include $$(TGBI_$(1):.o=.d)
endef
$(eval $(call TGB_INST,a))
$(eval $(call TGB_INST,b))
$(B)/gbcore_a.a: $(TGBI_a) $(MACHO_GROUP)
	sh $(MACHO_GROUP) $(NM) $(OBJCOPY) '$(SYM_US)' $@.d tgba_ '^gbcore_' '' '' $(TGBI_a)
	@rm -f $@
	$(AR) rcs $@ $@.d/*.o
	@rm -rf $@.d
$(B)/gbcore_b.a: $(TGBI_b) $(MACHO_GROUP)
	sh $(MACHO_GROUP) $(NM) $(OBJCOPY) '$(SYM_US)' $@.d tgbb_ '^gbcore_' gbcore_ gbcoreb_ $(TGBI_b)
	@rm -f $@
	$(AR) rcs $@ $@.d/*.o
	@rm -rf $@.d
MACHO_ARCHIVES += $(B)/gbcore_a.a $(B)/gbcore_b.a
endif

# ---- the NES core (FCEUmm), in-process like TGB Dual: nescore.mk
include $(GP)/vita/nescore.mk
GB_OBJS += $(NES_OBJS)
# ---- the WonderSwan core (Beetle), in-process the same way: wswancore.mk
include $(GP)/vita/wswancore.mk
GB_OBJS += $(WS_OBJS)
ifeq ($(HOST_OS),macos)
MACHO_ARCHIVES += $(NES_PRIV) $(WS_PRIV)
endif

# ---- frontend-common, netdrv core, the cable bus ----------------------------
FE_INC  := -I$(GP)/frontend-common -I$(GP)/netdrv \
           -I$(GP)/libretro/libretro-common/include -I$(GP)
FE_OBJS := $(addprefix $(B)/frontend-common/,fe_host.o fe_evt.o fe_util.o \
             fe_gblink.o fe_padpair.o fe_linkmode.o fe_favs.o fe_collect.o fe_autopilot.o \
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
	$(NM) -g --defined-only $< | awk -v u='$(SYM_US)' 'NF == 3 { print $$3 " " u "gbab_" substr($$3, length(u) + 1) }' > $@.syms
	printf '%s\n' '$(SYM_US)fe_host_input_inject $(SYM_US)gbab_fe_host_input_inject' \
	  '$(SYM_US)fe_host_mem_read $(SYM_US)gbab_fe_host_mem_read' \
	  '$(SYM_US)fe_host_sram_crc_now $(SYM_US)gbab_fe_host_sram_crc_now' >> $@.syms
	$(OBJCOPY) --redefine-syms=$@.syms $< $@

# Every object compiled with -MMD -MP has a .d: the render thread's pair and the
# GB pair glue (gbcore_dual.o, tgb_shared.o) did not have theirs read, so a header
# change under them rebuilt nothing.  (The *_linked*.o files are partial links and
# have no .d; a missing one is ignored.)
CORE_DEPS := $(CORE_OBJS:.o=.d) $(GB_OBJS:.o=.d) $(GB_CORE_OBJS:.o=.d) $(FE_OBJS:.o=.d) \
             $(B)/render/video_r.d $(B)/render/render_data.d
-include $(CORE_DEPS)
