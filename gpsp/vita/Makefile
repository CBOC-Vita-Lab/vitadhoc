# gpsp/vita/Makefile -- GBAdhoc for the PS Vita.
#
#   make            build/vita/eboot.bin   (all cores + the app)
#   make vpk        VitAdhoc.vpk           (installable with VitaShell)
#   make report     sizes, static memory per core, imported modules
#   make assets     regenerate sce_sys/*.png from the logo (needs Pillow)
#   make clean
#
# Needs only the base vitasdk (VITASDK, default /opt/vitasdk): no vita2d, no
# SDL, no zlib/libpng.
#
# WHAT IS BUILT (corebuild.mk does the cores; this file the app and the VPK)
#   libgpsp_a.a     the GBA core, instance A: arm-vita-eabi, ARM dynarec,
#                   GBA_LINK, standard RGB565, the switches in coreflags.mk
#   gpsp_b.o        instance B: the same objects partially linked, every
#                   global renamed gbab_* (the link partner's machine)
#   gbcore_tgbdual_linked{,_b}.o   TGB Dual (GB/GBC), two instances
#   frontend-common fe_host and friends, netdrv's core, the cable bus
#   the app         app.mk: main_vita.c, the PSP UI compiled -DVITA,
#                   video_vita.c on GXM (vendored libvita2d), pspcompat.c,
#                   the network transports, plat_vita.c, audio, the JIT
#                   memory and heap arrangement
#
# VARIABLES
#   VERSION=4.1.1        shown in the log and written to PARAM.SFO (APP_VER)
#   TITLE_ID=GBAD00001   the bubble's id (4 letters + 5 digits)
#   ARM_FAR_STUBS=1      build the core with -DARM_FAR_STUBS (arm/: stubs
#                        reached through veneers instead of a 24-bit bl)
#   GBA_LINK=0           single core, no link partner (bisecting only)
#   VITA_ATTRIBUTE2=12   PARAM.SFO ATTRIBUTE2: the memory budget.  0 = the
#                        standard 256 MiB; 4 / 8 / 12 = +29 / +77 / +109 MiB.
#                        12 is vita-mksfoex's own default; it is passed
#                        explicitly so the SFO does not change with the
#                        toolchain.  The heap fits either way (heap_vita.c).
#   UNSAFE=1             an "unsafe" eboot (extended permissions).  Nothing
#                        here needs it; a kernel-plugin feature would.
#   EXTRA_CFLAGS=...     one-off core defines
#
# WHAT THE APP IS MADE OF is not listed here: app.mk has the one list of
# sources (shared with hostcore/Makefile, the Linux twin of the whole app)
# and the convention by which cheats.mk and link.mk add to it:
#
#   APP_SRCS       += file.c ...       built for the Vita and the twin
#   APP_SRCS_VITA  += file.c ...       the Vita only
#   APP_SRCS_HOST  += file.c ...       the twin only
#   APP_DEFS       += -D...
#   APP_INCDIRS    += dir ...
#   APP_LIBS_VITA  += -lSceXxx_stub
#   VPK_EXTRA      += file=path/in/the/vpk
#
# (paths relative to this directory).  ui.mk and net.mk are the UI's and the
# network's own lists; app.mk reads them as they are.

VITASDK ?= /opt/vitasdk
export VITASDK
PREFIX  := $(VITASDK)/bin/arm-vita-eabi-
CC      := $(PREFIX)gcc
CXX     := $(PREFIX)g++
AR      := $(PREFIX)ar
NM      := $(PREFIX)nm
OBJCOPY := $(PREFIX)objcopy
SIZE    := $(PREFIX)size
READELF := $(PREFIX)readelf
LD_R    := $(PREFIX)ld

VERSION  ?= 4.1.1
TITLE    ?= VitAdhoc
TITLE_ID ?= GBAD00001
VITA_ATTRIBUTE2 ?= 12
UNSAFE   ?= 0

GP := ..
B  := build/vita

# ---- what the app is made of ----------------------------------------------------
V := .
include app.mk

# ---- the cores ----------------------------------------------------------------
CPU_ARCH     := arm
HAVE_DYNAREC := 1
# The root Makefile's vita section, minus -mword-relocations (a RetroArch
# dynamic-core need: vita-elf-create relocates movw/movt pairs) and plus an
# explicit FPU.  -fno-optimize-sibling-calls is upstream's: the stubs rely on
# C helpers returning to them.
CORE_ARCH_FLAGS := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
                   -fomit-frame-pointer -ffast-math \
                   -fno-unwind-tables -fno-asynchronous-unwind-tables \
                   -ftree-vectorize -fno-optimize-sibling-calls
CORE_PLAT_DEFS  := -DVITA
# ND_DEFS: netdrv's sizing reaches fe_host.c and netpacket_host.c too.
FE_FLAGS        := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
                   -O2 -g -Wall -Wextra -DVITA $(ND_DEFS)

include corebuild.mk

# ---- the app --------------------------------------------------------------------
APP_DEFS += -DGBADHOC_VERSION=\"$(VERSION)\" \
            $(if $(filter 1,$(GBA_LINK)),-DGBA_LINK) \
            $(if $(filter 1,$(ARM_FAR_STUBS)),-DARM_FAR_STUBS)
# -Wno-type-limits: psp/ui_psp.h compares an enum with 0, and an ARM EABI
# enum is unsigned.
APP_WARN := -Wall -Wextra -Wno-type-limits -std=gnu11
APP_OBJS := $(foreach s,$(APP_ALL_VITA),$(call app_obj,$(s)))
# What the app calls, each list after what uses it: the UI and the renderer
# (ui.mk), the transports (net.mk), then plat_vita.c's own.  arm-vita-eabi-
# gcc's specs add newlib's needs (SceLibKernel, SceIofilemgr, SceNet, ...).
# The GPU programs' text (shaders/*.inc) as C strings for vback_vita2d.c,
# which hands it to the console's own Cg compiler at run time.
GPU_SHADERS := shaders/xbrz_common.inc shaders/xbrz_pass0.inc \
               shaders/xbrz_pass1.inc shaders/xbrz_pass2.inc
$(B)/gen/gpu_shaders.h: $(GPU_SHADERS)
	@mkdir -p $(dir $@)
	@{ echo "/* generated from $(GPU_SHADERS): do not edit */"; \
	   for f in $^; do n=$$(basename $$f .inc); \
	     echo "static const char gpu_src_$$n[] ="; \
	     sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $$f; \
	     echo ";"; done; } > $@.tmp && mv $@.tmp $@
$(B)/app/vita/vback_vita2d.o: $(B)/gen/gpu_shaders.h
$(B)/app/vita/vback_vita2d.o: APP_FILE_FLAGS := -I$(B)/gen

LIBS     := $(APP_LIBS_VITA) $(VITA_UI_LIBS) $(NET_VITA_LIBS) \
            -lSceShaccCg_stub_weak \
            -lSceAppUtil_stub -lSceCommonDialog_stub -lSceDisplay_stub \
            -lSceCtrl_stub -lSceAudio_stub -lScePower_stub -lSceRtc_stub \
            -lSceSysmem_stub -lSceLibKernel_stub -lm
# A private build may carry its owner's own GBA BIOS dump, read by the app
# from app0:gba_bios.bin when ux0:data/GBAdhoc has none (main_vita.c
# app_bios_dir).  The file is never part of the repository:
#     make vpk BIOS=/path/to/gba_bios.bin
ifneq ($(strip $(BIOS)),)
VPK_EXTRA += $(BIOS)=gba_bios.bin
endif
DIST_NOTICES := $(wildcard ../../licenses/*) $(wildcard ../../notices/*)
VPK_EXTRA += $(foreach f,$(DIST_NOTICES),$(f)=$(patsubst ../../%,%,$(f)))
VPK_FILES := -a sce_sys/icon0.png=sce_sys/icon0.png \
             -a sce_sys/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png \
             -a sce_sys/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png \
             -a sce_sys/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml \
             $(addprefix -a ,$(VPK_EXTRA))
VPK_EXTRA_FILES := $(foreach x,$(VPK_EXTRA),$(firstword $(subst =, ,$(x))))

APP_FLAGS = -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -O2 -g \
            $(APP_WARN) $(APP_DEFS) $(APP_INC_FLAGS) $(FE_INC) $(ND_DEFS) \
            -MMD -MP

ELF  := $(B)/gbadhoc.elf
VELF := $(B)/gbadhoc.velf
SELF := $(B)/eboot.bin
SFO  := $(B)/param.sfo
VPK  := VitAdhoc.vpk
LIVEAREA := sce_sys/icon0.png sce_sys/livearea/contents/bg.png \
            sce_sys/livearea/contents/startup.png \
            sce_sys/livearea/contents/template.xml

.DEFAULT_GOAL := all
all: $(SELF)
vpk: $(VPK)

$(foreach s,$(APP_ALL_VITA),$(eval $(call APP_RULE,$(s))))

# -Wl,-q keeps the relocations vita-elf-create turns into the SCE module's.
# Order: B before A is irrelevant to the result (their symbols are disjoint);
# the archive last, so only what something references is pulled from it.
LINK_ELF = $(CC) -Wl,-q -Wl,-Map,$(B)/gbadhoc.map -o $(ELF) $(APP_OBJS) \
	  $(VITA_UI_SHADERS) $(FE_OBJS) $(GB_OBJS) $(CORE_R) $(CORE_B) $(CORE_A) $(1) $(LIBS)

$(ELF): $(APP_OBJS) $(VITA_UI_SHADERS) $(FE_OBJS) $(GB_OBJS) $(CORE_R) $(CORE_B) $(CORE_A)
	$(call LINK_ELF,)

# ROOM FOR THE SCE DATA.  vita-elf-create writes its import tables (a few
# KiB) behind the last byte of segment 0 and gives up when segment 1, which
# the linker starts on the next 64 KiB boundary, is nearer than that:
#   "Cannot allocate 3420 bytes for SCE data at end of segment 0; segment 1
#    overlaps"
# (and then aborts in free()).  Whether it is nearer depends on the last 16
# bits of the program's size -- on any change anywhere; the rewind's code
# was the one that landed 2964 bytes short of a boundary.  When it happens
# the program is linked once more with 8 KiB of constants, which carries the
# end of segment 0 over the boundary and leaves 56 KiB behind it.
$(B)/elfpad.o:
	@mkdir -p $(dir $@)
	echo 'const char gbadhoc_elf_pad[8192] = { 1 };' | $(CC) -x c -c -o $@ -

# -v lists every import and relocation.  The whole listing is kept in the
# build directory; what is printed is everything that is not routine (any
# line vita-elf-create adds beyond its six section headers is a diagnostic),
# and the imports are distilled for `make report`.
$(VELF): $(ELF) $(B)/elfpad.o
	$(VITASDK)/bin/vita-elf-create -v $(ELF) $@ > $(B)/elf-create.log 2>&1 || \
	  { grep -q 'segment 1 overlaps' $(B)/elf-create.log && \
	    echo "  no room for the SCE data behind segment 0: linking again with padding" && \
	    $(call LINK_ELF,$(B)/elfpad.o) && \
	    $(VITASDK)/bin/vita-elf-create -v $(ELF) $@ > $(B)/elf-create.log 2>&1; } || \
	  { grep -v '^ ' $(B)/elf-create.log; exit 1; }
	@grep -v '^ ' $(B)/elf-create.log | grep -v -e '^Function stubs' \
	  -e '^Variable stubs' -e '^Relocations' -e '^Segments:' \
	  -e '^Total SCE data size' -e '^export config loaded' || true
	@sed -n 's/.*(\(Sce[A-Za-z0-9_]*\)) target_nid=[0-9A-Fa-fx]* (\(.*\))/\1 \2/p' \
	  $(B)/elf-create.log | sort > $(B)/imports.txt

# Safe: no extended permissions (the app only touches ux0:data/ and public
# APIs).  -c compresses the SELF.
$(SELF): $(VELF)
	$(VITASDK)/bin/vita-make-fself $(if $(filter 1,$(UNSAFE)),,-s) -c $< $@

# APP_VER is NN.NN.  3.5.0 -> 03.50; a third component is the second digit
# of the minor field.
APP_VER := $(shell echo $(VERSION) | awk -F. '{ printf "%02d.%d%d", $$1, $$2, $$3 }')
$(SFO): Makefile $(APP_STAMP)
	@mkdir -p $(dir $@)
	$(VITASDK)/bin/vita-mksfoex -s TITLE_ID=$(TITLE_ID) -s APP_VER=$(APP_VER) \
	  -d ATTRIBUTE2=$(VITA_ATTRIBUTE2) \
	  "$(TITLE)" $@

$(VPK): $(SELF) $(SFO) $(LIVEAREA) $(VPK_EXTRA_FILES)
	@rm -f $@
	$(VITASDK)/bin/vita-pack-vpk -s $(SFO) -b $(SELF) $(VPK_FILES) $@

# The LiveArea pictures are committed (a build needs no Pillow); this
# regenerates them from the logo.
assets:
	python3 tools/make_livearea.py

# Sizes and what the SELF imports.  "static" = .data + .bss an object adds to
# the image: memory the process holds whether or not that core ever runs.
report: $(SELF)
	@echo "== image (text data bss)"; $(SIZE) $(ELF) | tail -1
	@$(READELF) -lW $(VELF) | awk '$$1 == "LOAD" { print "segment at", $$3, "file", $$5, "mem", $$6, $$7 $$8 }'
	@ls -l $(SELF) $(wildcard $(VPK)) | awk '{ print $$5, $$9 }'
	@echo "== per core (text data bss, static = data + bss)"
	@$(SIZE) -t $(CORE_A) | tail -1 | awk '{ print "GBA instance A:", $$1, $$2, $$3, "static", $$2 + $$3 }'
ifneq ($(CORE_B),)
	@$(SIZE) $(CORE_B) | tail -1 | awk '{ print "GBA instance B:", $$1, $$2, $$3, "static", $$2 + $$3 }'
endif
	@$(SIZE) $(B)/gbcore/gbcore_tgbdual_linked.o | tail -1 | awk '{ print "GB  instance A:", $$1, $$2, $$3, "static", $$2 + $$3 }'
	@$(SIZE) $(B)/gbcore/gbcore_tgbdual_linked_b.o | tail -1 | awk '{ print "GB  instance B:", $$1, $$2, $$3, "static", $$2 + $$3 }'
	@$(SIZE) -t $(FE_OBJS) $(B)/gbcore/gbcore_dual.o $(B)/gbcore/tgbdual/tgb_shared.o | tail -1 | awk '{ print "frontend-common, netdrv, cable bus, GB pair:", $$1, $$2, $$3, "static", $$2 + $$3 }'
	@$(SIZE) -t $(APP_OBJS) | tail -1 | awk '{ print "the app:", $$1, $$2, $$3, "static", $$2 + $$3 }'
	@echo "== imports (module: functions)"
	@awk '{ n[$$1]++; f[$$1] = f[$$1] " " $$2 } END { for (m in n) printf "%-26s %2d:%s\n", m, n[m], f[m] }' $(B)/imports.txt | sort

clean:
	rm -rf $(B) $(VPK)

-include $(APP_OBJS:.o=.d)
.PHONY: all vpk assets report clean
