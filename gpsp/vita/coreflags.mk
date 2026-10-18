# vita/coreflags.mk -- the GBA core's feature switches for the PS Vita build.
#
# ONE list, read by three builds so they cannot drift apart:
#   gpsp/vita/Makefile            the Vita app (arm-vita-eabi, out of tree)
#   gpsp/vita/hostcore/Makefile   the host twins (x86-64 and armhf/qemu)
#   gpsp/Makefile  platform=vita  the in-tree libretro archive
#
# Every switch the shipping PSP "release" profile sets (tools/build.sh
# CORE_COMMON, docs/BUILD-SWITCHES.md) was checked against the ARM backend
# (arm/arm_emit.h, arm/arm_stub.S) before it was put on either list below.
# A switch whose machinery lives in mips/ is not a no-op to be carried along:
# it is either a compile error or, worse, half a mechanism (C bookkeeping with
# nothing in the stub feeding it).
#
# ---- ON: pure C, the same on every CPU backend --------------------------
#
# GBA_LINK            the two-core link cable (gbalink_core.c).  The point of
#                     the product.  Core hooks in update_gba and serial.c; the
#                     ARM stub's part (the cycle stamp of an I/O write) is the
#                     armlink workstream's.
# GBA_PC_MASK         mask the guest pc to 28 bits in the block lookup, as the
#                     GBA's address bus does.  Without it a garbage pc indexes
#                     memory_map_read[] and def_seq_cycles[] out of bounds.
#                     The ARM stub reaches the lookup only through
#                     block_lookup_address_*, which is where the mask is.
# BADJUMP_SAFE        an unmappable guest pc soft-resets the GBA instead of
#                     being handed to `bx r0` (arm_stub.S jumps to whatever
#                     the lookup returns; ~0 is a prefetch abort on a Vita).
#                     fe_host finishes the reset between frames.
# DMA_SMC_FLUSH       an HBlank/VBlank DMA that lands on translated code
#                     flushes the RAM cache and re-dispatches.  update_gba
#                     reports it as "pc changed" (bit 30), which
#                     arm_update_gba already honours.
# SERIAL_IDLE_FAST    skip update_serial's call chain when no transfer is
#                     pending and there is no link role; exact by
#                     construction (main.c).
#
# ---- OFF: implemented in the MIPS emitter/stub only ---------------------
#
# SMC_GATES, _SIMPLE, _RANKED, SMC_GATE_BITMAP, SMC_GATE_CHARGE
#                     compile, but gates are harvested from
#                     smc_last_write_addr, which only mips_stub.S's smc_write
#                     records (main.c smc_prof_note_store).  The ARM stub
#                     calls flush_translation_cache_ram_smc with no address,
#                     so every gate would be planted at address 0.
# SMC_PARTIAL_SAFE, _STABLE_THUNK, _DIRECT_LINKS, SMC_RETIRE_WINDOW
#                     do not compile for ARM (mips_emit_j, the 16-byte RAM
#                     block prologue, mips_indirect_branch_*).
# DISPATCH_CYCLE_CHECK, DISPATCH_CACHE
#                     the check and the probe are asm in mips_stub.S; the C
#                     side alone would fill a table nothing reads.
# JIT_CODE_DISCIPLINE jit_code.h #errors outside MIPS; it exists for the
#                     Allegrex I-cache.  The Vita publishes code with
#                     sceKernelSyncVMDomain (cpu_threaded.c).
# RUNTIME_JIT_CACHE, SMALL_TRANSLATION_CACHE, LAYOUT_PIN, ROM_BUFFER_SIZE=15,
# USE_PSP_RGB565_FORMAT, the Media Engine renderer, MEMSIZE
#                     the PSP's memory budget, I-cache and GE.  The Vita gets
#                     standard RGB565, a whole resident ROM and its
#                     translation caches from one VM block (vita/jit_vita.c).
# OVERCLOCK_60FPS     upstream's Vita default.  It makes the core emit 1/60 s
#                     of sound per frame; GBAdhoc's frontends pace and
#                     resample themselves (vita/audio_vita.c) and keep the
#                     core's 59.7275 Hz timing identical to the PSP's.
#
#
# ---- the link, as the Vita app runs it -----------------------------------
#
# GBALINK_HALT_BLOCK_EXACT   a halted parent's transfer ends AT its time, not
#                     a cycle later (gbalink_core.c; found with
#                     tools/linktest).  Both consoles of a session run the
#                     same build, so this only has to agree with itself; with
#                     it the pair is hash-identical at every lookahead.
# GBALINK_KEEP_SERIAL_MODE   the cartridge's own serial mode comes back when a
#                     cable session (or the local link) ends: the Vita never
#                     reloads the core around a session, as the PSP does.
# ARM_FAR_STUBS       ON.  The ARM emitter reaches .text through tables, never
#                     PC-relative; this adds the check that the two caches
#                     can reach EACH OTHER and far exits when they cannot.
#                     With both caches in one block (the Vita's arrangement)
#                     the emitted code is what a build without it emits, and
#                     jit_vita.c no longer has to refuse a block the kernel
#                     happened to place far from the program.
GBA_LINK      ?= 1
ARM_FAR_STUBS ?= 1

GBADHOC_CORE_DEFS := -DGBA_PC_MASK=1 -DBADJUMP_SAFE=1 -DDMA_SMC_FLUSH=1 \
                     -DSERIAL_IDLE_FAST=1 -DGBALINK_HALT_BLOCK_EXACT \
                     -DGBALINK_KEEP_SERIAL_MODE
ifeq ($(ARM_FAR_STUBS),1)
GBADHOC_CORE_DEFS += -DARM_FAR_STUBS
endif
