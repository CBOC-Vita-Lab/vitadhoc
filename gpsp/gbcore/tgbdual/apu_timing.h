/* GBAdhoc: per-instance, per-consumer APU timing. Included only by apu.c.
 * The CPU and delayed audio renderer use the same sequencer, never a shared
 * static tick counter. Times are 4 MHz T-cycles, including on double-speed CGB.
 * SPDX-License-Identifier: GPL-2.0-or-later */
struct apu_timing {
	dword phase, step;
	dword envelope[3];
	dword sweep_timer, sweep_shadow, sweep_enabled, sweep_negated;
};
static struct apu_timing cpu_timing, gen_timing;
static struct apu_timing *apu_timer = &cpu_timing;
static int apu_cpu_clock, apu_time, gen_time;
static byte gen_mem[0x30], saved_mem[0x30];
static dword sq1_sample, sq2_sample, wav_index, wav_sample;
static int noi_sample = 10000;
static dword noise_index;

static int sweep_calculate(void)
{
	int delta = apu_timer->sweep_shadow >> snd_stat.sq1_sw_shift;
	int next;
	if (snd_stat.sq1_sw_dir) {
		apu_timer->sweep_negated = 1;
		next = (int)apu_timer->sweep_shadow - delta;
	} else next = (int)apu_timer->sweep_shadow + delta;
	if (next > 2047) snd_stat.sq1_playing = false;
	return next;
}

static void envelope_tick(int *volume, int direction, int pace, dword *timer)
{
	if (*timer && --*timer == 0) {
		*timer = pace ? (unsigned)pace : 8;
		if (pace && ((direction && *volume < 15) || (!direction && *volume > 0)))
			*volume += direction ? 1 : -1;
	}
}

/* GBAdhoc: a 512 Hz DIV-APU edge. Step names describe the next edge. */
void snd_update(void)
{
	if (snd_stat.master_enable) {
		if (!(apu_timer->step & 1)) {
#define LENGTH(ch) if (snd_stat.ch##_hold && snd_stat.ch##_len > 0 && \
			--snd_stat.ch##_len == 0) snd_stat.ch##_playing = false
			LENGTH(sq1); LENGTH(sq2); LENGTH(wav); LENGTH(noi);
#undef LENGTH
		}
		if (apu_timer->step == 2 || apu_timer->step == 6) {
			if (apu_timer->sweep_timer && --apu_timer->sweep_timer == 0) {
				apu_timer->sweep_timer = snd_stat.sq1_sw_time ? snd_stat.sq1_sw_time : 8;
				if (apu_timer->sweep_enabled && snd_stat.sq1_sw_time) {
					int next = sweep_calculate();
					if (next <= 2047 && snd_stat.sq1_sw_shift) {
						apu_timer->sweep_shadow = snd_stat.sq1_freq = next;
						sweep_calculate();
					}
				}
			}
		}
		if (apu_timer->step == 7) {
			envelope_tick(&snd_stat.sq1_vol, snd_stat.sq1_env_dir, snd_stat.sq1_env_speed, &apu_timer->envelope[0]);
			envelope_tick(&snd_stat.sq2_vol, snd_stat.sq2_env_dir, snd_stat.sq2_env_speed, &apu_timer->envelope[1]);
			envelope_tick(&snd_stat.noi_vol, snd_stat.noi_env_dir, snd_stat.noi_env_speed, &apu_timer->envelope[2]);
		}
		apu_timer->step = (apu_timer->step + 1) & 7;
	}
}

static void apu_advance(unsigned cycles)
{
	apu_timer->phase += cycles;
	while (apu_timer->phase >= 8192) {
		apu_timer->phase -= 8192;
		snd_update();
	}
}

void apu_update(void)
{
	int clock = cpu_get_clock();
	int elapsed = clock - apu_cpu_clock;
	apu_cpu_clock = clock;
	if (elapsed > 0) {
		unsigned cycles = (unsigned)elapsed >> (cpu_get_speed() ? 1 : 0);
		apu_time += cycles;
		apu_advance(cycles);
	}
}

static void apu_enqueue(word adr, byte dat)
{
	/* Keep the last slot usable; the old saturating count skipped it. */
	if (snd_que_count < SND_QUE_SIZE) {
		snd_write_que[snd_que_count].adr = adr;
		snd_write_que[snd_que_count].dat = dat;
		snd_write_que[snd_que_count++].clock = apu_time;
	}
}

void apu_div_reset(void)
{
	apu_update();
	apu_enqueue(0xff04, 0);
	if (cpu_timing.phase & 4096) snd_update();
	cpu_timing.phase = 0;
}

void apu_rebase(int cpu_base)
{
	apu_cpu_clock -= cpu_base;
	apu_time = 0;
	snd_bef_clock = 0;
}

void apu_restore_clock(void)
{
	apu_cpu_clock = cpu_get_clock();
	apu_time = 0;
	apu_discard_audio();
}

/* Headless can later become audible: start its next frame at the current
 * register/timer state instead of replaying a stale pre-headless APU. */
void apu_discard_audio(void)
{
	snd_stat_cpy = snd_stat;
	gen_timing = cpu_timing;
	memcpy(gen_mem, snd_mem, sizeof(gen_mem));
	snd_que_count = 0;
	snd_bef_clock = apu_time;
}

static void render_begin(void)
{
	apu_update();
	snd_stat_tmp = snd_stat;
	snd_stat = snd_stat_cpy;
	memcpy(saved_mem, snd_mem, sizeof(saved_mem));
	memcpy(snd_mem, gen_mem, sizeof(gen_mem));
	apu_timer = &gen_timing;
	gen_time = snd_bef_clock;
}

static void render_until(int time, int *cur)
{
	while (*cur < snd_que_count && snd_write_que[*cur].clock <= time) {
		struct apu_que *q = &snd_write_que[(*cur)++];
		if (q->clock > gen_time) apu_advance(q->clock - gen_time);
		gen_time = q->clock;
		snd_process(q->adr, q->dat);
	}
	if (time > gen_time) apu_advance(time - gen_time);
	gen_time = time;
}

static void render_end(int *cur)
{
	render_until(apu_time, cur);
	snd_que_count = 0;
	snd_bef_clock = apu_time;
	snd_stat_cpy = snd_stat;
	snd_stat = snd_stat_tmp;
	memcpy(gen_mem, snd_mem, sizeof(gen_mem));
	memcpy(snd_mem, saved_mem, sizeof(saved_mem));
	apu_timer = &cpu_timing;
}

/* Keep the existing 256 reserved state bytes and state length. Old files
 * contain zeros: their missing timer phase starts at zero deterministically.
 * The explicit little-endian words do not depend on host struct padding. */
static void apu_put32(byte **p, dword v)
{
	int i; for (i = 0; i < 4; i++) *(*p)++ = (byte)(v >> (i * 8));
}
static dword apu_get32(const byte **p)
{
	dword v = 0; int i; for (i = 0; i < 4; i++) v |= (dword)*(*p)++ << (i * 8); return v;
}
void apu_save_cpu_timing(byte *out)
{
	byte *p = out;
	apu_put32(&p, cpu_timing.phase); apu_put32(&p, cpu_timing.step);
	apu_put32(&p, cpu_timing.envelope[0]); apu_put32(&p, cpu_timing.envelope[1]); apu_put32(&p, cpu_timing.envelope[2]);
	apu_put32(&p, cpu_timing.sweep_timer); apu_put32(&p, cpu_timing.sweep_shadow);
	apu_put32(&p, cpu_timing.sweep_enabled); apu_put32(&p, cpu_timing.sweep_negated);
}
void apu_save_timing(byte *out)
{
	byte *p = out + 40;
	memset(out, 0, 256); memcpy(out, "APU1", 4);
	apu_save_cpu_timing(out + 4);
	apu_put32(&p, sq1_cur_pos); apu_put32(&p, sq2_cur_pos);
	apu_put32(&p, wav_cur_pos); apu_put32(&p, noi_cur_pos);
	apu_put32(&p, sq1_sample); apu_put32(&p, sq2_sample);
	apu_put32(&p, wav_index); apu_put32(&p, wav_sample);
	apu_put32(&p, (dword)noi_sample); apu_put32(&p, noise_index);
}
void apu_load_timing(const byte *in)
{
	const byte *p = in + 4;
	memset(&cpu_timing, 0, sizeof(cpu_timing));
	sq1_cur_pos = sq2_cur_pos = wav_cur_pos = noi_cur_pos = 0;
	sq1_sample = sq2_sample = wav_index = wav_sample = noise_index = 0;
	noi_sample = 10000;
	if (!memcmp(in, "APU1", 4)) {
		cpu_timing.phase = apu_get32(&p) & 8191; cpu_timing.step = apu_get32(&p) & 7;
		cpu_timing.envelope[0] = apu_get32(&p) % 9; cpu_timing.envelope[1] = apu_get32(&p) % 9; cpu_timing.envelope[2] = apu_get32(&p) % 9;
		cpu_timing.sweep_timer = apu_get32(&p) % 9; cpu_timing.sweep_shadow = apu_get32(&p) & 2047;
		cpu_timing.sweep_enabled = apu_get32(&p) & 1; cpu_timing.sweep_negated = apu_get32(&p) & 1;
		sq1_cur_pos = apu_get32(&p) & 8191; sq2_cur_pos = apu_get32(&p) & 8191;
		wav_cur_pos = apu_get32(&p) & 65535; noi_cur_pos = apu_get32(&p) % snd_rate;
		sq1_sample = apu_get32(&p) & 7; sq2_sample = apu_get32(&p) & 7;
		wav_index = apu_get32(&p) & 31; wav_sample = apu_get32(&p) & 15;
		noi_sample = (int32_t)apu_get32(&p); noise_index = apu_get32(&p);
		if (noi_sample < -10000 || noi_sample > 12000) noi_sample = 10000;
	}
	apu_timer = &cpu_timing;
	apu_cpu_clock = cpu_get_clock(); apu_time = 0;
	apu_discard_audio();
}
