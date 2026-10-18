# gpsp/vita/hostcore/Makefile -- the Linux twin of the WHOLE Vita app.
#
# The same main_vita.c, audio_vita.c, PSP UI (psp/ui_psp.c & co., compiled
# -DVITA), video_vita.c and pspcompat.c as the VPK -- the list is ../app.mk,
# shared with ../Makefile -- with three stand-ins for the console:
#
#   plat_host.c        for plat_vita.c: time, the display's clock, the pad
#                      (a script), the audio port (a WAV), threads
#   ../vback_soft.c    for vback_vita2d.c + libvita2d: the software renderer,
#                      whose 960x544 frame --shot writes as a PNG
#   ../stubs_vita.c    weak do-nothing linking, until link.mk brings the
#                      real files (see ../app.mk); cheats are real here too,
#                      with the database of ../assets (HOST_APP_DIR, --app)
#
#   make ARCH=x86     -> build/x86/gbadhoc_host     x86-64, the x86 dynarec
#   make ARCH=armhf   -> build/armhf/gbadhoc_host   ARMv7-A hard-float, the
#                        ARM dynarec the Vita uses; run it with
#                        qemu-arm -L /usr/arm-linux-gnueabihf build/armhf/gbadhoc_host
#   make ARCH=armhf VITASIM=1 -> build/armhf-vitasim/gbadhoc_host
#                        the armhf twin with the core built -DVITA: the
#                        Vita's translation-cache path (vita/jit_vita.c,
#                        one VM block for both instances, the reach check,
#                        sceKernelSyncVMDomain) over a simulated kernel
#   make test         build x86 + armhf + testrom/, then run_tests.sh (the
#                     cores: every test ROM on x86 against armhf, and the
#                     link bench) and app_tests.py (the app: the game list,
#                     the menu, states, fast forward... on both).  About
#                     fifteen minutes, nearly all of it qemu.
#
# Both link instance A and instance B of the GBA core and both TGB Dual
# instances, with the same feature switches as the Vita build
# (../coreflags.mk).  DYNAREC=0 builds the interpreter-only core.
#
# The command line of the program is at the top of plat_host.c.
#
# The x86 twin is a native x86-64 process: gpSP's x86 emitter runs in long
# mode as well (x86/x86_stub.S has both ABIs), which is how the root
# Makefile's `platform=unix` and tools/gbamirror build it.  -no-pie keeps the
# image low so the mapped translation cache lands within the emitter's
# 2 GiB reach (memmap.c).
ARCH    ?= x86
DYNAREC ?= 1
# VITASIM=1 (armhf only): build the core with -DVITA and run the Vita's own
# translation-cache path -- vita/jit_vita.c, libretro.c's VITA glue,
# sceKernelSyncVMDomain -- against a simulated kernel (vitasim.c).
VITASIM ?= 0
# SAN=1 (x86): the APP's objects -- main_vita.c, the UI, the video layer --
# built with AddressSanitizer and UBSan (build/x86-san).  The cores are not
# instrumented: generated code and the sanitizer's bookkeeping do not mix.
#   make ARCH=x86 SAN=1 && ./app_tests.py x86-san
SAN ?= 0

GP := ../..
V  := ..
comma := ,
B  := build/$(ARCH)$(if $(filter 0,$(DYNAREC)),-interp)$(if $(filter 1,$(VITASIM)),-vitasim)$(if $(filter 1,$(SAN)),-san)
SAN_FLAGS := $(if $(filter 1,$(SAN)),-fsanitize=address$(comma)undefined -fno-omit-frame-pointer)

ifeq ($(ARCH),x86)
CC      := gcc
CXX     := g++
AR      := ar
NM      := nm
OBJCOPY := objcopy
LD_R    := ld
CPU_ARCH := x86_32
CORE_ARCH_FLAGS := -fomit-frame-pointer -ffast-math
LDFLAGS_ARCH := -no-pie
else ifeq ($(ARCH),armhf)
CROSS   := arm-linux-gnueabihf-
CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
AR      := $(CROSS)ar
NM      := $(CROSS)nm
OBJCOPY := $(CROSS)objcopy
LD_R    := $(CROSS)ld
CPU_ARCH := arm
# What the root Makefile's vita section passes, on a Linux toolchain: ARM
# state, Cortex-A9-class ARMv7-A with NEON, hard-float.
CORE_ARCH_FLAGS := -marm -march=armv7-a -mfpu=neon -mfloat-abi=hard \
                   -fomit-frame-pointer -ffast-math -fno-optimize-sibling-calls
# Static and not PIE: the image starts at 0x10000, as low as the Vita's sits
# at 0x81000000 relative to its own memory, and qemu-arm needs no sysroot
# libraries at run time.
LDFLAGS_ARCH := -static -no-pie
else
$(error ARCH must be x86 or armhf)
endif

include $(V)/app.mk

HAVE_DYNAREC := $(DYNAREC)
ifeq ($(VITASIM),1)
ifneq ($(ARCH),armhf)
$(error VITASIM=1 needs ARCH=armhf: it is the ARM emitter's branch reach that the Vita path guards)
endif
CORE_PLAT_DEFS := -DVITA -Ivitasim
else
# MMAP_JIT_CACHE: a Linux process maps its translation caches (memmap.c); the
# Vita build gets them from vita/jit_vita.c instead.
CORE_PLAT_DEFS := $(if $(filter 1,$(DYNAREC)),-DMMAP_JIT_CACHE)
endif
FE_FLAGS := $(CORE_ARCH_FLAGS) -O2 -g -Wall -Wextra $(ND_DEFS)

include $(V)/corebuild.mk

# libretro-common's VITA branches are file I/O through sceIo*; the simulation
# is about memory, so those two files stay POSIX.
$(B)/core/libretro/libretro-common/file/file_path.o \
$(B)/core/libretro/libretro-common/vfs/vfs_implementation.o: CORE_FILE_FLAGS := -UVITA

# -DPSPCOMPAT_PLAT: pspcompat.c's clock, vertical blank and pad are plat.h's,
# so the UI's modal screens and the main loop share ONE timeline and ONE
# input script (plat_host.c).
APP_FLAGS := $(CORE_ARCH_FLAGS) -O2 -g -Wall -Wextra -std=gnu11 -MMD -MP \
             $(SAN_FLAGS) \
             $(APP_DEFS) -DPSPCOMPAT_PLAT \
             $(if $(filter 1,$(GBA_LINK)),-DGBA_LINK) \
             -DGBADHOC_VERSION=\"host-$(GIT_VERSION)\" \
             -DHOST_APP_DIR=\"$(abspath $(V)/assets)\" \
             $(APP_INC_FLAGS) $(FE_INC) $(ND_DEFS)
APP_ALL   := $(APP_ALL_HOST)
ifeq ($(VITASIM),1)
APP_FLAGS += -DVITASIM -Ivitasim
APP_ALL   += $(V)/jit_vita.c vitasim.c
endif
APP_OBJS  := $(foreach s,$(APP_ALL),$(call app_obj,$(s)))

.DEFAULT_GOAL := all
all: $(B)/gbadhoc_host

$(foreach s,$(APP_ALL),$(eval $(call APP_RULE,$(s))))

# Link with the C driver: the core's two C++ sources use no libstdc++
# (-fno-rtti -fno-exceptions).
$(B)/gbadhoc_host: $(APP_OBJS) $(FE_OBJS) $(GB_OBJS) $(CORE_A) $(CORE_B) $(CORE_R)
	$(CC) $(CORE_ARCH_FLAGS) $(LDFLAGS_ARCH) $(SAN_FLAGS) -o $@ $(APP_OBJS) $(FE_OBJS) \
	  $(GB_OBJS) $(CORE_R) $(CORE_B) $(CORE_A) -lm -lpthread

# ROMS: a directory of extra test cartridges for run_tests.sh (default: the
# ones the port builds itself).
test:
	$(MAKE) ARCH=x86
	$(MAKE) ARCH=armhf
	$(MAKE) -C testrom
	$(MAKE) -C ../tests/cheats/rom cheatrom.gba
	./run_tests.sh $(ROMS)
	./app_tests.py x86
	./app_tests.py armhf

clean:
	rm -rf $(B)

-include $(APP_OBJS:.o=.d)
.PHONY: all clean test
