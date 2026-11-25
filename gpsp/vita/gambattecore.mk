# vita/gambattecore.mk -- the Accurate Game Boy core (Gambatte, gbcore/gambatte/) built into the app as an in-process
# core, the way TGB Dual (corebuild.mk), the NES (nescore.mk) and the WonderSwan (wswancore.mk) are.  Included by
# corebuild.mk.  Needs CC CXX LD_R OBJCOPY CORE_ARCH_FLAGS FE_FLAGS FE_INC B GP.
#
#   $(B)/gambatte/gambattecore.o   libgambatte + gbcore/gambatte_host.cpp as ONE relocatable object whose only
#                                  globals are gambatte_host_*
# GAM_OBJS goes on the link line beside GB_OBJS.  GPL-2.0 (libgambatte); see gbcore/gambatte/README.md.
#
# The sources are libgambatte/src's, minus nothing: video_libretro.cpp and cartridge_libretro.cpp are the fork's
# frontend-neutral halves.  -DHAVE_NETWORK is the serial port's SerialIO hook (the Game Boy Printer and the test
# suites' serial text); -DVIDEO_RGB565 the 16-bit picture; -D__LIBRETRO__ the memory accessors the host reads.

GAM_DIR := $(GP)/gbcore/gambatte
GAM_SRC := bootloader cpu gambatte initstate interrupter interruptrequester gambatte-memory sound statesaver tima video \
           video_libretro mem/cartridge mem/cartridge_libretro mem/huc3 mem/memptrs mem/rtc \
           sound/channel1 sound/channel2 sound/channel3 sound/channel4 sound/duty_unit sound/envelope_unit \
           sound/length_counter video/ly_counter video/lyc_irq video/next_m0_time video/ppu video/sprite_mapper
GAM_INC  := -I$(GAM_DIR)/libgambatte/src -I$(GAM_DIR)/libgambatte/include -I$(GAM_DIR)/common -I$(GAM_DIR)/compat
GAM_DEFS := -DHAVE_NETWORK -DVIDEO_RGB565 -D__LIBRETRO__ -DHAVE_STDINT_H -DNDEBUG
GAM_OPT  ?= -O3
GAM_CXXFLAGS := $(CORE_ARCH_FLAGS) $(GAM_OPT) -g -std=gnu++11 -w -fno-exceptions -fno-rtti -fno-threadsafe-statics \
                -fno-strict-aliasing $(GAM_DEFS) $(GAM_INC) -MMD -MP
GAM_CORE_OBJS := $(addprefix $(B)/gambatte/lib/,$(addsuffix .o,$(subst /,_,$(GAM_SRC))))

define GAM_RULE
$$(B)/gambatte/lib/$(subst /,_,$(1)).o: $$(GAM_DIR)/libgambatte/src/$(1).cpp $$(FLAGS_STAMP)
	@mkdir -p $$(dir $$@)
	$$(CXX) $$(GAM_CXXFLAGS) -c -o $$@ $$<
endef
$(foreach s,$(GAM_SRC),$(eval $(call GAM_RULE,$(s))))
$(B)/gambatte/host.o: $(GP)/gbcore/gambatte_host.cpp $(GP)/gbcore/gambatte_host.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CXX) $(GAM_CXXFLAGS) -c -o $@ $<

ifneq ($(HOST_OS),macos)
$(B)/gambatte/gambattecore.o: $(GAM_CORE_OBJS) $(B)/gambatte/host.o
	$(LD_R) -r -o $@.tmp $^
	$(OBJCOPY) -w --keep-global-symbol='gambatte_host_*' $@.tmp $@
	@rm -f $@.tmp
GAM_PRIV := $(B)/gambatte/gambattecore.o
else
# Mach-O: as nescore.mk (tools/build/macho-group.sh, linked whole).
GAM_PRIV := $(B)/gambatte/gambattecore_priv.a
$(GAM_PRIV): $(GAM_CORE_OBJS) $(B)/gambatte/host.o $(MACHO_GROUP)
	sh $(MACHO_GROUP) $(NM) $(OBJCOPY) '$(SYM_US)' $@.d gamp_ '^gambatte_host_' '' '' \
	  $(GAM_CORE_OBJS) $(B)/gambatte/host.o
	@rm -f $@
	$(AR) rcs $@ $@.d/*.o
	@rm -rf $@.d
endif

GAM_OBJS := $(if $(filter macos,$(HOST_OS)),,$(GAM_PRIV))
-include $(shell find $(B)/gambatte -name '*.d' 2>/dev/null)
