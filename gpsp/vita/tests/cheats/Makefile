# gpsp/vita/tests/cheats/Makefile -- the cheats tests.
#
#   make unit   ARCH=x86|armhf   test_engine (decoders, ciphers, the mGBA
#                                suite) and test_list (.cht files, the
#                                database, the list); no emulator needed
#   make e2e    ARCH=x86|armhf   cheat_e2e: both emulator cores, built here
#                                out of tree with ../../corebuild.mk, running
#                                the two cartridges of rom/ with codes on
#   make test                    all of it, x86 and armhf (qemu-arm)
#   make vita                    compile the cheats sources with
#                                arm-vita-eabi-gcc (compile only)
#   make db                      rebuild ../../assets/cheats.db
#                                (LIBRETRO_DB=/path/to/libretro-database)
#
# armhf is the ARM dynarec the Vita uses, run with qemu-arm (static binary).
# The x86 twin is a native x86-64 process on gpSP's x86 dynarec.  cheat_e2e
# also runs the GBA part once more with the dynarec switched off.
ARCH    ?= x86
DYNAREC := 1
RGBDS   ?=
LIBRETRO_DB ?= $(HOME)/src/libretro-database

GP := ../../..
B  := build/$(ARCH)

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
RUN     :=
else ifeq ($(ARCH),armhf)
CROSS   := arm-linux-gnueabihf-
CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
AR      := $(CROSS)ar
NM      := $(CROSS)nm
OBJCOPY := $(CROSS)objcopy
LD_R    := $(CROSS)ld
CPU_ARCH := arm
# as hostcore/Makefile: what the Vita build passes, on a Linux toolchain
CORE_ARCH_FLAGS := -marm -march=armv7-a -mfpu=neon -mfloat-abi=hard \
                   -fomit-frame-pointer -ffast-math -fno-optimize-sibling-calls
LDFLAGS_ARCH := -static -no-pie
RUN     := qemu-arm -L /usr/arm-linux-gnueabihf
else
$(error ARCH must be x86 or armhf)
endif

HAVE_DYNAREC := $(DYNAREC)
CORE_PLAT_DEFS := $(if $(filter 1,$(DYNAREC)),-DMMAP_JIT_CACHE)
FE_FLAGS := $(CORE_ARCH_FLAGS) -O2 -g -Wall -Wextra

include $(GP)/vita/corebuild.mk
include $(GP)/vita/cheats.mk

TEST_FLAGS := $(CORE_ARCH_FLAGS) -O2 -g -Wall -Wextra -std=gnu11 \
              -I$(GP) -I$(GP)/frontend-common -I$(GP)/vita -I. -Imgba
APP_FLAGS  := $(TEST_FLAGS) $(if $(filter 1,$(GBA_LINK)),-DGBA_LINK) \
              $(FE_INC) $(CHEATS_DEFS)

.DEFAULT_GOAL := unit

# ---- unit ---------------------------------------------------------------------
$(B)/unit/%.o: %.c $(GP)/cheat_engine.c $(GP)/cheat_engine.h mgba_shim.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_FLAGS) -Wno-unused-function -Wno-missing-field-initializers -c -o $@ $<
$(B)/unit/vbam_ref.o: vbam_ref.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CORE_ARCH_FLAGS) -O1 -fno-exceptions -fno-rtti -fno-strict-aliasing -c -o $@ $<

$(B)/test_engine: $(addprefix $(B)/unit/,test_engine.o mgba_shim.o mgba/gba_test_cheats.o vbam_ref.o)
	$(CC) $(CORE_ARCH_FLAGS) $(LDFLAGS_ARCH) -o $@ $^

# the list without an emulator: cheats_vita.c -DCHEATS_VITA_NO_CORE
$(B)/list/%.o: $(GP)/vita/%.c $(GP)/vita/cheats_vita.h $(GP)/vita/cheatdb.h $(GP)/cheat_engine.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_FLAGS) -DCHEATS_VITA_NO_CORE -c -o $@ $<
$(B)/list/cheat_engine.o: $(GP)/cheat_engine.c $(GP)/cheat_engine.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_FLAGS) -c -o $@ $<
$(B)/list/test_list.o: test_list.c $(GP)/vita/cheats_vita.h $(GP)/vita/cheatdb.h
	@mkdir -p $(dir $@)
	$(CC) $(TEST_FLAGS) -c -o $@ $<
$(B)/test_list: $(addprefix $(B)/list/,test_list.o cheats_vita.o cheatdb.o cheat_engine.o)
	$(CC) $(CORE_ARCH_FLAGS) $(LDFLAGS_ARCH) -o $@ $^

# A database of the two test cartridges, built by the real tool from
# fixture/ (a libretro-database tree in miniature).
# (always rebuilt: it takes a moment, and make cannot name the fixture's
# files -- "Nintendo - Game Boy Advance" has spaces)
build/fixture.db: rom/cheatrom.gba rom/gbcheat.gb FORCE
	@mkdir -p build/fixture
	python3 fixture/dat.py rom/cheatrom.gba rom/gbcheat.gb build/fixture
	python3 $(GP)/vita/tools/make_cheatdb.py --db build/fixture --out $@ > build/fixture.txt

rom/cheatrom.gba rom/gbcheat.gb: rom/crt0.S rom/main.c rom/gba.ld rom/gbcheat.asm
	$(MAKE) -C rom RGBDS=$(RGBDS)

unit: $(B)/test_engine $(B)/test_list build/fixture.db
	$(RUN) $(B)/test_engine
	$(RUN) $(B)/test_list $(GP)/vita/assets/cheats.db build/fixture.db build/work-$(ARCH)

# ---- end to end -----------------------------------------------------------------
E2E_OBJS := $(addprefix $(B)/app/,cheat_e2e.o fe_core_shim.o $(CHEATS_SRCS:.c=.o))
$(B)/app/cheat_e2e.o: cheat_e2e.c $(APP_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(APP_FLAGS) -MMD -MP -c -o $@ $<
$(B)/app/%.o: $(GP)/vita/%.c $(APP_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(APP_FLAGS) -MMD -MP -c -o $@ $<

$(B)/cheat_e2e: $(E2E_OBJS) $(FE_OBJS) $(GB_OBJS) $(CORE_A) $(CORE_B)
	$(CC) $(CORE_ARCH_FLAGS) $(LDFLAGS_ARCH) -o $@ $(E2E_OBJS) $(FE_OBJS) \
	  $(GB_OBJS) $(CORE_B) $(CORE_A) -lm -lpthread

-include $(E2E_OBJS:.o=.d)

e2e: $(B)/cheat_e2e build/fixture.db
	$(RUN) $(B)/cheat_e2e build/e2e-$(notdir $(B)) rom build/fixture.db

# ---- everything -------------------------------------------------------------------
test:
	$(MAKE) ARCH=x86 unit
	$(MAKE) ARCH=armhf unit
	nice -n 10 $(MAKE) -j2 ARCH=x86 build/x86/cheat_e2e
	nice -n 10 $(MAKE) -j2 ARCH=armhf build/armhf/cheat_e2e
	$(MAKE) ARCH=x86 e2e
	$(MAKE) ARCH=armhf e2e

# ---- compile for the console (nothing here can run it) ------------------------------
# With the flags gpsp/vita/Makefile gives each kind of object: the core's
# (cheat_engine.c, cheats.c), the frontend layer's (gbcore) and the app's.
VITASDK ?= /opt/vitasdk
VITA_CC := $(VITASDK)/bin/arm-vita-eabi-gcc
VITA_CPU  := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
VITA_CORE := $(VITA_CPU) -fomit-frame-pointer -ffast-math -fno-unwind-tables \
             -fno-asynchronous-unwind-tables -ftree-vectorize \
             -fno-optimize-sibling-calls -O3 -DNDEBUG -Wall \
             -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H -D__LIBRETRO__ \
             -DINLINE=inline -DFRONTEND_SUPPORTS_RGB565 $(GBADHOC_CORE_DEFS) \
             -DVITA -DHAVE_DYNAREC -DARM_ARCH -DGBA_LINK $(INCFLAGS)
VITA_FE   := $(VITA_CPU) -O2 -g -Wall -Wextra -DVITA -DGBA_LINK $(FE_INC)
VITA_APP  := $(VITA_CPU) -O2 -g -Wall -Wextra -std=gnu11 -DVITA -DGBA_LINK \
             $(FE_INC) -I$(GP)/vita $(CHEATS_DEFS)
vita:
	@mkdir -p build/vita
	$(VITA_CC) $(VITA_APP) -c -o build/vita/cheats_vita.o $(GP)/vita/cheats_vita.c
	$(VITA_CC) $(VITA_APP) -c -o build/vita/cheatdb.o $(GP)/vita/cheatdb.c
	$(VITA_CC) $(VITA_CORE) -c -o build/vita/cheat_engine.o $(GP)/cheat_engine.c
	$(VITA_CC) $(VITA_CORE) -c -o build/vita/cheats.o $(GP)/cheats.c
	$(VITA_CC) $(VITA_CORE) -c -o build/vita/gba_memory.o $(GP)/gba_memory.c
	$(VITA_CC) $(VITA_FE) -I$(GP)/gbcore -c -o build/vita/gbcore_tgbdual.o $(GP)/gbcore/gbcore_tgbdual.c
	@$(VITASDK)/bin/arm-vita-eabi-size build/vita/*.o
	@$(VITASDK)/bin/arm-vita-eabi-nm -u build/vita/cheats_vita.o build/vita/cheatdb.o | sort -u

db:
	python3 $(GP)/vita/tools/make_cheatdb.py --db $(LIBRETRO_DB) --out $(GP)/vita/assets/cheats.db

clean:
	rm -rf build
	$(MAKE) -C rom clean

.PHONY: unit e2e test vita db clean
