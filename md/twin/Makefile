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
all: $(B)/md_twin $(B)/player_twin $(B)/scale_proof $(B)/sidebyside
$(B)/mdcore.o: FORCE
	$(MAKE) -C ../core TARGET=$(TARGET) B=$(BA) PROFILE=$(PROFILE) all
$(B)/md_twin: main.cpp $(B)/mdcore.o $(FE)/host.h
	$(CXX) -O2 -g -std=gnu++14 -Wall -Wextra $(CPUF) -I$(FE) -o $@ main.cpp $(B)/mdcore.o -lm

$(B)/scale_proof: scale_proof.cpp $(B)/mdcore.o $(B)/pk/pk_scale.o $(FE)/host.h
	$(CXX) -O2 -g -std=gnu++14 -Wall -Wextra $(CPUF) -I$(FE) -I../../playerkit/src -o $@ scale_proof.cpp $(B)/mdcore.o $(B)/pk/pk_scale.o -lm
$(B)/sidebyside: sidebyside.cpp $(B)/mdcore.o $(B)/pk/pk_scale.o $(FE)/host.h
	$(CXX) -O2 -g -std=gnu++14 -Wall -Wextra $(CPUF) -I$(FE) -I../../playerkit/src -o $@ sidebyside.cpp $(B)/mdcore.o $(B)/pk/pk_scale.o -lm
# player_twin: the Sega player's own logic (../vita/player.cpp) over the core, with playerkit's software renderer (no gpsp/ code)
PK       := ../../playerkit
PK_SRC   := pk_util pk_text pk_font_data pk_icons pk_gfx_soft pk_states pk_launch pk_patch pk_input pk_notice pk_menu pk_pace pk_handoff pk_look pk_cores pk_scale pk_rewind pk_ff pk_speed
PK_OBJS  := $(addprefix $(B)/pk/,$(addsuffix .o,$(PK_SRC)))
PL_FE    := picture cheats savefile audio_pace
PL_OBJS  := $(addprefix $(B)/pl/,$(addsuffix .o,$(PL_FE))) $(B)/pl/player.o $(B)/pl/pipe.o $(B)/pl/handoff_proof.o $(B)/pl/player_twin.o
$(B)/pk/%.o: $(PK)/src/%.cpp $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pk
	$(CXX) -O2 -std=gnu++14 -Wall $(CPUF) -I$(PK)/src -c -o $@ $<
$(B)/pl/%.o: $(FE)/%.cpp $(wildcard $(FE)/*.h) $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pl
	$(CXX) -O2 -std=gnu++14 -Wall -Wextra $(CPUF) -I$(FE) -I$(PK)/src -c -o $@ $<
$(B)/pl/%.o: ../vita/%.cpp $(wildcard ../vita/*.h) $(wildcard $(FE)/*.h) $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pl
	$(CXX) -O2 -std=gnu++14 -Wall -Wextra $(CPUF) -I$(FE) -I../vita -I$(PK)/src -c -o $@ $<
$(B)/pl/%.o: %.cpp $(wildcard ../vita/*.h) $(wildcard $(FE)/*.h) $(wildcard $(PK)/src/*.h)
	@mkdir -p $(B)/pl
	$(CXX) -O2 -std=gnu++14 -Wall $(CPUF) -I$(FE) -I../vita -I$(PK)/src -c -o $@ $<
$(B)/player_twin: $(PL_OBJS) $(PK_OBJS) $(B)/mdcore.o
	$(CXX) $(CPUF) -o $@ $^ -lm -pthread
player_twin: $(B)/player_twin
FORCE:
clean:
	rm -rf build
.PHONY: all clean FORCE
