# Linux twin of the Sega player core: headless runner (main.cpp) linked with the relocatable object mdcore.o
# (md/core/Makefile: Genesis Plus GX + md/frontend/host.c, only md_host_* exported).
#   make -C md/twin                  x86_64 host   -> build/x86/md_twin
#   make -C md/twin ARCH=armhf       armhf, static -> build/armhf/md_twin   (run with qemu-arm)
#   make -C md/twin ARCH=x86 PROFILE=1   same with frame pointers, for callgrind
# Copyright CBOC-Vita. MIT licence for this Makefile and main.cpp.
ARCH ?= x86
B    ?= build/$(ARCH)
BA   := $(abspath $(B))
FE   := ../frontend
ifeq ($(ARCH),armhf)
CXX  := arm-linux-gnueabihf-g++
TARGET := armhf
CPUF := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -static
else
CXX  ?= g++
TARGET := host
CPUF :=
endif
# ccache (tools/build/ccache.mk): prefix inside CC/CXX, a no-op without ccache; USE_CCACHE=0 turns it off.
include ../../tools/build/ccache.mk
override CXX := $(call ccw,$(CXX))
all: $(B)/md_twin $(B)/player_twin $(B)/scale_proof $(B)/sidebyside $(B)/wide_pipe_proof
$(B)/mdcore.o: FORCE
	$(MAKE) -C ../core TARGET=$(TARGET) B=$(BA) PROFILE=$(PROFILE) all
# the wide view (--wide): the adapter ../frontend/wide.cpp over widekit (MIT) and its PNG writer
WK       := ../../widekit
WK_FILES := $(WK)/src/wk_layers.cpp $(WK)/src/wk_map.cpp $(WK)/src/wk_compose.cpp $(WK)/src/wk_profile.cpp $(WK)/src/wk_util.cpp
$(B)/md_twin: main.cpp $(B)/mdcore.o $(FE)/host.h $(FE)/wide.h $(FE)/wide.cpp $(WK_FILES) $(wildcard $(WK)/include/*.h)
	$(CXX) -O2 -g -std=gnu++14 -Wall -Wextra $(CPUF) -I$(FE) -I../core -I$(WK)/include -I$(WK)/src -I$(WK)/tests -o $@ main.cpp $(FE)/wide.cpp $(WK_FILES) $(WK)/tests/png.cpp $(B)/mdcore.o -lm -pthread

$(B)/scale_proof: scale_proof.cpp $(B)/mdcore.o $(B)/pk/pk_scale.o $(FE)/host.h $(FE)/wide.h $(FE)/wide.cpp $(WK_FILES)
	$(CXX) -O2 -g -std=gnu++14 -Wall -Wextra $(CPUF) -I$(FE) -I../../playerkit/src -I$(WK)/include -I$(WK)/src -o $@ scale_proof.cpp $(FE)/wide.cpp $(WK_FILES) $(B)/mdcore.o $(B)/pk/pk_scale.o -lm
# wide_pipe_proof: the wide view through the player's picture pipe (../vita/pipe.cpp) on a second thread, bit for bit against composing at once
$(B)/wide_pipe_proof: wide_pipe_proof.cpp ../vita/pipe.cpp ../vita/pipe.h $(B)/pk/pk_handoff.o $(B)/mdcore.o $(FE)/host.h $(FE)/wide.h $(FE)/wide.cpp $(WK_FILES)
	$(CXX) -O2 -g -std=gnu++14 -Wall -Wextra $(CPUF) -I$(FE) -I../vita -I../../playerkit/src -I$(WK)/include -I$(WK)/src -o $@ wide_pipe_proof.cpp ../vita/pipe.cpp $(FE)/wide.cpp $(WK_FILES) $(B)/pk/pk_handoff.o $(B)/mdcore.o -lm -pthread
$(B)/sidebyside: sidebyside.cpp $(B)/mdcore.o $(B)/pk/pk_scale.o $(FE)/host.h
	$(CXX) -O2 -g -std=gnu++14 -Wall -Wextra $(CPUF) -I$(FE) -I../../playerkit/src -o $@ sidebyside.cpp $(B)/mdcore.o $(B)/pk/pk_scale.o -lm
# player_twin: the Sega player's own logic (../vita/player.cpp) over the core, with playerkit's software renderer (no gpsp/ code)
PK       := ../../playerkit
# RetroAchievements (playerkit/ra.mk): pk_ra, the rcheevos client and ra_http.c, the thread layer of the twin
ifeq ($(ARCH),armhf)
RA_CC    := $(call ccw,arm-linux-gnueabihf-gcc)
else
RA_CC    := $(call ccw,gcc)
endif
RA_GP     := ../../gpsp/vita
RA_OBJDIR := $(B)/ra
RA_CFLAGS := -std=gnu11 -O2 -g -w $(CPUF)
include $(PK)/ra.mk
.DEFAULT_GOAL := all
$(B)/ra/pk_ra_plat_posix.o: $(PK)/ra/pk_ra_plat_posix.c $(PK)/src/pk_ra_plat.h
	@mkdir -p $(B)/ra
	$(RA_CC) $(RA_CFLAGS) -I$(PK)/src -c -o $@ $<
$(B)/pk/pk_ra.o: PK_EXTRA_INC := $(RA_INC)
PK_SRC   := pk_util pk_text pk_font_data pk_icons pk_gfx_soft pk_states pk_launch pk_patch pk_input pk_notice pk_menu pk_pace pk_handoff pk_look pk_cores pk_scale pk_rewind pk_ff pk_speed pk_ra
PK_OBJS  := $(addprefix $(B)/pk/,$(addsuffix .o,$(PK_SRC))) $(B)/ra/pk_ra_plat_posix.o $(RA_OBJS)
PL_FE    := picture cheats savefile audio_pace
PL_OBJS  := $(addprefix $(B)/pl/,$(addsuffix .o,$(PL_FE))) $(B)/pl/player.o $(B)/pl/pipe.o $(B)/pl/handoff_proof.o $(B)/pl/player_twin.o
$(B)/pk/%.o: $(PK)/src/%.cpp $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pk
	$(CXX) -O2 -std=gnu++14 -Wall $(CPUF) -I$(PK)/src $(PK_EXTRA_INC) -c -o $@ $<
$(B)/pl/%.o: $(FE)/%.cpp $(wildcard $(FE)/*.h) $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pl
	$(CXX) -O2 -std=gnu++14 -Wall -Wextra $(CPUF) -I$(FE) -I$(PK)/src -c -o $@ $<
$(B)/pl/%.o: ../vita/%.cpp $(wildcard ../vita/*.h) $(wildcard $(FE)/*.h) $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pl
	$(CXX) -O2 -std=gnu++14 -Wall -Wextra $(CPUF) -I$(FE) -I../vita -I$(PK)/src -c -o $@ $<
$(B)/pl/%.o: %.cpp $(wildcard ../vita/*.h) $(wildcard $(FE)/*.h) $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pl
	$(CXX) -O2 -std=gnu++14 -Wall $(CPUF) -I$(FE) -I../vita -I$(PK)/src -I$(PK)/tests -c -o $@ $<
$(B)/player_twin: $(PL_OBJS) $(PK_OBJS) $(B)/mdcore.o
	$(CXX) $(CPUF) -o $@ $^ -lm -pthread
player_twin: $(B)/player_twin
FORCE:
clean:
	rm -rf build
.PHONY: all clean FORCE
