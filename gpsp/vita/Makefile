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
#   VERSION=4.1.1        the GBAdhoc release the core started from; written to
#                        PARAM.SFO (APP_VER)
#   CORE_VERSION=...     what the log, link compatibility and RetroAchievements
#                        report as the core: VERSION plus the 4.5.0 fixes taken in
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
# ccache (tools/build/ccache.mk): CC and CXX carry the prefix themselves, no
# symlinks; empty and a no-op when ccache is not installed (USE_CCACHE=0 too).
include ../../tools/build/ccache.mk
CC      := $(call ccw,$(PREFIX)gcc)
CXX     := $(call ccw,$(PREFIX)g++)
AR      := $(PREFIX)ar
NM      := $(PREFIX)nm
OBJCOPY := $(PREFIX)objcopy
SIZE    := $(PREFIX)size
READELF := $(PREFIX)readelf
LD_R    := $(PREFIX)ld

VERSION  ?= 4.1.1
# The core is GBAdhoc 4.1.1 plus a chosen set of fixes from 4.5.0 (CHANGELOG
# 1.3, docs/TECHNICAL.md), not 4.1.1 and not 4.5.0.
CORE_VERSION ?= $(VERSION)+4.5.0fixes
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
# C helpers returning to them.  -fno-math-errno, not -ffast-math (1.4,
# lessons-audit 10): the NES core's sound and palette use floats, and the
# standalone nes.bin never had fast-math; the twin's pictures and sound were
# byte-identical both ways (GBA, GB, GBC, NES NROM/MMC3, WonderSwan).
CORE_ARCH_FLAGS := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
                   -fomit-frame-pointer -fno-math-errno \
                   -fno-unwind-tables -fno-asynchronous-unwind-tables \
                   -ftree-vectorize -fno-optimize-sibling-calls
CORE_PLAT_DEFS  := -DVITA
# ND_DEFS: netdrv's sizing reaches fe_host.c and netpacket_host.c too.
FE_FLAGS        := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
                   -O2 -g -Wall -Wextra -DVITA $(ND_DEFS)

include corebuild.mk

# ---- the app --------------------------------------------------------------------
APP_DEFS += -DGBADHOC_VERSION=\"$(CORE_VERSION)\" \
            $(if $(filter 1,$(GBA_LINK)),-DGBA_LINK) \
            $(if $(filter 1,$(ARM_FAR_STUBS)),-DARM_FAR_STUBS)
# -Wno-type-limits: psp/ui_psp.h compares an enum with 0, and an ARM EABI
# enum is unsigned.
# Online play (2.0): the HTTPS client (nethttps/) is built for the console's
# sceNet and threads (NP_TARGET_VITA); Mbed TLS comes from MBEDTLS=<prefix>,
# a private build of tools/online/build-mbedtls.py (python3
# tools/online/build-mbedtls.py --target vita --work DIR -> DIR/mbedtls/vita).
# Without the prefix everything compiles, the client has no TLS
# (np_https_available() = 0), and `make vpk` stops: a package without TLS
# would be an Online that cannot connect.
APP_DEFS += -DNP_TARGET_VITA
# MBEDTLS= may also come from the environment (MBEDTLS_VITA, or GATE_MBEDTLS_VITA, the
# gate's variable) so a shell can set it once.
MBEDTLS ?= $(or $(MBEDTLS_VITA),$(GATE_MBEDTLS_VITA))
ifneq ($(strip $(MBEDTLS)),)
APP_DEFS += -DNP_HAVE_MBEDTLS -I$(MBEDTLS)/include
MBEDTLS_LIBS := -L$(MBEDTLS)/lib -lmbedtls -lmbedx509 -lmbedcrypto
else ifneq ($(filter vpk $(VPK),$(MAKECMDGOALS)),)
$(error make vpk needs MBEDTLS=<prefix>: python3 tools/online/build-mbedtls.py --target vita --work DIR, then MBEDTLS=DIR/mbedtls/vita (or export MBEDTLS_VITA=that prefix); docs/BUILDING.md, Mbed TLS)
endif
APP_WARN := -Wall -Wextra -Wno-type-limits -std=gnu11
APP_OBJS := $(foreach s,$(APP_ALL_VITA),$(call app_obj,$(s)))
# newlib's FILE-slot allocation (__sfp) has no lock on the console: two threads
# in fopen() at once can be handed one FILE.  playerkit/vita/pk_stdio_lock.c is
# a real recursive lock the linker puts in place of the three no-ops
# (--wrap).  On by default; `make STDIO_LOCK=0` builds without it.
STDIO_LOCK ?= 1
ifeq ($(STDIO_LOCK),1)
STDIO_LOCK_OBJ := $(B)/app/pk_stdio_lock.o
STDIO_LOCK_LD  := -Wl,--wrap=__sfp -Wl,--wrap=__sfp_lock_acquire -Wl,--wrap=__sfp_lock_release
APP_OBJS += $(STDIO_LOCK_OBJ)
$(STDIO_LOCK_OBJ): ../../playerkit/vita/pk_stdio_lock.c
	@mkdir -p $(dir $@)
	$(CC) $(APP_FLAGS) -w -c -o $@ $<
endif
# What the app calls, each list after what uses it: the UI and the renderer
# (ui.mk), the transports (net.mk), then plat_vita.c's own.  arm-vita-eabi-
# gcc's specs add newlib's needs (SceLibKernel, SceIofilemgr, SceNet, ...).
# The HTTPS client's own objects use Mbed TLS structs, so they take the library's enum size (-fno-short-enums,
# gpsp/vita/deps/Makefile.mbedtls; arm-vita-eabi-gcc defaults to 1-byte enums, and mbedtls_ssl_config is then 4
# bytes shorter than the library's). Their public header (np_https.h) keeps every enum 4 bytes wide, so the rest of
# the app, built with the default, sees the same layout.
NET_ENUM_FLAGS := -fno-short-enums
$(B)/app/nethttps/np_https.o $(B)/app/nethttps/np_https_sys.o $(B)/app/nethttps/np_https_tls_mbedtls.o: \
  APP_FILE_FLAGS := $(NET_ENUM_FLAGS)
# The upscaler's per-pixel loops (Scale2x/3x, the picture's preparation):
# -O3 for this object only (a third fewer instructions in epx_rows on the
# host twin); the rest of the app stays -O2.
$(B)/app/vita/upscale_vita.o: APP_FILE_FLAGS := -O3 -ftree-vectorize

LIBS     := $(APP_LIBS_VITA) $(VITA_UI_LIBS) $(NET_VITA_LIBS) $(MBEDTLS_LIBS) \
            -lSceAppUtil_stub -lSceCommonDialog_stub -lSceDisplay_stub \
            -lSceCtrl_stub -lSceAudio_stub -lScePower_stub -lSceRtc_stub \
            -lSceMotion_stub -lSceCamera_stub \
            -lSceSysmem_stub -lSceLibKernel_stub -lstdc++ -lm
# A private build may carry its owner's own GBA BIOS dump, read by the app
# from app0:gba_bios.bin when ux0:data/GBAdhoc has none (main_vita.c
# app_bios_dir).  The file is never part of the repository:
#     make vpk BIOS=/path/to/gba_bios.bin
ifneq ($(strip $(BIOS)),)
VPK_EXTRA += $(BIOS)=gba_bios.bin
endif
DIST_NOTICES := $(wildcard ../../licenses/*) $(wildcard ../../notices/*)
VPK_EXTRA += $(foreach f,$(DIST_NOTICES),$(f)=$(patsubst ../../%,%,$(f)))
# ---- SNES (VitAdhoc 1.3) ---------------------------------------------------
# snes.bin is a SEPARATE PROGRAM (../../snes: Snes9x core, own MIT frontend)
# launched by VitAdhoc through plat_exec_program.  It is built by its own
# Makefile and only packed here; nothing of it is linked into eboot.bin
# (Snes9x's non-commercial licence is not GPL-compatible).  Its licence and
# provenance travel in the VPK.
SNES_BIN := ../../snes/vita/build/snes.bin
VPK_EXTRA += $(SNES_BIN)=snes.bin \
             ../../snes/core/LICENSE=licenses/Snes9x-LICENSE.txt \
             ../../snes/core/PROVENANCE.md=licenses/Snes9x-PROVENANCE.md
# The sub-make inherits this make's job slots (no -j1: snes/vita's rules are
# independent per object and the link waits for all of them).  It always runs,
# so a changed SNES source is seen; it touches snes.bin only when it relinks.
$(SNES_BIN): FORCE
	+$(MAKE) -C ../../snes/vita
# ---- Sega (VitAdhoc 2.0) ---------------------------------------------------
# md.bin is a SEPARATE PROGRAM (../../md: Genesis Plus GX core, own MIT frontend)
# for the Mega Drive / Genesis, Master System, Game Gear and SG-1000.  Built by
# its own Makefile (which builds the core object first) and only packed here;
# nothing of it is linked into eboot.bin (Genesis Plus GX's non-commercial
# licence is not GPL-compatible), and nothing of this app is linked into it
# (make -C ../../md/vita check-links).  Its licence and provenance travel in the VPK.
MD_BIN := ../../md/vita/build/md.bin
# (licenses/GenesisPlusGX-LICENSE.txt, a copy of md/core/LICENSE.txt, is already in DIST_NOTICES.)
VPK_EXTRA += $(MD_BIN)=md.bin \
             ../../md/core/PROVENANCE.md=licenses/GenesisPlusGX-PROVENANCE.md
$(MD_BIN): FORCE
	+$(MAKE) -C ../../md/vita
FORCE:
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
	$(call ccache_summary)

$(foreach s,$(APP_ALL_VITA),$(eval $(call APP_RULE,$(s))))

# -Wl,-q keeps the relocations vita-elf-create turns into the SCE module's.
# Order: B before A is irrelevant to the result (their symbols are disjoint);
# the archive last, so only what something references is pulled from it.
LINK_ELF = $(CC) -Wl,-q $(STDIO_LOCK_LD) -Wl,-Map,$(B)/gbadhoc.map -o $(ELF) $(APP_OBJS) \
	  $(VITA_UI_SHADERS) $(FE_OBJS) $(GB_OBJS) $(CORE_R) $(CORE_B) $(CORE_A) $(1) $(LIBS)

# A WEAK undefined symbol links without complaint and is a null call or a
# null address on the console (lessons 13 Nov: an archive member that was
# not pulled in worked on the host and was null on the Vita).  The link fails
# on EVERY weak undefined symbol that is not on this list.  The list is the
# known ones, each used only behind an address check (`if (&sym)`): the
# core's optional speed hooks and counters (instance A, and instance B under
# its gbab_ prefix), the translation-cache release B can do without, and the
# sanitizer's start hook of a non-ASan build, and the toolchain's own start-up
# code (crtbegin: transactional-memory and unwind-frame registration, libc's
# __libc_fini), which tests each address before calling.  A new name here needs the
# same reason, written next to it.
WEAK_OK := irq_io_fast timer_lazy jit_big_i_calls jit_big_i_max \
           jit_big_d_calls jit_pin_at \
           gbab_irq_io_fast gbab_timer_lazy gbab_jit_big_i_calls \
           gbab_jit_big_i_max gbab_jit_big_d_calls gbab_jit_pin_at \
           gbab_dynarec_release_translation_caches __asan_init \
           _ITM_deregisterTMCloneTable _ITM_registerTMCloneTable \
           __deregister_frame_info __register_frame_info __libc_fini
# (nm marks a weak undefined function `w` and a weak undefined object `v`;
# a version suffix such as @VERS is dropped before the comparison.)
CHECK_WEAK = @ok=" $(WEAK_OK) "; \
	w="$$($(NM) $(1) | awk '($$1 == "w" || $$1 == "v") && NF == 2 { print $$2 }' | sed 's/@.*//')"; \
	bad=""; for s in $$w; do case "$$ok" in *" $$s "*) ;; *) bad="$$bad $$s";; esac; done; \
	if [ -n "$$bad" ]; then echo "weak undefined in $(1), not on the allowlist (gpsp/vita/Makefile WEAK_OK):$$bad"; rm -f $(1); exit 1; fi

$(ELF): $(APP_OBJS) $(VITA_UI_SHADERS) $(FE_OBJS) $(GB_OBJS) $(CORE_R) $(CORE_B) $(CORE_A)
	$(call LINK_ELF,)
	$(call CHECK_WEAK,$(ELF))

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
	@dups="$$(printf '%s\n' $(foreach x,$(filter-out -a,$(VPK_FILES)),$(lastword $(subst =, ,$(x)))) | sort | uniq -d)"; \
	  if [ -n "$$dups" ]; then echo "duplicate archive member names in the package (VPK_EXTRA / VPK_FILES):"; echo "$$dups"; exit 1; fi
	@rm -f $@
	$(VITASDK)/bin/vita-pack-vpk -s $(SFO) -b $(SELF) $(VPK_FILES) $@

# The fast path for a change that is not code (new art, gamedb.txt, cheats.db, a
# notice, a rebuilt snes.bin): rewrite the last package replacing only the
# members whose content changed, the others copied as stored.  It compiles
# nothing of the app (run `make` first when eboot.bin should change) and
# builds snes.bin only through its own make (a no-op when it is current).
# The result has the same member list, order, modes and per-member contents
# as `make vpk`; tools/build/vpk_repack.py verifies that before it replaces
# the old file.  Releases are built with `make vpk` from a clean tree.
vpk-repack: $(SFO) $(SNES_BIN) $(MD_BIN)
	@test -f $(VPK) || { echo "vpk-repack: no $(VPK) yet: run make vpk once first"; exit 1; }
	@test -f $(SELF) || { echo "vpk-repack: no $(SELF): run make first"; exit 1; }
	python3 ../../tools/build/vpk_repack.py $(VPK) -s $(SFO) -b $(SELF) $(VPK_FILES)

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

include ../../tools/build/ccache-stats.mk
-include $(APP_OBJS:.o=.d)
.PHONY: all vpk vpk-repack assets report clean FORCE
