# gpsp/vita/app.mk -- WHAT THE APP IS MADE OF, for both builds.
#
# Included by gpsp/vita/Makefile (the VPK) and by gpsp/vita/hostcore/Makefile
# (the Linux twin of the whole app), each with
#
#     V := <path of gpsp/vita from that Makefile>      ("." and "..")
#
# so the two builds cannot drift apart: a file added here is in both.
#
# ---- THE LIST -----------------------------------------------------------------
# Paths are relative to gpsp/vita/ ("../psp/x.c" reaches the rest of gpsp).
#
#   APP_SRCS        built for the Vita AND the twin: plain C against plat.h,
#                   psp/video_psp.h and frontend-common -- no Sce call, no
#                   POSIX-only call
#   APP_SRCS_VITA   the Vita only (the Sony calls)
#   APP_SRCS_HOST   the twin only (their stand-ins on Linux)
#   APP_DEFS        -D... for every app source, both builds
#   APP_INCDIRS     extra include directories, relative to gpsp/vita/
#   APP_LIBS_VITA   stub libraries for the Vita link (-lSceXxx_stub)
#   VPK_EXTRA       files for the package, as  <file>=<path inside the VPK>
#                   (the file relative to gpsp/vita/; "app0:<path>" at run
#                   time)
#
# ---- OPTIONAL FRAGMENTS: cheats.mk, link.mk -----------------------------------
# Read here if they exist, AFTER the lists below are set, so a fragment only
# appends:
#
#     # gpsp/vita/cheats.mk   (exists: the engine, the reader, the database)
#     APP_SRCS  += cheats_vita.c cheatdb.c
#     VPK_EXTRA += assets/cheats.db=cheats.db
#
#     # gpsp/vita/link.mk     (to come: the two link files of app_vita.h)
#     APP_SRCS      += link_vita.c local_vita.c
#     APP_SRCS_HOST += hostcore/net_host.c       # net_vita.h on Linux
#     APP_DEFS      += -DLINK_SOMETHING
#
# link.mk's files REPLACE the do-nothing definitions in stubs_vita.c by
# themselves: those are weak, so nothing has to be removed from any list
# (and one of the two files alone replaces only its own part).
# The network files of net.mk (net_vita.c, the transports, UPnP) are already
# in the VPK; the twin gets none of them until link.mk names its stand-in.

ifndef V
$(error app.mk: set V to the path of gpsp/vita before including it)
endif

APP_SRCS      := main_vita.c stubs_vita.c audio_vita.c fe_core_shim.c
# the local link (app_vita.h part B): both machines on this console
APP_SRCS      += local_vita.c
APP_SRCS      += rewind_vita.c
# the game list's previews: the last ten seconds played (clip_vita.h)
APP_SRCS      += clip_vita.c
APP_SRCS      += preview_vita.c
APP_SRCS      += menu_diag.c
APP_SRCS      += menu_load.c menu_files.c
# what the game list knows about a cartridge (names, blurbs) and its box art
# fetched by itself (gamedb.h, artfetch.h)
APP_SRCS      += gamedb.c artfetch.c
# the GBA's picture drawn on another core (render_thread.h)
APP_SRCS      += render_thread.c
# what has been played, when and for how long (play_log.h); patches applied
# to a game as it starts (rom_patch.h)
APP_SRCS      += play_log.c rom_patch.c
# RetroAchievements (ra_vita.h): the app's side, the web requests, and the
# runtime every RetroAchievements emulator uses (third_party/rcheevos, MIT;
# PROVENANCE there).  Built with RC_NO_THREADS: one thread calls it.
APP_SRCS      += ra_vita.c ra_http.c
RC_DIR        := third_party/rcheevos/src
APP_SRCS      += $(RC_DIR)/rc_client.c $(RC_DIR)/rc_compat.c \
                 $(RC_DIR)/rc_util.c $(RC_DIR)/rc_version.c \
                 $(addprefix $(RC_DIR)/rcheevos/,alloc.c condition.c \
                    condset.c consoleinfo.c format.c lboard.c memref.c \
                    operand.c rc_validate.c richpresence.c runtime.c \
                    runtime_progress.c trigger.c value.c) \
                 $(addprefix $(RC_DIR)/rapi/,rc_api_common.c rc_api_info.c \
                    rc_api_runtime.c rc_api_user.c) \
                 $(RC_DIR)/rhash/md5.c
APP_SRCS_VITA := plat_vita.c jit_vita.c heap_vita.c
APP_SRCS_HOST := hostcore/plat_host.c
APP_DEFS      := -DVITA -DRC_NO_THREADS
APP_INCDIRS   := third_party/rcheevos/include third_party/rcheevos/src
APP_LIBS_VITA := -lSceHttp_stub -lSceSsl_stub
VPK_EXTRA     :=

VPK_EXTRA     += assets/gamedb.txt=gamedb.txt

-include $(V)/cheats.mk
-include $(V)/link.mk

# ---- the UI, the video layer, the network: the other workstreams' lists ------
# ui.mk and net.mk stay theirs.  They define VITA_UI_* / NET_VITA_*; this is
# where those become part of the app.  ui.mk's paths follow GPSP_DIR, net.mk's
# are relative to gpsp/vita/.
GPSP_DIR := $(V)/..
include $(V)/ui.mk
include $(V)/net.mk

# The GXM backend and libvita2d are the Vita's; everything else of ui.mk --
# the PSP UI compiled -DVITA, video_vita.c, the fonts, pspcompat.c -- is the
# same source on the twin, which draws through vback_soft.c instead.
# (filter takes one % per pattern: the directory test is a findstring.)
in_vita2d    = $(findstring /third_party/vita2d/,$(1))
in_rcheevos  = $(findstring /third_party/rcheevos/,$(1))
UI_GXM_SRCS := $(foreach s,$(VITA_UI_SRCS_C),$(if $(or $(filter %/vback_vita2d.c,$(s)),$(call in_vita2d,$(s))),$(s)))
UI_SRCS     := $(filter-out $(UI_GXM_SRCS),$(VITA_UI_SRCS_C)) $(VITA_UI_SRCS_S)
# netdrv.c is frontend-common's (corebuild.mk FE_OBJS): not twice.
NET_SRCS    := $(addprefix $(V)/,$(filter-out ../netdrv/netdrv.c,$(NET_VITA_SRCS)))
# netdrv's compile-time sizing.  It must reach EVERY file that includes
# netdrv.h -- the app's sources and corebuild.mk's FE_FLAGS -- or two objects
# disagree on sizeof(netdrv) without a link error (net.mk has the numbers).
ND_DEFS     := $(filter -D%,$(NET_VITA_CFLAGS))

APP_ALL_VITA := $(addprefix $(V)/,$(APP_SRCS) $(APP_SRCS_VITA)) \
                $(UI_SRCS) $(UI_GXM_SRCS) $(NET_SRCS)
APP_ALL_HOST := $(addprefix $(V)/,$(APP_SRCS) $(APP_SRCS_HOST) vback_soft.c \
                  vback_soft_prog.cpp) \
                $(UI_SRCS)

APP_INC_FLAGS := $(VITA_UI_CFLAGS) $(VITA_UI_ASFLAGS) -I$(V) \
                 $(addprefix -I$(V)/,$(APP_INCDIRS))

# ---- one object per source, under $(B)/app ------------------------------------
# "../../psp/ui_psp.c" -> $(B)/app/psp/ui_psp.o
app_obj = $(B)/app/$(subst ../,,$(patsubst ./%,%,$(basename $(1)))).o

# Third-party code is not ours to lint; the PSP UI is compiled with the
# warnings its own Makefile uses.
APP_QUIET := %/stb_impl.c %/stb_impl_vita.c
UI_WARN   := -Wno-unused-parameter -Wno-missing-field-initializers \
             -Wno-format-truncation
# xBRZ (third_party/xbrz) is C++: no exceptions, no RTTI, nothing of
# libstdc++ at link time (it needs none), and not ours to lint either.
APP_CXX_FLAGS := -x c++ -std=gnu++14 -O3 -fno-math-errno -fno-exceptions -fno-rtti -w
app_file_flags = $(if $(filter %.cpp,$(1)),$(APP_CXX_FLAGS),$(if $(or $(filter $(APP_QUIET),$(1)),$(call in_vita2d,$(1)),$(call in_rcheevos,$(1))),-w,$(if $(filter $(UI_SRCS) $(UI_GXM_SRCS),$(1)),$(UI_WARN))))

# The includer defines CC and APP_FLAGS, then:
#     $(foreach s,$(APP_ALL_xxx),$(eval $(call APP_RULE,$(s))))
# APP_FILE_FLAGS: empty; a target-specific value adjusts one object.
define APP_RULE
$(call app_obj,$(1)): $(1) $$(APP_STAMP)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(APP_FLAGS) $(call app_file_flags,$(1)) $$(APP_FILE_FLAGS) -c -o $$@ $$<
endef
