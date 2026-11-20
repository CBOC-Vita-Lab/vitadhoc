# md.bin: the Sega program of VitAdhoc (Genesis Plus GX core + our Vita frontend): Mega Drive / Genesis, Master System, Game Gear, SG-1000.
#   make -C md/vita -j2                    builds build/md.bin (the core object first: make -C md/core vita, done by this Makefile)
#   make -C md/vita check-links            fails when anything but md/, playerkit/, widekit/ and the vendored libvita2d is linked
# Run by VitAdhoc via sceAppMgrLoadExec; packed into the VPK by gpsp/vita/Makefile.
# Copyright CBOC-Vita. MIT licence for this file; ../core keeps the Genesis Plus GX licence (non-commercial: nothing from gpsp/ may be linked).

VITASDK ?= /opt/vitasdk
export VITASDK
PREFIX := $(VITASDK)/bin/arm-vita-eabi-
include ../../tools/build/ccache.mk
CC  := $(call ccw,$(PREFIX)gcc)
CXX := $(call ccw,$(PREFIX)g++)
NM  := $(PREFIX)nm

CORE := ../core
FE   := ../frontend
# libvita2d (ISC/MIT, vendored once for the players: its own PROVENANCE file). The only code from outside md/ and playerkit/ that md.bin links.
V2D  := ../../snes/frontend/third_party/vita2d
STB  := ../../snes/frontend/third_party/stb
PK   := ../../playerkit
B    := build
BA   := $(abspath $(B))

CPUFLAGS := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
INC := -I$(FE) -I. -I$(STB) -I$(V2D)/include -I$(PK)/src -I$(PK)/vita
OPT ?= -O3
CXXFLAGS := -MMD -MP $(CPUFLAGS) $(OPT) -g -std=gnu++14 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing -fno-rtti -D__vita__ $(INC)
CFLAGS   := -MMD -MP $(CPUFLAGS) -O2 -g -w -DVITA2D_NO_PGF -D__vita__ -I$(V2D)/include

FE_OBJS := $(B)/fe/picture.o $(B)/fe/audio_pace.o $(B)/fe/cheats.o $(B)/fe/savefile.o $(B)/player.o $(B)/pipe.o $(B)/main.o
PK_SRC  := pk_util pk_text pk_font_data pk_icons pk_states pk_launch pk_patch pk_input pk_notice pk_menu pk_pace pk_handoff pk_cores pk_look pk_scale pk_rewind pk_ff pk_speed
PK_OBJS := $(addprefix $(B)/pk/,$(addsuffix .o,$(PK_SRC))) $(B)/pk/pk_gfx_vita.o
# newlib's FILE-slot allocation has no lock on the console (playerkit/vita/pk_stdio_lock.c puts a real one in place with --wrap)
STDIO_LOCK ?= 1
ifeq ($(STDIO_LOCK),1)
PK_OBJS += $(B)/pk/pk_stdio_lock.o
STDIO_LOCK_LD := -Wl,--wrap=__sfp -Wl,--wrap=__sfp_lock_acquire -Wl,--wrap=__sfp_lock_release
endif
V2D_OBJS := $(B)/v2d/vita2d.o $(B)/v2d/vita2d_draw.o $(B)/v2d/vita2d_texture.o $(B)/v2d/utils.o
SHADERS  := $(wildcard $(V2D)/shader/*_gxp.o)

LIBS := -lSceShaccCg_stub_weak -lSceGxm_stub -lSceDisplay_stub -lSceSysmodule_stub \
        -lSceCommonDialog_stub -lSceAppMgr_stub -lSceCtrl_stub -lSceTouch_stub \
        -lSceAppUtil_stub -lSceAudio_stub -lScePower_stub -lSceRtc_stub \
        -lSceSysmem_stub -lSceLibKernel_stub -lm

all: $(B)/md.bin

# A WEAK undefined symbol links without complaint and is a null call on the console: the link fails on any weak undefined symbol that is not
# listed here (the same list as snes/vita/Makefile and gpsp/vita/Makefile: what the toolchain's runtime leaves weak on purpose, and the pthread_*
# of libstdc++'s thread layer). With those unresolved libstdc++ believes the program is single threaded, so std::mutex, std::call_once, shared_ptr
# counts and the guards of function-local statics do NOT lock. Audited: md/, playerkit/ use none of std::mutex, std::thread, condition_variable,
# shared_ptr, call_once. Sharing between threads is done with pk::SpinLock, atomics and vita semaphores (main.cpp); the function-local statics with
# constructors (pk_util fs(), pk_input models, pic_lcd_table) are first used on one thread before another can reach them. Nothing may throw on two
# threads at once. A new use of any of those needs a real lock.
WEAK_OK := ^(_ITM_|_ZGT|__cxa_pure_virtual$$|__deregister_frame_info$$|__register_frame_info$$|__gnu_Unwind_Find_exidx$$|__libc_fini$$|pthread_(cancel|cond_broadcast|cond_wait|getspecific|key_create|key_delete|mutex_destroy|mutex_init|mutex_lock|mutex_unlock|once|setspecific)$$)
CHECK_WEAK = @w="$$($(NM) $(1) | awk '$$1 == "w" || $$1 == "v" { print $$2 }' | grep -v -E '$(WEAK_OK)')"; \
	if [ -n "$$w" ]; then echo "weak undefined in $(1), not on the allowlist (md/vita/Makefile WEAK_OK): $$w"; rm -f $(1); exit 1; fi

# the relocatable core object: Genesis Plus GX + host.c, only md_host_* exported (md/core/Makefile)
$(B)/mdcore.o: FORCE
	@mkdir -p $(B)
	$(MAKE) -C $(CORE) TARGET=vita B=$(BA) all
FORCE:

MD_OBJS := $(B)/mdcore.o $(FE_OBJS) $(PK_OBJS) $(V2D_OBJS) $(SHADERS)
LINK_ELF = $(CXX) $(CPUFLAGS) -Wl,-q $(STDIO_LOCK_LD) -o $(B)/md.elf $(MD_OBJS) $(1) $(LIBS)
$(B)/md.elf: $(MD_OBJS)
	$(call LINK_ELF,)
	$(call CHECK_WEAK,$@)
	@$(MAKE) --no-print-directory check-links LINKED="$^"

# ROOM FOR THE SCE DATA (see snes/vita/Makefile, gpsp/vita/Makefile): when vita-elf-create reports
# "segment 1 overlaps" (the text ends within ~3.3 KB below a 64 KiB boundary: any change can cause it)
# the program is linked again with 8 KiB of constants.
$(B)/elfpad.o:
	@mkdir -p $(B)
	echo 'const char md_elf_pad[8192] = { 1 };' | $(CC) -x c -c -o $@ -

$(B)/md.velf: $(B)/md.elf $(B)/elfpad.o
	$(VITASDK)/bin/vita-elf-create $< $@ > $(B)/elf-create.log 2>&1 || \
	  { grep -q 'segment 1 overlaps' $(B)/elf-create.log && \
	    echo "  no room for the SCE data behind segment 0 ($$(grep -o 'Cannot allocate [0-9]* bytes' $(B)/elf-create.log)): linking again with 8 KiB of padding" && \
	    $(call LINK_ELF,$(B)/elfpad.o) && \
	    $(VITASDK)/bin/vita-elf-create $< $@; } || { cat $(B)/elf-create.log; exit 1; }

$(B)/md.bin: $(B)/md.velf
	$(VITASDK)/bin/vita-make-fself -s -c $< $@

# Provenance of what is linked: every object comes from md/, playerkit/ or the vendored libvita2d; nothing from gpsp/ (GPL code next to a
# non-commercial core) and no GPL file of playerkit's tests. Run on the object list of the link (the build path is under md/vita/build).
LINKED ?= $(B)/mdcore.o $(FE_OBJS) $(PK_OBJS) $(V2D_OBJS)
check-links:
	@bad=""; for f in $(LINKED); do case "$$f" in *third_party/vita2d/*) ;; *gpsp*|*xbrz*|*xBRZ*|*../snes/*|*../nes/*) bad="$$bad $$f";; esac; done; \
	if [ -n "$$bad" ]; then echo "md.bin links code it must not:$$bad"; exit 1; fi; \
	for f in $(PK_SRC); do [ -f $(PK)/src/$$f.cpp ] || { echo "missing $$f.cpp"; exit 1; }; done; \
	echo "check-links: $(words $(LINKED)) objects, none from gpsp/ or xBRZ; sources are md/, playerkit/ and the vendored libvita2d"

$(B)/fe/%.o: $(FE)/%.cpp $(wildcard $(FE)/*.h) $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/fe
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/%.o: %.cpp $(wildcard *.h) $(wildcard $(FE)/*.h) $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)
	$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/pk/%.o: $(PK)/src/%.cpp $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pk
	$(CXX) $(CPUFLAGS) $(OPT) -g -std=gnu++14 -w -fno-strict-aliasing -fno-rtti -D__vita__ -I$(PK)/src -I$(PK)/vita -c -o $@ $<
$(B)/pk/pk_gfx_vita.o: $(PK)/vita/pk_gfx_vita.cpp $(wildcard $(PK)/vita/*.h) $(PK)/src/pk_gfx.h
	@mkdir -p $(B)/pk
	$(CXX) $(CPUFLAGS) $(OPT) -g -std=gnu++14 -w -fno-strict-aliasing -fno-rtti -D__vita__ -I$(PK)/src -I$(PK)/vita -I$(V2D)/include -c -o $@ $<
$(B)/pk/pk_stdio_lock.o: $(PK)/vita/pk_stdio_lock.c
	@mkdir -p $(B)/pk
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/v2d/%.o: $(V2D)/source/%.c
	@mkdir -p $(B)/v2d
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(shell find $(B) -name '*.d' 2>/dev/null)

clean:
	rm -rf $(B)
.PHONY: all clean check-links FORCE
