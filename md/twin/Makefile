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
all: $(B)/md_twin
$(B)/mdcore.o: FORCE
	$(MAKE) -C ../core TARGET=$(TARGET) B=$(BA) PROFILE=$(PROFILE) all
$(B)/md_twin: main.cpp $(B)/mdcore.o $(FE)/host.h
	$(CXX) -O2 -g -std=gnu++14 -Wall -Wextra $(CPUF) -I$(FE) -o $@ main.cpp $(B)/mdcore.o -lm
FORCE:
clean:
	rm -rf build
.PHONY: all clean FORCE
