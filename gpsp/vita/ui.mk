# gpsp/vita/ui.mk -- the Vita video layer and UI, as a Makefile fragment.
#
# Include it from the Vita frontend's Makefile with GPSP_DIR set to the gpsp/
# directory (no trailing slash):
#
#     GPSP_DIR := ..
#     include $(GPSP_DIR)/vita/ui.mk
#     OBJS    += $(VITA_UI_OBJS)
#     CFLAGS  += $(VITA_UI_CFLAGS)
#     LIBS    += $(VITA_UI_LIBS)
#
# and give .S files the include path in VITA_UI_ASFLAGS (the font blob).
# Nothing here needs a library VitaSDK does not ship: libvita2d's core is
# built from third_party/vita2d, stb is header-only.
#
# What it provides:
#   VITA_UI_SRCS_C    C sources (paths relative to the including Makefile)
#   VITA_UI_SRCS_S    assembler sources (the embedded Inter TTFs)
#   VITA_UI_SHADERS   libvita2d's precompiled shader objects (link as-is)
#   VITA_UI_OBJS      the .o list for all of the above
#   VITA_UI_CFLAGS    -D and -I flags the sources need
#   VITA_UI_ASFLAGS   -I for .incbin
#   VITA_UI_LIBS      the VitaSDK stubs to link, in order
#
# The main loop and the link layer provide the hooks in vita/ui_host_hooks.h
# (vita/ui_hooks_default.c, listed below, holds weak "not available" versions
# of the ones the Play together and Cheats pages added, so the app links
# before link_vita.c / local_vita.c / cheats_vita.c are in the build), and
# the build must also link frontend-common/fe_util.c fe_evt.c fe_favs.c
# fe_linkmode.c and fe_host.c (not listed here: the frontend already builds
# them).  psp/ui_psp.c includes vita/ui_vita_touch.h, ui_vita_widgets.h,
# ui_vita_play.h, ui_vita_cheats.h, ui_vita_browser.h and cheats_vita.h:
# headers, found through -I$(GPSP_DIR)/vita.

GPSP_DIR ?= ..

# The UI: the PSP frontend's own files, compiled -DVITA.
VITA_UI_SRCS_C := \
   $(GPSP_DIR)/psp/ui_psp.c \
   $(GPSP_DIR)/psp/osd_psp.c \
   $(GPSP_DIR)/psp/config_psp.c \
   $(GPSP_DIR)/psp/ctl_map.c \
   $(GPSP_DIR)/psp/share_psp.c \
   $(GPSP_DIR)/psp/mgift_cart.c \
   $(GPSP_DIR)/psp/mgift_payload.c \
   $(GPSP_DIR)/psp/stb_impl.c

# The video layer, the GXM backend, the PSP SDK shim.
VITA_UI_SRCS_C += \
   $(GPSP_DIR)/vita/video_vita.c \
   $(GPSP_DIR)/vita/upscale_vita.c \
   $(GPSP_DIR)/vita/third_party/xbrz/xbrz.cpp \
   $(GPSP_DIR)/vita/third_party/xbrz/xbrz_c.cpp \
   $(GPSP_DIR)/vita/video_vita_text.c \
   $(GPSP_DIR)/vita/vstyle.c \
   $(GPSP_DIR)/vita/vback_vita2d.c \
   $(GPSP_DIR)/vita/pspcompat.c \
   $(GPSP_DIR)/vita/ui_hooks_default.c \
   $(GPSP_DIR)/vita/third_party/stb/stb_impl_vita.c

# libvita2d, core subset (third_party/vita2d/PROVENANCE).
VITA_UI_SRCS_C += \
   $(GPSP_DIR)/vita/third_party/vita2d/source/vita2d.c \
   $(GPSP_DIR)/vita/third_party/vita2d/source/vita2d_draw.c \
   $(GPSP_DIR)/vita/third_party/vita2d/source/vita2d_texture.c \
   $(GPSP_DIR)/vita/third_party/vita2d/source/utils.c

VITA_UI_SRCS_S := $(GPSP_DIR)/vita/video_vita_fonts.S

VITA_UI_SHADERS := \
   $(GPSP_DIR)/vita/third_party/vita2d/shader/clear_v_gxp.o \
   $(GPSP_DIR)/vita/third_party/vita2d/shader/clear_f_gxp.o \
   $(GPSP_DIR)/vita/third_party/vita2d/shader/color_v_gxp.o \
   $(GPSP_DIR)/vita/third_party/vita2d/shader/color_f_gxp.o \
   $(GPSP_DIR)/vita/third_party/vita2d/shader/texture_v_gxp.o \
   $(GPSP_DIR)/vita/third_party/vita2d/shader/texture_f_gxp.o \
   $(GPSP_DIR)/vita/third_party/vita2d/shader/texture_tint_f_gxp.o

VITA_UI_OBJS := $(VITA_UI_SRCS_C:.c=.o) $(VITA_UI_SRCS_S:.S=.o) \
                $(VITA_UI_SHADERS)

# -DVITA selects the Vita paths in psp/*.c; the core's own Makefile
# (platform=vita) defines it too.  VITA2D_NO_PGF: see third_party/vita2d.
VITA_UI_CFLAGS := -DVITA -DVITA2D_NO_PGF \
   -I$(GPSP_DIR)/vita \
   -I$(GPSP_DIR)/vita/third_party/stb \
   -I$(GPSP_DIR)/vita/third_party/vita2d/include \
   -I$(GPSP_DIR)/psp \
   -I$(GPSP_DIR)/frontend-common

VITA_UI_ASFLAGS := -I$(GPSP_DIR)/assets/fonts

# GXM and what libvita2d's core pulls in, the pad, the front touch panel,
# the system keyboard (the IME dialog is in SceCommonDialog; SceAppUtil is
# its prerequisite and gives the console's user name), files, the clock
# (RTC: local time for the save-state stamps).
VITA_UI_LIBS := \
   -lSceGxm_stub -lSceDisplay_stub -lSceSysmodule_stub \
   -lSceCommonDialog_stub -lSceAppMgr_stub \
   -lSceCtrl_stub -lSceTouch_stub -lSceAppUtil_stub -lSceRtc_stub -lm
