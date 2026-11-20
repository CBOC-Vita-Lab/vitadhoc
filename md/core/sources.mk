# Genesis Plus GX translation units that md.bin builds (paths relative to md/core). The libretro layer, the NTSC filters,
# libchdr, zstd, tremor and minimp3 are not vendored (see PROVENANCE.md). Copyright CBOC-Vita. MIT licence for this file.
CORE_SRCS := \
	crc32.c rng.c genesis.c io_ctrl.c loadrom.c mem68k.c membnk.c memz80.c state.c system.c vdp_ctrl.c vdp_render.c vdp_render_w.c vdp_thread.c \
	m68k/m68kcpu.c m68k/s68kcpu.c \
	z80/z80.c \
	sound/blip_buf.c sound/eq.c sound/opll.c sound/psg.c sound/sound.c sound/ym2413.c sound/ym2612.c sound/ym3438.c \
	cart_hw/areplay.c cart_hw/eeprom_93c.c cart_hw/eeprom_i2c.c cart_hw/eeprom_spi.c cart_hw/flash_cfi.c \
	cart_hw/ggenie.c cart_hw/md_cart.c cart_hw/megasd.c cart_hw/yx5200.c cart_hw/sms_cart.c cart_hw/sram.c \
	cart_hw/svp/ssp16.c cart_hw/svp/svp.c \
	input_hw/activator.c input_hw/gamepad.c input_hw/graphic_board.c input_hw/input.c input_hw/lightgun.c \
	input_hw/mouse.c input_hw/paddle.c input_hw/smash.c input_hw/sportspad.c input_hw/teamplayer.c \
	input_hw/terebi_oekaki.c input_hw/xe_1ap.c \
	cd_hw/cd_cart.c cd_hw/cdc.c cd_hw/cdd.c cd_hw/gfx.c cd_hw/pcm.c cd_hw/scd.c
