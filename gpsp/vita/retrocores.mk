# retrocores.mk -- Sega Mega Drive / Genesis, NES and SNES cores for the Vita.
#
#   md    Genesis Plus GX   (gpsp/retro/genesis_plus_gx)  Mega Drive / Genesis
#   nes   FCEUmm            (gpsp/retro/fceumm)           NES / Famicom
#   snes  Snes9x 2005       (gpsp/retro/snes9x2005)       Super Nintendo
#
# Each is an unmodified libretro core.  It is built with its own makefile
# (platform=vita, in a private copy under build/vita/retro/<core>-src, so the
# source tree stays clean), joined with a private copy of the libretro-common
# subset it expects its frontend to provide, and then partially linked into
# ONE relocatable object in which every symbol is local except the libretro
# entry points, renamed <core>_retro_*  (md_retro_run, nes_retro_run, ...).
# So the three cores, and gpSP's own retro_* functions, share no symbol and
# need no namespace of their own; the same trick gbcore_instance.ld plays
# for the second GB instance.  frontend-common/fe_retro.c drives them.
#
# Included by Makefile (before the app's link rules); needs $(B), $(PREFIX),
# $(CC), $(AR) and $(VITASDK) from there.
#
# RETRO_CORES=0 builds without them (the app then lists no new consoles).

RETRO_CORES ?= 1
RC_SRC := ../retro
RC_B   := $(B)/retro
RC_LRC := ../libretro/libretro-common

RC_ARCH := -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard

# The libretro-common pieces the cores call (RetroArch links them in).
RC_COMMON_SRCS := file/file_path_io.c streams/file_stream.c streams/file_stream_transforms.c \
                  vfs/vfs_implementation.c file/file_path.c \
                  string/stdstring.c compat/compat_strl.c \
                  compat/compat_posix_string.c time/rtime.c \
                  encodings/encoding_utf.c
RC_COMMON_OBJS := $(addprefix $(RC_B)/lrc/,$(notdir $(RC_COMMON_SRCS:.c=.o)))
RC_COMMON_VPATH := $(addprefix $(RC_LRC)/,$(dir $(RC_COMMON_SRCS)))
vpath %.c $(RC_COMMON_VPATH)

$(RC_B)/lrc/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(RC_ARCH) -O2 -DVITA -I$(RC_LRC)/include -c $< -o $@

$(RC_B)/rcommon.o: $(RC_COMMON_OBJS)
	$(PREFIX)ld -r $^ -o $@

# $(call RC_CORE,name,source dir,archive,make command)
define RC_CORE
$(RC_B)/$(1).o: $(RC_B)/rcommon.o $(shell find $(RC_SRC)/$(2) -type f -name '*.[ch]' 2>/dev/null) \
                retrocores.mk
	rm -rf $(RC_B)/$(1)-src && mkdir -p $(RC_B)/$(1)-src
	cp -a $(RC_SRC)/$(2)/. $(RC_B)/$(1)-src/
	cd $(RC_B)/$(1)-src && CFLAGS="$(RC_ARCH)" CXXFLAGS="$(RC_ARCH)" \
	  PATH=$(VITASDK)/bin:$$$$PATH $(4) -j$(RC_JOBS)
	$(PREFIX)ld -r --whole-archive $(RC_B)/$(1)-src/$(3) --no-whole-archive \
	  $(RC_B)/rcommon.o -o $(RC_B)/$(1).joined.o
	$(PREFIX)nm -g --defined-only $(RC_B)/$(1).joined.o | awk '{print $$$$3}' \
	  | grep '^retro_' | grep -v '^retro_vfs_' | sort > $(RC_B)/$(1).api
	sed 's/.*/&  $(1)_&/' $(RC_B)/$(1).api > $(RC_B)/$(1).rename
	sed 's/^/$(1)_/' $(RC_B)/$(1).api > $(RC_B)/$(1).keep
	$(PREFIX)objcopy --redefine-syms=$(RC_B)/$(1).rename \
	  --keep-global-symbols=$(RC_B)/$(1).keep $(RC_B)/$(1).joined.o $$@
	rm -f $(RC_B)/$(1).joined.o
endef

RC_JOBS ?= 4
$(eval $(call RC_CORE,md,genesis_plus_gx,genesis_plus_gx_libretro_vita.a,$(MAKE) -f Makefile.libretro platform=vita))
$(eval $(call RC_CORE,nes,fceumm,fceumm_libretro_vita.a,$(MAKE) -f Makefile.libretro platform=vita))
$(eval $(call RC_CORE,snes,snes9x2005,snes9x2005_libretro_vita.a,$(MAKE) platform=vita))

# fe_retro.c, the frontend side, is in corebuild.mk's FE_OBJS (the host twin
# builds it too, without FE_RETRO_CORES: every retro console then reports
# itself unavailable).
ifeq ($(RETRO_CORES),1)
RC_OBJS := $(RC_B)/md.o $(RC_B)/nes.o $(RC_B)/snes.o
FE_FLAGS += -DFE_RETRO_CORES=1
APP_DEFS += -DFE_RETRO_CORES=1
else
RC_OBJS :=
endif
