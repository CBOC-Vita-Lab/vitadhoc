# snes.bin: the SNES program of VitAdhoc (Snes9x core + our Vita frontend).
#   make -C snes/vita -j2                  builds build/snes.bin (any -j: the objects are independent, the link waits for all)
# Run by VitAdhoc via sceAppMgrLoadExec; packed into the VPK by gpsp/vita/Makefile.
# Copyright CBOC-Vita. MIT licence for this file; ../core keeps the Snes9x licence.
# The programs are separate. The one exception is the two-player link (netplay.cpp): the project's own UDP transport and NAT
# layer from gpsp/netdrv (netplay.mk, owner's code, MIT OR GPL-2.0-or-later) are compiled in beside playerkit's pk_link. None
# of the main app's Game Boy link (netdrv.c, fe_gblink.c, fe_evt.c, fe_util.c) is: the build checks it (CHECK_LICENCE).

VITASDK ?= /opt/vitasdk
export VITASDK
PREFIX := $(VITASDK)/bin/arm-vita-eabi-
# ccache (tools/build/ccache.mk): the prefix is part of CC/CXX; a no-op without ccache, USE_CCACHE=0 turns it off
include ../../tools/build/ccache.mk
CC  := $(call ccw,$(PREFIX)gcc)
CXX := $(call ccw,$(PREFIX)g++)
NM  := $(PREFIX)nm

CORE := ../core
FE   := ../frontend
V2D  := $(FE)/third_party/vita2d
B    := build

include $(CORE)/sources.mk
GP := ../../gpsp
include netplay.mk
empty :=
space := $(empty) $(empty)

CPUFLAGS := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
# S9X_PROF compiles the timing hooks in; they cost nothing while g_s9xp.on is 0
# (the app switches them on for one second in four to keep the log cheap).
CORE_DEFS := -DHAVE_STDINT_H -DNDEBUG -DS9X_PROF -DUSE_SELF_THREADED=0 -D__vita__ -DSA1_FAST_RUN=15
PK  := ../../playerkit
WK  := ../../widekit
INC := -I$(WK)/include -I$(CORE) -I$(CORE)/apu -I$(CORE)/apu/bapu -I$(FE) -I$(FE)/third_party/stb -I$(V2D)/include -I$(PK)/src -I$(PK)/vita
OPT ?= -O3
CXXFLAGS := -MMD -MP $(CPUFLAGS) $(OPT) -g -std=gnu++14 -w -fno-strict-aliasing -fno-rtti -fno-exceptions $(CORE_DEFS) $(INC)
CFLAGS   := -MMD -MP $(CPUFLAGS) -O2 -g -w -DVITA2D_NO_PGF -D__vita__ -I$(V2D)/include
# RetroAchievements (playerkit/ra.mk): pk_ra, the rcheevos client and ra_http.c: MIT code only (check-ra says so)
RA_GP     := ../../gpsp/vita
RA_OBJDIR := $(B)/ra
RA_CC     = $(CC)
RA_CFLAGS = -MMD -MP $(CPUFLAGS) -O2 -g -w -D__vita__
include $(PK)/ra.mk
.DEFAULT_GOAL := all

CORE_OBJS := $(addprefix $(B)/core/,$(CORE_SRCS:.cpp=.o))
# Second, renamed copy of the renderer for the PPU thread (see ../core/rcopy.sh)
RCOPY_SRC := gfx tile tileimpl-h2x1 tileimpl-n1x1 tileimpl-n2x1
RCOPY_OBJS := $(addprefix $(B)/rcopy/,$(addsuffix .o,$(RCOPY_SRC)))
# Third copy (suffix _w, state *_W): the wide view's margin thread, see ../core/ppu_mt.cpp
RCOPY_OBJS += $(addprefix $(B)/wcopy/,$(addsuffix .o,$(RCOPY_SRC)))
FE_OBJS   := $(B)/host.o $(B)/prof.o $(B)/audio_pace.o $(B)/picture.o $(B)/wide.o $(B)/player.o $(B)/pipe.o $(B)/netplay.o $(B)/main.o
# the two-player link's C files from gpsp/ (netplay.mk), built as the main app builds them (-DVITA: sceKernel threads, netctl),
# (no netdrv header: PK_PLAYER, playerkit/src/pk_lan_compat.h)
NP_OBJS   := $(patsubst $(GP)/%.c,$(B)/gp/%.o,$(NETPLAY_GP_SRC))
# widekit (MIT, ../../widekit): the wide view's layer analysis, learned map and composition
WK_SRC    := $(notdir $(basename $(wildcard $(WK)/src/*.cpp)))
WK_OBJS   := $(addprefix $(B)/wk/,$(addsuffix .o,$(WK_SRC)))
# playerkit (MIT): the in-game menu, states, input mapping, launch protocol, patches, notices, pacing
PK_SRC    := pk_util pk_i18n pk_text pk_glyphs pk_font_data pk_icons pk_states pk_launch pk_patch pk_input pk_notice pk_menu pk_pace pk_handoff pk_cores pk_look pk_scale pk_rewind pk_ff pk_speed pk_link pk_link_net pk_ra
PK_OBJS   := $(addprefix $(B)/pk/,$(addsuffix .o,$(PK_SRC))) $(B)/pk/pk_gfx_vita.o $(B)/pk/pk_ra_plat_vita.o $(RA_OBJS)
# newlib's FILE-slot allocation has no lock on the console (playerkit/vita/pk_stdio_lock.c puts a real
# one in place with --wrap); `make STDIO_LOCK=0` builds without it, as the main app's Makefile does too
STDIO_LOCK ?= 1
ifeq ($(STDIO_LOCK),1)
PK_OBJS += $(B)/pk/pk_stdio_lock.o
STDIO_LOCK_LD := -Wl,--wrap=__sfp -Wl,--wrap=__sfp_lock_acquire -Wl,--wrap=__sfp_lock_release
endif
V2D_OBJS  := $(B)/v2d/vita2d.o $(B)/v2d/vita2d_draw.o $(B)/v2d/vita2d_texture.o $(B)/v2d/utils.o
SHADERS   := $(wildcard $(V2D)/shader/*_gxp.o)

LIBS := -lSceNet_stub -lSceNetCtl_stub -lSceShaccCg_stub_weak -lSceGxm_stub -lSceDisplay_stub -lSceSysmodule_stub \
        -lSceCommonDialog_stub -lSceAppMgr_stub -lSceCtrl_stub -lSceTouch_stub \
        -lSceAppUtil_stub -lSceAudio_stub -lScePower_stub -lSceRtc_stub \
        -lSceHttp_stub -lSceSsl_stub -lSceSysmem_stub -lSceLibKernel_stub -lm

all: $(B)/snes.bin

# A WEAK undefined symbol links without complaint and is a null call on the console (gpsp/vita/Makefile has the same
# check): the link fails on any weak undefined symbol that is not listed here. The list is what the toolchain's runtime
# leaves weak on purpose, and the pthread_* of libstdc++'s thread layer. With those unresolved libstdc++ believes the
# program is single threaded, so std::mutex, std::call_once, shared_ptr counts and the guards of function-local statics
# do NOT lock. Audited (13 Nov): snes/, playerkit/, widekit/ use none of std::mutex, std::thread, condition_variable,
# shared_ptr, call_once; strings are the C++11 ABI (no shared refcount); the function-local statics with constructors
# (host.cpp mix_job's vector, pk_util.cpp fs(), pk_input.cpp models) are first used on one thread before another can
# reach them. With the gthreads inactive libstdc++'s exception globals are process-wide, so nothing may throw on two threads at once. Sharing between threads is done with pk::SpinLock and atomics. A new use of any of those needs a real lock.
WEAK_OK := ^(_ITM_|_ZGT|__cxa_pure_virtual$$|__deregister_frame_info$$|__register_frame_info$$|__gnu_Unwind_Find_exidx$$|__libc_fini$$|pthread_(cancel|cond_broadcast|cond_wait|getspecific|key_create|key_delete|mutex_destroy|mutex_init|mutex_lock|mutex_unlock|once|setspecific)$$)
CHECK_WEAK = @w="$$($(NM) $(1) | awk '$$1 == "w" || $$1 == "v" { print $$2 }' | grep -v -E '$(WEAK_OK)')"; \
	if [ -n "$$w" ]; then echo "weak undefined in $(1), not on the allowlist (snes/vita/Makefile WEAK_OK): $$w"; rm -f $(1); exit 1; fi

SNES_OBJS := $(CORE_OBJS) $(RCOPY_OBJS) $(FE_OBJS) $(NP_OBJS) $(WK_OBJS) $(PK_OBJS) $(V2D_OBJS) $(SHADERS)
LINK_ELF = $(CXX) $(CPUFLAGS) -Wl,-q -Wl,-Map=$(B)/snes.map $(STDIO_LOCK_LD) -o $(B)/snes.elf $(SNES_OBJS) $(1) $(LIBS)

# LICENCE CHECK: the player must not link the main app's Game Boy link (third-party GPL-2.0 from the GBAdhoc project):
# not in the object list, not in the linker's map, and none of its symbols defined in the program. The build fails if one is.
FORBID_RE := $(subst .,\.,$(subst $(space),|,$(strip $(NETPLAY_FORBIDDEN))))
FORBID_HDR_RE := $(subst .,\.,$(subst $(space),|,$(strip $(NETPLAY_FORBIDDEN_HDR))))
FORBID_SYM := ^(netdrv_|fe_gblink_|fe_evt|fe_crc32|fe_util_|fe_padpair_|gbdual_|gbcore_|gblink_)
CHECK_LICENCE = @bad="$$( { for o in $(SNES_OBJS); do basename $$o; done | grep -E '^($(FORBID_RE))$$'; \
	grep -E '[/( ]($(FORBID_RE))' $(B)/snes.map; \
	$(NM) $(1) | awk '$$2 ~ /^[TtDdBbRrWw]$$/ { print $$3 }' | grep -E '$(FORBID_SYM)'; } )"; \
	if [ -n "$$bad" ]; then echo "LICENCE: snes.bin links a file of the main app's Game Boy link (GBAdhoc, GPL-2.0): $$bad"; rm -f $(1); exit 1; fi; \
	hdr="$$( for o in $(SNES_OBJS); do d="$${o%.o}.d"; [ -f "$$d" ] && cat "$$d"; done 2>/dev/null | tr ' \\' '\n\n' | grep -E '(^|/)($(FORBID_HDR_RE))$$' | sort -u)"; \
	if [ -n "$$hdr" ]; then echo "LICENCE: snes.bin was compiled against a header of the main app's Game Boy link driver (GBAdhoc, GPL-2.0): $$hdr"; rm -f $(1); exit 1; fi; \
	echo "check-licence: none of $(NETPLAY_FORBIDDEN) in the object list, the link map or the symbol table, none of $(NETPLAY_FORBIDDEN_HDR) in any compile's dependency file (the link is pk_link + transport_lan + nat_*)"

# RetroAchievements: of everything from gpsp/ in the program only the files of RA_SRCS (ra.mk) may be there, and each is MIT
# (ra_http.c and ra_roots.h are the owner's, dual MIT OR GPL-2.0-or-later; the rcheevos client is MIT; ra_vita.c is not linked).
check-ra:
	@bad=""; for f in $(SNES_OBJS); do case "$$f" in */ra/*|build/ra/*) n=$$(basename $$f .o); ok=0; for r in $(RA_NAMES:.c=); do [ "$$r" = "$$n" ] && ok=1; done; [ $$ok = 1 ] || bad="$$bad $$f";; esac; done; \
	if [ -n "$$bad" ]; then echo "snes.bin links an unlisted RetroAchievements object:$$bad"; exit 1; fi; \
	if grep -E '[/( ](ra_vita|fe_evt|fe_util|netdrv|fe_gblink)\.o' $(B)/snes.map >/dev/null 2>&1; then echo "snes.bin's link map names a GPL file of the app"; exit 1; fi; \
	for f in $(RA_MIT_FILES); do grep -q 'MIT' $$f || { echo "RetroAchievements file without an MIT licence: $$f"; exit 1; }; done; \
	if grep -l 'GPL-2.0-only\|GNU General Public' $(RA_SRCS) $(PK)/src/pk_ra.cpp $(PK)/src/pk_ra.h >/dev/null 2>&1; then echo "a GPL-only file is in the RetroAchievements list"; exit 1; fi; \
	echo "check-ra: $(words $(RA_SRCS)) MIT files from gpsp/ for RetroAchievements (ra_http.c dual MIT OR GPL-2.0-or-later taken as MIT, the rcheevos client), ra_vita.c not linked, link map clean"

$(B)/snes.elf: $(CORE_OBJS) $(RCOPY_OBJS) $(FE_OBJS) $(NP_OBJS) $(WK_OBJS) $(PK_OBJS) $(V2D_OBJS) $(SHADERS)
	$(call LINK_ELF,)
	$(call CHECK_WEAK,$@)
	$(call CHECK_LICENCE,$@)
	@$(MAKE) --no-print-directory check-ra SNES_OBJS="$(SNES_OBJS)"

# ROOM FOR THE SCE DATA (same cure as gpsp/vita/Makefile $(VELF)): vita-elf-create writes its import
# tables (a few KiB) behind the last byte of segment 0 and gives up ("Cannot allocate N bytes for SCE
# data at end of segment 0; segment 1 overlaps", then a crash) when segment 1, which the linker starts
# on the next 64 KiB boundary, is nearer than that.  Whether it is depends on the last 16 bits of the
# program's size, so ANY change anywhere can trigger it.  Then the program is linked once more with
# 8 KiB of constants, which carries the end of segment 0 over the boundary.
$(B)/elfpad.o:
	@mkdir -p $(B)
	echo 'const char snes_elf_pad[8192] = { 1 };' | $(CC) -x c -c -o $@ -

$(B)/snes.velf: $(B)/snes.elf $(B)/elfpad.o
	$(VITASDK)/bin/vita-elf-create $< $@ > $(B)/elf-create.log 2>&1 || \
	  { grep -q 'segment 1 overlaps' $(B)/elf-create.log && \
	    echo "  no room for the SCE data behind segment 0 ($$(grep -o "Cannot allocate [0-9]* bytes" $(B)/elf-create.log); text ends at $$($(PREFIX)readelf -lW $< | awk '$$1 == "LOAD" && n++ == 0 { printf "%x", $$3 + $$6 }'), next segment at 64 KiB boundary): linking again with 8 KiB of padding" && \
	    $(call LINK_ELF,$(B)/elfpad.o) && \
	    $(VITASDK)/bin/vita-elf-create $< $@; } || { cat $(B)/elf-create.log; exit 1; }

$(B)/snes.bin: $(B)/snes.velf
	$(VITASDK)/bin/vita-make-fself -s -c $< $@

$(B)/rcopy.map: $(addprefix $(B)/core/,$(addsuffix .o,$(RCOPY_SRC))) $(CORE)/rcopy.sh
	NM=$(PREFIX)nm sh $(CORE)/rcopy.sh $@ $(filter %.o,$^)
$(B)/rcopy/%.o: $(B)/core/%.o $(B)/rcopy.map
	@mkdir -p $(B)/rcopy
	$(PREFIX)objcopy --redefine-syms=$(B)/rcopy.map $< $@
$(B)/wcopy.map: $(addprefix $(B)/core/,$(addsuffix .o,$(RCOPY_SRC))) $(CORE)/rcopy.sh
	NM=$(PREFIX)nm RCOPY_SUF=w sh $(CORE)/rcopy.sh $@ $(filter %.o,$^)
$(B)/wcopy/%.o: $(B)/core/%.o $(B)/wcopy.map
	@mkdir -p $(B)/wcopy
	$(PREFIX)objcopy --redefine-syms=$(B)/wcopy.map $< $@

# Super FX: the exact speed-up of agent P5 (limit and fast paths in the GSU interpreter)
$(B)/core/fxinst.o $(B)/core/fxemu.o $(B)/core/fxfast.o: CXXFLAGS += -DFX_LIMIT -DFX_FAST -DFX_LINEFLAGS
$(B)/core/%.o: $(CORE)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/%.o: $(FE)/%.cpp
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/netplay.o: netplay.cpp netplay.h $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)
	$(CXX) -MMD -MP $(CXXFLAGS) $(NETPLAY_INC) $(NETPLAY_ND_DEFS) -c -o $@ $<
$(B)/gp/%.o: $(GP)/%.c
	@mkdir -p $(dir $@)
	$(CC) -MMD -MP $(CPUFLAGS) -O2 -g -Wall -Wextra -DVITA $(NETPLAY_INC) $(NETPLAY_ND_DEFS) -c -o $@ $<
$(B)/pk/pk_link_net.o: $(PK)/src/pk_link_net.cpp $(wildcard $(PK)/src/*.h)
	@mkdir -p $(dir $@)
	$(CXX) -MMD -MP $(CXXFLAGS) $(NETPLAY_INC) $(NETPLAY_ND_DEFS) -c -o $@ $<
$(B)/main.o: main.cpp $(FE)/host.h player.h pipe.h netplay.h
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/pipe.o: pipe.cpp pipe.h $(FE)/host.h $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/player.o: player.cpp player.h $(FE)/host.h $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/wk/%.o: $(WK)/src/%.cpp $(wildcard $(WK)/include/*.h) $(wildcard $(WK)/src/*.h)
	@mkdir -p $(B)/wk
	$(CXX) $(CPUFLAGS) $(OPT) -g -std=gnu++11 -fno-exceptions -fno-rtti -I$(WK)/include -c -o $@ $<
$(B)/pk/pk_ra.o: PK_EXTRA_INC := $(RA_INC)
$(B)/pk/%.o: $(PK)/src/%.cpp $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pk
	$(CXX) $(CXXFLAGS) $(PK_EXTRA_INC) -c -o $@ $<
$(B)/pk/pk_ra_plat_vita.o: $(PK)/vita/pk_ra_plat_vita.c $(PK)/src/pk_ra_plat.h
	@mkdir -p $(B)/pk
	$(CC) $(CFLAGS) -I$(PK)/src -c -o $@ $<
$(B)/pk/pk_gfx_vita.o: $(PK)/vita/pk_gfx_vita.cpp $(wildcard $(PK)/vita/*.h) $(PK)/src/pk_gfx.h
	@mkdir -p $(B)/pk
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/pk/pk_stdio_lock.o: $(PK)/vita/pk_stdio_lock.c
	@mkdir -p $(B)/pk
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/v2d/%.o: $(V2D)/source/%.c
	@mkdir -p $(B)/v2d
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(shell find $(B) -name '*.d' 2>/dev/null)

clean:
	rm -rf $(B)
.PHONY: all clean check-ra
