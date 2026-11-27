# Genesis Plus GX translation units that md.bin builds (paths relative to md/core). The libretro layer, the NTSC filters
# and minimp3 are not vendored; the Sega CD readers (libchdr with lzma, zlib, zstd, dr_flac; tremor) are (see PROVENANCE.md). Copyright CBOC-Vita. MIT licence for this file.
CORE_SRCS := \
	crc32.c rng.c genesis.c io_ctrl.c loadrom.c mem68k.c membnk.c memz80.c state.c system.c vdp_ctrl.c vdp_render.c vdp_render_w.c vdp_thread.c fm_thread.c \
	m68k/m68kcpu.c m68k/s68kcpu.c \
	z80/z80.c \
	sound/blip_buf.c sound/eq.c sound/opll.c sound/psg.c sound/sound.c sound/ym2413.c sound/ym2612.c sound/ym3438.c \
	cart_hw/areplay.c cart_hw/eeprom_93c.c cart_hw/eeprom_i2c.c cart_hw/eeprom_spi.c cart_hw/flash_cfi.c \
	cart_hw/ggenie.c cart_hw/md_cart.c cart_hw/megasd.c cart_hw/yx5200.c cart_hw/sms_cart.c cart_hw/sram.c \
	cart_hw/svp/ssp16.c cart_hw/svp/svp.c \
	input_hw/activator.c input_hw/gamepad.c input_hw/graphic_board.c input_hw/input.c input_hw/lightgun.c \
	input_hw/mouse.c input_hw/paddle.c input_hw/smash.c input_hw/sportspad.c input_hw/teamplayer.c \
	input_hw/terebi_oekaki.c input_hw/xe_1ap.c \
	cd_hw/cd_cart.c cd_hw/cdc.c cd_hw/cdd.c cd_hw/cdio.c cd_hw/gfx.c cd_hw/pcm.c cd_hw/scd.c

# Sega CD disc readers: libchdr (BSD-3) and what it needs (lzma public domain, zlib licence, zstd BSD-3, dr_flac public domain / MIT-0),
# tremor (Ogg Vorbis, BSD). Compiled with the same flags as the core.
CHDR := cd_hw/libchdr
CORE_SRCS += \
	$(CHDR)/src/libchdr_bitstream.c $(CHDR)/src/libchdr_cdrom.c $(CHDR)/src/libchdr_chd.c $(CHDR)/src/libchdr_flac.c $(CHDR)/src/libchdr_huffman.c \
	$(CHDR)/deps/lzma/src/Alloc.c $(CHDR)/deps/lzma/src/Bra86.c $(CHDR)/deps/lzma/src/BraIA64.c $(CHDR)/deps/lzma/src/CpuArch.c \
	$(CHDR)/deps/lzma/src/Delta.c $(CHDR)/deps/lzma/src/LzFind.c $(CHDR)/deps/lzma/src/LzmaDec.c $(CHDR)/deps/lzma/src/LzmaEnc.c $(CHDR)/deps/lzma/src/Sort.c \
	$(CHDR)/deps/zlib/adler32.c $(CHDR)/deps/zlib/inffast.c $(CHDR)/deps/zlib/inflate.c $(CHDR)/deps/zlib/inftrees.c $(CHDR)/deps/zlib/zutil.c \
	$(CHDR)/deps/zstd/common/entropy_common.c $(CHDR)/deps/zstd/common/error_private.c $(CHDR)/deps/zstd/common/fse_decompress.c \
	$(CHDR)/deps/zstd/common/zstd_common.c $(CHDR)/deps/zstd/common/xxhash.c \
	$(CHDR)/deps/zstd/decompress/huf_decompress.c $(CHDR)/deps/zstd/decompress/zstd_ddict.c $(CHDR)/deps/zstd/decompress/zstd_decompress.c \
	$(CHDR)/deps/zstd/decompress/zstd_decompress_block.c \
	sound/tremor/bitwise.c sound/tremor/block.c sound/tremor/codebook.c sound/tremor/floor0.c sound/tremor/floor1.c sound/tremor/framing.c \
	sound/tremor/info.c sound/tremor/mapping0.c sound/tremor/mdct.c sound/tremor/registry.c sound/tremor/res012.c sound/tremor/sharedbook.c \
	sound/tremor/synthesis.c sound/tremor/vorbisfile.c sound/tremor/window.c
