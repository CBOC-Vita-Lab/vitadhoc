# gpsp/vita/host -- the Linux host twin of the Vita UI.
#
#   make            build ui_host_test
#   make run        fixture + every screenshot into out/shots_all/, then the
#                   pixel checks: the scaler (check_shots.py) and the menus'
#                   text contrast over the worst backdrop (check_contrast.py)
#   make shots      run, then refresh the committed set in shots/
#   make armhf-check   the same test built for 32-bit ARM, run under qemu-arm,
#                      screenshots compared with the x86-64 ones (hours)
#   make armhf-check-ui   the same for the menu pages' groups (minutes)
#   make psp-check  the PSP configuration's object code is unchanged
#   make vita-linkcheck   compile + link for the Vita (nothing is run)
#   make clean
#
# The UI sources are the PSP's, compiled -DVITA exactly as the Vita build
# compiles them; only the backend (vback_soft.c) and pspcompat.c's POSIX body
# differ from the console.  See ui_host_test.c.

GPSP := ../..
CC   ?= gcc
PY   ?= python3

CFLAGS := -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -Wno-missing-field-initializers -Wno-format-truncation \
          -DVITA -DGBA_LINK \
          -I$(GPSP)/vita -I$(GPSP)/vita/third_party/stb -I$(GPSP)/psp \
          -I$(GPSP)/frontend-common -I$(GPSP)/netdrv -I$(GPSP)
ASFLAGS := -I$(GPSP)/assets/fonts
LDLIBS := -lm -lpthread

SRC_UI   := $(GPSP)/psp/ui_psp.c $(GPSP)/psp/osd_psp.c \
            $(GPSP)/psp/config_psp.c $(GPSP)/psp/ctl_map.c \
            $(GPSP)/psp/stb_impl.c $(GPSP)/psp/font_8x16.c
SRC_VID  := $(GPSP)/vita/video_vita.c $(GPSP)/vita/video_vita_text.c \
            $(GPSP)/vita/upscale_vita.c \
            $(GPSP)/vita/vback_soft.c $(GPSP)/vita/pspcompat.c \
            $(GPSP)/vita/ui_hooks_default.c \
            $(GPSP)/vita/third_party/stb/stb_impl_vita.c
SRC_FE   := $(GPSP)/frontend-common/fe_util.c $(GPSP)/frontend-common/fe_evt.c \
            $(GPSP)/frontend-common/fe_favs.c \
            $(GPSP)/frontend-common/fe_linkmode.c
SRC_ASM  := $(GPSP)/vita/video_vita_fonts.S
SRCS     := ui_host_test.c $(SRC_UI) $(SRC_VID) $(SRC_FE)

OUT    ?= out
OBJDIR := $(OUT)/obj
RUNNER ?=
OBJS   := $(patsubst %.c,$(OBJDIR)/%.o,$(notdir $(SRCS))) \
          $(OBJDIR)/video_vita_fonts.o $(OBJDIR)/xbrz.o $(OBJDIR)/xbrz_c.o
VPATH  := $(sort $(dir $(SRCS) $(SRC_ASM)))

all: $(OUT)/ui_host_test

$(OUT)/ui_host_test: $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDLIBS)

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -c -o $@ $<

# xBRZ (C++), for the CPU upscalers
$(OBJDIR)/%.o: $(GPSP)/vita/third_party/xbrz/%.cpp | $(OBJDIR)
	$(CC) -x c++ -std=gnu++14 -O2 -fno-exceptions -fno-rtti -w -c -o $@ $<

# third-party code is not ours to lint
$(OBJDIR)/stb_impl.o $(OBJDIR)/stb_impl_vita.o: CFLAGS += -w

$(OBJDIR)/video_vita_fonts.o: video_vita_fonts.S | $(OBJDIR)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $@

run: $(OUT)/ui_host_test
	rm -rf $(OUT)/root $(OUT)/shots_all
	TZ=UTC $(PY) make_fixture.py $(OUT)/root
	TZ=UTC $(RUNNER) ./$(OUT)/ui_host_test $(OUT)/root $(OUT)/shots_all $(ONLY)
	$(PY) check_shots.py $(OUT)/shots_all
	$(PY) check_contrast.py $(OUT)/shots_all

# The representative set that is committed: the names in shots.list.
shots: run
	mkdir -p shots
	rm -f shots/*.png
	set -e; for n in $$(grep -v '^#' shots.list); do \
	   cp out/shots_all/$$n.png shots/; done
	ls shots/*.png | wc -l

# The same sources as 32-bit ARM (char unsigned, 32-bit long and pointers, as
# on the Vita), run under qemu-arm, and every screenshot compared with the
# x86-64 run's.
ARMCC   ?= arm-linux-gnueabihf-gcc
ARMROOT ?= /usr/arm-linux-gnueabihf
armhf-check: run
	$(MAKE) OUT=out/armhf RUNNER="qemu-arm -L $(ARMROOT)" \
	   CC="$(ARMCC) -march=armv7-a -mfpu=neon -mfloat-abi=hard -marm" run
	$(PY) compare_shots.py out/shots_all out/armhf/shots_all

# The same check for some test groups only -- the menus' pages take minutes
# under qemu-arm where the whole run (the browser's art, the scalers) takes
# hours.  Both twins run the SAME groups from a fresh fixture (the virtual
# clock and config.ini carry from group to group, so a subset is only
# comparable with the same subset), then every screenshot is compared.
#   make armhf-check-ui                    the Play together, Cheats, touch
#                                          and light-theme groups
#   make armhf-check-ui GROUPS="cheats"
GROUPS ?= play cheats touch light
ARMFLAGS := -march=armv7-a -mfpu=neon -mfloat-abi=hard -marm
armhf-check-ui: $(OUT)/ui_host_test
	$(MAKE) OUT=out/armhf CC="$(ARMCC) $(ARMFLAGS)" out/armhf/ui_host_test
	rm -rf out/sub out/armhf/sub
	mkdir -p out/sub out/armhf/sub
	TZ=UTC $(PY) make_fixture.py out/sub/root
	TZ=UTC $(PY) make_fixture.py out/armhf/sub/root
	TZ=UTC ./$(OUT)/ui_host_test out/sub/root out/sub/shots "$(GROUPS)" \
	   > out/sub/log.txt
	TZ=UTC qemu-arm -L $(ARMROOT) ./out/armhf/ui_host_test \
	   out/armhf/sub/root out/armhf/sub/shots "$(GROUPS)" \
	   > out/armhf/sub/log.txt
	@tail -1 out/sub/log.txt; tail -1 out/armhf/sub/log.txt
	$(PY) compare_shots.py out/sub/shots out/armhf/sub/shots

psp-check:
	sh psp_unchanged.sh

clean:
	rm -rf out

-include $(OBJS:.o=.d)

.PHONY: all run shots armhf-check armhf-check-ui psp-check clean

# ---- PS Vita compile + link check (needs VitaSDK; nothing is run) ----------
#   make vita-linkcheck
# Builds vita/ui.mk's sources with arm-vita-eabi-gcc and links them with
# vita_linkcheck.c into out/vita/linkcheck.{elf,velf,self}.
VITASDK ?= /opt/vitasdk
VCC     := $(VITASDK)/bin/arm-vita-eabi-gcc
VDIR    := out/vita
GPSP_DIR := $(GPSP)
include $(GPSP)/vita/ui.mk
VSRCS := vita_linkcheck.c $(VITA_UI_SRCS_C) \
         $(GPSP)/frontend-common/fe_util.c $(GPSP)/frontend-common/fe_evt.c \
         $(GPSP)/frontend-common/fe_favs.c \
         $(GPSP)/frontend-common/fe_linkmode.c
VOBJS := $(patsubst %.c,$(VDIR)/%.o,$(notdir $(VSRCS))) \
         $(VDIR)/video_vita_fonts.o
VCFLAGS := -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
           -Wno-missing-field-initializers -Wno-format-truncation \
           -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
           -DGBA_LINK $(VITA_UI_CFLAGS) -I$(GPSP)/netdrv -I$(GPSP)
vpath %.c $(sort $(dir $(VSRCS)))

$(VDIR)/%.o: %.c | $(VDIR)
	$(VCC) $(VCFLAGS) -MMD -c -o $@ $<
$(VDIR)/stb_impl.o $(VDIR)/stb_impl_vita.o $(VDIR)/vita2d.o \
$(VDIR)/vita2d_draw.o $(VDIR)/vita2d_texture.o $(VDIR)/utils.o: VCFLAGS += -w
$(VDIR)/video_vita_fonts.o: $(GPSP)/vita/video_vita_fonts.S | $(VDIR)
	$(VCC) $(VITA_UI_ASFLAGS) -c -o $@ $<
$(VDIR):
	mkdir -p $@

vita-linkcheck: $(VOBJS)
	$(VCC) -Wl,-q -o $(VDIR)/linkcheck.elf $(VOBJS) $(VITA_UI_SHADERS) \
	   $(VITA_UI_LIBS)
	$(VITASDK)/bin/vita-elf-create $(VDIR)/linkcheck.elf $(VDIR)/linkcheck.velf
	$(VITASDK)/bin/vita-make-fself -s $(VDIR)/linkcheck.velf $(VDIR)/linkcheck.self
	@ls -l $(VDIR)/linkcheck.elf $(VDIR)/linkcheck.velf $(VDIR)/linkcheck.self
	@$(VITASDK)/bin/arm-vita-eabi-size $(VDIR)/linkcheck.elf

.PHONY: vita-linkcheck
-include $(VOBJS:.o=.d)
