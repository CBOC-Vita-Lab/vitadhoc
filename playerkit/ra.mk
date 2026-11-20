# playerkit: what RetroAchievements in a player is built from (included by the player Makefiles and by playerkit/Makefile).
# Copyright CBOC-Vita. MIT licence (see LICENSE).
#
# LICENCE. Only MIT code: pk_ra (playerkit), the rcheevos client (gpsp/vita/third_party/rcheevos, MIT), and ra_http.c with ra_roots.h
# (owner's files, dual MIT OR GPL-2.0-or-later, taken under the MIT option; the PEM root certificates inside ra_roots.h are the CAs'
# public certificates from the Mozilla root store, not ours, see LICENSING.md and notices/THIRD-PARTY.md). NOT gpsp/vita/ra_vita.c (it needs the app's GPL pieces).
# `make check-links` of the players fails on any object of gpsp/ that is not one of RA_SRCS, and on any RA_SRCS file that is not MIT.
#
# The includer sets before the include: RA_GP (the gpsp/vita directory relative to it), RA_OBJDIR, RA_CC, RA_CFLAGS (without the RA_* ones),
# PK (the playerkit directory relative to it).
RA_RC     = $(RA_GP)/third_party/rcheevos
RA_RCSRC  = $(RA_RC)/src
RA_SRCS   = $(RA_GP)/ra_http.c $(RA_RCSRC)/rc_client.c $(RA_RCSRC)/rc_compat.c $(RA_RCSRC)/rc_util.c $(RA_RCSRC)/rc_version.c \
            $(addprefix $(RA_RCSRC)/rcheevos/,alloc.c condition.c condset.c consoleinfo.c format.c lboard.c memref.c operand.c rc_validate.c \
              richpresence.c runtime.c runtime_progress.c trigger.c value.c) \
            $(addprefix $(RA_RCSRC)/rapi/,rc_api_common.c rc_api_info.c rc_api_runtime.c rc_api_user.c) $(RA_RCSRC)/rhash/md5.c
RA_NAMES  = $(notdir $(RA_SRCS))
RA_OBJS   = $(addprefix $(RA_OBJDIR)/,$(RA_NAMES:.c=.o))
RA_DEFS   = -DRC_NO_THREADS -DPK_RA_PLAYER
RA_INC    = -I$(RA_GP) -I$(RA_RC)/include -I$(RA_RCSRC) -I$(PK)/src
# the files the licence check reads
RA_MIT_FILES = $(RA_GP)/ra_http.c $(RA_GP)/ra_http.h $(RA_GP)/ra_roots.h $(PK)/src/pk_ra.cpp $(PK)/src/pk_ra.h $(PK)/src/pk_ra_plat.h $(RA_RC)/LICENSE

define RA_RULE
$$(RA_OBJDIR)/$(notdir $(1:.c=.o)): $(1) $(wildcard $(RA_GP)/*.h)
	@mkdir -p $$(RA_OBJDIR)
	$$(RA_CC) $$(RA_CFLAGS) $$(RA_DEFS) $$(RA_INC) -c -o $$@ $$<
endef
$(foreach s,$(RA_SRCS),$(eval $(call RA_RULE,$(s))))
