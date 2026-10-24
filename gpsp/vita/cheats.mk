# cheats.mk -- the cheats part of the Vita build, for gpsp/vita/Makefile (and
# the host twins) to include.  Paths are relative to gpsp/vita/.
#
#     include cheats.mk
#     OBJS      += $(CHEATS_SRCS:.c=.o)        # app objects
#     CFLAGS    += $(CHEATS_DEFS)
#     VPK_FILES += $(CHEATS_VPK_FILES)         # vita-pack-vpk -a src=dst
#
# and at boot:  cheats_db_open("app0:cheats.db")   (see cheats_vita.h).
#
#   CHEATS_SRCS       cheats_vita.c  the list, the .cht file, the cores
#                     cheatdb.c      the bundled database's reader
#   CHEATS_CORE_SRCS  empty.  The decoder (gpsp/cheat_engine.c) is in
#                     gpsp/Makefile.common's SOURCES_C, so corebuild.mk
#                     builds it into BOTH GBA core instances already (and
#                     the app links instance A's copy: cheat_gba_compile...).
#                     gpsp/cheats.c and gbcore/gbcore_tgbdual.c were core
#                     files before.
#   CHEATS_DEFS       empty.  (-DCHEATS_VITA_NO_CORE builds cheats_vita.c
#                     without an emulator, for the list tests only.)
#   CHEATS_VPK        the database, as vita-pack-vpk's "-a" argument
#                     (CHEATS_VPK_FILES is the same with the -a in front)
#
# The app's include path must have gpsp/ (cheat_engine.h, gbcore/gbcore.h)
# and gpsp/frontend-common (fe_console.h): corebuild.mk's FE_INC has both.
#
# The database is committed (assets/cheats.db, 1.4 MiB); to rebuild it from
# a libretro-database checkout:
#     python3 tools/make_cheatdb.py --db <libretro-database> --out assets/cheats.db
# Tests: make -C tests/cheats test   (see tests/cheats/README).

CHEATS_SRCS      := cheats_vita.c cheatdb.c
CHEATS_CORE_SRCS :=
CHEATS_DEFS      :=
CHEATS_VPK       := assets/cheats.db=cheats.db
CHEATS_VPK_FILES := -a $(CHEATS_VPK)

# ---- into the app (app.mk's convention: the VPK and the host twin) -----------
# The same two files on the Vita and on the twin; the database rides in the
# package as app0:cheats.db (the twin reads it where it is, assets/).
APP_SRCS  += $(CHEATS_SRCS)
APP_DEFS  += $(CHEATS_DEFS)
VPK_EXTRA += $(CHEATS_VPK)
