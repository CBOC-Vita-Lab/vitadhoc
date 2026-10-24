/* GBAdhoc: validate pointer/index-bearing fields before restoring ANY RAM.
 * Included only by gb.c. Existing state lengths and legacy files are kept.
 * SPDX-License-Identifier: GPL-2.0-or-later */
static int state_int(const byte *p)
{
	int v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static int state_apu_valid(const byte *p)
{
	struct apu_stat a;
	memcpy(&a, p, sizeof(a));
#define BETWEEN(v, hi) ((unsigned)(v) <= (unsigned)(hi))
#define CHANNEL(ch) (BETWEEN(a.ch##_len, 64) && BETWEEN(a.ch##_init_len, 64) && \
	BETWEEN(a.ch##_vol, 15) && BETWEEN(a.ch##_init_vol, 15) && \
	BETWEEN(a.ch##_env_dir, 1) && BETWEEN(a.ch##_env_speed, 7) && BETWEEN(a.ch##_hold, 1))
	if (!CHANNEL(sq1) || !CHANNEL(sq2) || !CHANNEL(noi) ||
	    !BETWEEN(a.sq1_type, 3) || !BETWEEN(a.sq2_type, 3) ||
	    !BETWEEN(a.sq1_sw_time, 7) || !BETWEEN(a.sq1_sw_dir, 1) ||
	    !BETWEEN(a.sq1_sw_shift, 7) || !BETWEEN(a.wav_vol, 4) ||
	    !BETWEEN(a.wav_len, 256) || !BETWEEN(a.wav_init_len, 256) ||
	    !BETWEEN(a.wav_hold, 1) || !BETWEEN(a.wav_enable, 1) ||
	    !BETWEEN(a.noi_freq, 0x100000) ||
	    !BETWEEN(a.master_vol[0], 7) || !BETWEEN(a.master_vol[1], 7)) return 0;
#undef CHANNEL
#undef BETWEEN
	return 1;
}

static int gb_state_valid(const byte *buf, size_t size)
{
	static const int ram_banks[] = {1,1,1,4,16,8};
	const byte *p;
	int model, banks, page, sram;
	/* gb_save_state uses the current cartridge/model, already matched by the
	 * adapter. Reject a direct call with another layout too. */
	if (size != gb_save_state(NULL) || size < 4 ||
	    (model = state_int(buf)) != rom_get_info()->gb_type) return 0;
	p = buf + 4 + (model <= 2 ? 0x4000 : 0xc000) +
		ram_banks[rom_get_info()->ram_size] * 0x2000 + 0xa0 + 0x80;
	page = state_int(p); sram = state_int(p + 4); p += 8;
	banks = 2 << rom_get_info()->rom_size;
	if (banks < 64) banks = 64; /* adapter's minimum ROM allocation is 1 MiB */
	/* ROM pointer is biased by -0x4000: page -1 means bank zero at 4000. */
	if (page < -1 || page >= banks - 1 || sram < 0 || sram > 15) return 0;
	if (model >= 3) {
		if ((unsigned)state_int(p) > 7 || (unsigned)state_int(p + 4) > 1) return 0;
		p += 8;
	}
	p += sizeof(struct cpu_regs) + sizeof(struct gb_regs);
	if (model >= 3) p += sizeof(struct gbc_regs) + sizeof(word) * 64;
	p += 4 * sizeof(int); /* halt, old serial field, mapper, RAM-enable */
	if (model >= 3) p += 6 * sizeof(int);
	if (!state_apu_valid(p) || !state_apu_valid(p + sizeof(struct apu_stat) + 0x30)) return 0;
	if (!cpu_timer_timing_valid(p + 2 * sizeof(struct apu_stat) + 0x30 +
	    (model >= 3 ? 1 : 0) + 80)) return 0;
	p += 2 * sizeof(struct apu_stat) + 0x30 + 256 + (model >= 3 ? 1 : 0);
	if (model == 2) {
		/* A corrupt packet cursor otherwise indexes outside sgb_buffer on the
		 * very next joypad write. These are the parser's reachable ranges. */
		int bit = state_int(p + 4), bits = state_int(p + 8);
		int packets = state_int(p + 12), parser = state_int(p + 16);
		int index = state_int(p + 20), mask = state_int(p + 40);
		if ((bit != 0 && bit != 128) || (unsigned)bits > 128 ||
		    (unsigned)packets > 6 || (unsigned)parser > 2 ||
		    (unsigned)index > 16 || (unsigned)mask > 3) return 0;
		p += 44 + sizeof(word) * (128 + 2048) + 7 * 16;
		for (unsigned i = 0; i < 18 * 20 + 45 * 20 * 18; i++)
			if (p[i] > 3) return 0; /* palette number used by lcd_sgb_render */
	}
	return 1;
}
