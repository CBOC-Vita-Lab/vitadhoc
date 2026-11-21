/* VitAdhoc: the VDP worker (see vdp_thread.h for the design). Copyright CBOC-Vita; Genesis Plus GX licence (md/core/LICENSE.txt). */
#define VDPW_NO_WRAP
#include "shared.h"
#include "vdp_thread.h"

/* ---- the worker's copy of the VDP state the renderer reads (vdp_render_w.c names them vw_*) ------------------------------------------- */
uint8 ALIGNED_(4) vw_vram[0x10000];
uint8 ALIGNED_(4) vw_sat[0x400];
uint8 ALIGNED_(4) vw_vsram[0x80];
uint8 vw_reg[0x20];
uint16 vw_status;
uint16 vw_ntab, vw_ntbb, vw_ntwb, vw_satb, vw_hscb;
uint8 vw_bg_name_dirty[0x800];
uint16 vw_bg_name_list[0x800];
uint16 vw_bg_list_index;
uint8 vw_hscroll_mask, vw_playfield_shift, vw_playfield_col_mask, vw_odd_frame, vw_interlaced;
uint16 vw_playfield_row_mask, vw_v_counter, vw_vscroll, vw_lines_per_frame, vw_max_sprite_pixels;
t_bitmap vw_bitmap;
extern int vw_wide_m;                                 /* the wide view strip width of the worker's renderer (defined by its copy of vdp_render.c) */
extern wide_line_t vw_wide_rec[256];
extern int vw_wide_planes;
extern uint32 vw_wide_opq[256][2][3];
extern uint16 vw_wide_pal[32];
extern void vw_wide_rows_clear(void);
extern void vw_wide_row_set(int plane, int row, unsigned int addr, const uint16 *words, int n);
extern uint32 vw_wide_gen, vw_wide_parse_gen;      /* the strip cache of the worker's renderer: bumped by every record that changes what a strip is drawn from */
extern int vw_wide_cache_on, vw_wide_cache_check;
extern unsigned long vw_wide_cache_stat[3];

/* the worker's compiled copy of the renderer */
extern void vw_render_line(int line);
extern void vw_remap_line(int line);
extern void vw_blank_line(int line, int offset, int width);
extern void (*vw_parse_satb)(int line);
extern void vw_window_clip(unsigned int data, unsigned int sw);
extern void vw_color_update_m4(int index, unsigned int data);
extern void vw_color_update_m5(int index, unsigned int data);
extern void vw_render_init(void);
extern void vw_render_reset(void);
extern unsigned vw_vdp_fn_encode(void);
extern void vw_vdp_fn_decode(unsigned c);
extern uint16 vw_spr_col;
extern uint8 vw_tms_status_set;

/* the emulating thread's renderer (for the function pointer codes) */
extern unsigned vdp_fn_encode(void);

extern int8 reset_do_not_clear_buffers;

int vdpw_on;
uint8 vdp_status_only;

/* ---- the queue ----------------------------------------------------------------------------------------------------------------------- */
enum {
	OP_STATE = 1, OP_PUSH, OP_VBLK, OP_VALL, OP_SAT, OP_VSRAM, OP_VMARK, OP_RENDER, OP_REMAP, OP_BLANK, OP_PARSE, OP_WCLIP, OP_COLOR,
	OP_INIT, OP_RESET, OP_STOP, OP_CALL
};

/* what the renderer reads of the machine besides the raw arrays; sent whenever it differs from the last one sent */
typedef struct {
	uint8 reg[0x20];
	uint8 *bm_data;
	int bm_pitch, bm_width, bm_height;
	int vx, vy, vw, vh;
	unsigned fn;
	uint32 rst;                       /* reset_do_not_clear_buffers */
	uint16 ntab, ntbb, ntwb, satb, hscb, playfield_row_mask, v_counter, vscroll, lines_per_frame, max_sprite_pixels;
	uint8 hscroll_mask, playfield_shift, playfield_col_mask, odd_frame, interlaced, pad0[3];
	int wide_m, wide_planes;
} vdpw_state_t;
#define STATE_WORDS ((sizeof(vdpw_state_t) + 3) / 4)

#define QCAP_WORDS (1u << 17)             /* 512 KB; the largest record is the whole VRAM (16 K words) */
#define WAKE_EVERY 4                       /* lines between wake-ups of a sleeping worker */

typedef struct {
	const md_thread_ops *ops;
	void *sem;
	uint32 *ring;
	unsigned mask;
	unsigned head;                         /* published by the producer */
	unsigned tail;                         /* published by the worker after it has done a record */
	int waiting;                           /* the worker sleeps on the semaphore */
	/* producer private */
	unsigned ph, tail_seen;
	int since_wake;
	vdpw_state_t last_state;
	int have_state;
	uint8 sat_shadow[0x400], vsram_shadow[0x80];
	int shadow_valid;
	uint8 vdirty[0x800];
	uint16 vlist[0x800];
	int vn, vall;
	int push_needed, merge_needed;
	int check, stress, started;
	unsigned long check_errors;
	vdpw_census_t cen;
	unsigned long long vdp_busy;   /* written by the worker only, read by the emulating thread in the census (a stale value is fine) */
} VW;
static VW W;

#define LOADA(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define STOREA(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

/* ---- producer ------------------------------------------------------------------------------------------------------------------------ */

static void publish(int wake)
{
	__atomic_store_n(&W.head, W.ph, __ATOMIC_SEQ_CST);
	if (wake || W.since_wake >= WAKE_EVERY) {
		W.since_wake = 0;
		if (__atomic_exchange_n(&W.waiting, 0, __ATOMIC_SEQ_CST)) { W.ops->sem_post(W.ops->user, W.sem); W.cen.wakes++; }
	}
}

static void reserve(unsigned n)
{
	if (W.ph + n - W.tail_seen <= QCAP_WORDS) return;
	for (;;) {
		W.tail_seen = LOADA(W.tail);
		if (W.ph + n - W.tail_seen <= QCAP_WORDS) return;
		W.cen.full_waits++;
		publish(1);
		W.ops->yield(W.ops->user);
	}
}

static inline void put(uint32 w) { W.ring[W.ph++ & W.mask] = w; }
static void put_block(const void *src, unsigned words)
{
	const uint32 *s = (const uint32 *) src;
	unsigned i;
	for (i = 0; i < words; i++) put(s[i]);
}

static void fill_state(vdpw_state_t *s)
{
	memset(s, 0, sizeof *s);
	memcpy(s->reg, reg, sizeof reg);
	s->bm_data = bitmap.data; s->bm_pitch = bitmap.pitch; s->bm_width = bitmap.width; s->bm_height = bitmap.height;
	s->vx = bitmap.viewport.x; s->vy = bitmap.viewport.y; s->vw = bitmap.viewport.w; s->vh = bitmap.viewport.h;
	s->fn = vdp_fn_encode();
	s->rst = (uint32) reset_do_not_clear_buffers;
	s->ntab = ntab; s->ntbb = ntbb; s->ntwb = ntwb; s->satb = satb; s->hscb = hscb; s->playfield_row_mask = playfield_row_mask;
	s->v_counter = v_counter; s->vscroll = vscroll; s->lines_per_frame = lines_per_frame; s->max_sprite_pixels = max_sprite_pixels;
	s->hscroll_mask = hscroll_mask; s->playfield_shift = playfield_shift; s->playfield_col_mask = playfield_col_mask;
	s->odd_frame = odd_frame; s->interlaced = interlaced;
	s->wide_m = wide_m; s->wide_planes = wide_planes;
}

static void emit_state(void)
{
	vdpw_state_t s;
	fill_state(&s);
	if (W.have_state && !memcmp(&s, &W.last_state, sizeof s)) return;
	W.last_state = s; W.have_state = 1;
	reserve(1 + STATE_WORDS);
	put(OP_STATE);
	put_block(&s, STATE_WORDS);
}

static void emit_push(void)
{
	if (!W.push_needed) return;
	W.push_needed = 0;
	reserve(2);
	put(OP_PUSH); put((uint32) (status & 0x7F));
}

static void emit_vram(void)
{
	int i;
	if (W.vall) {
		reserve(1 + 0x4000);
		put(OP_VALL);
		put_block(vram, 0x4000);
		W.cen.vram_alls++;
		for (i = 0; i < W.vn; i++) W.vdirty[W.vlist[i]] = 0;
		W.vn = 0; W.vall = 0;
		return;
	}
	for (i = 0; i < W.vn; i++) {
		const unsigned name = W.vlist[i];
		W.vdirty[name] = 0;
		reserve(2 + 8);
		put(OP_VBLK); put(name);
		put_block(vram + (name << 5), 8);
	}
	W.cen.vram_blocks += (unsigned long) W.vn;
	W.vn = 0;
}

/* the sprite attribute copy and the vertical scroll RAM: small, compared with what the worker holds and sent in 32 byte pieces */
static void emit_small(void)
{
	unsigned i;
	for (i = 0; i < sizeof sat / 32; i++)
		if (!W.shadow_valid || memcmp(sat + i * 32, W.sat_shadow + i * 32, 32)) {
			memcpy(W.sat_shadow + i * 32, sat + i * 32, 32);
			reserve(2 + 8); put(OP_SAT); put(i); put_block(sat + i * 32, 8);
		}
	for (i = 0; i < sizeof vsram / 32; i++)
		if (!W.shadow_valid || memcmp(vsram + i * 32, W.vsram_shadow + i * 32, 32)) {
			memcpy(W.vsram_shadow + i * 32, vsram + i * 32, 32);
			reserve(2 + 8); put(OP_VSRAM); put(i); put_block(vsram + i * 32, 8);
		}
	W.shadow_valid = 1;
}

/* the state and the memory the next drawing call reads */
static void emit_world(void)
{
	emit_push();
	emit_state();
	emit_vram();
	emit_small();
}

void vdpw_mark_vram(unsigned name)
{
	name &= 0x7FF;
	if (!W.vdirty[name]) { W.vdirty[name] = 1; W.vlist[W.vn++] = (uint16) name; }
}

void vdpw_vram_all_changed(void) { W.vall = 1; }
void vdpw_status_changed(void) { if (vdpw_on) W.push_needed = 1; }

void vdpw_q_render_line(int line)
{
	emit_world();
	/* what the single thread's render_line did first: the pattern cache update of the patterns written since. The patterns (and which of their
	 * eight lines) go to the worker, whose render_line then does the update on its copy; the emulating thread keeps the dirty list as it was. */
	if ((reg[1] & 0x40) && bg_list_index) {
		unsigned i;
		for (i = 0; i < bg_list_index; i++) {
			const unsigned name = bg_name_list[i];
			reserve(3);
			put(OP_VMARK); put(name); put(bg_name_dirty[name]);
			bg_name_dirty[name] = 0;
		}
		bg_list_index = 0;
	}
	reserve(2);
	put(OP_RENDER); put((uint32) line);
	W.since_wake++; W.merge_needed = 1; W.cen.records++;
	publish(0);
	if (W.stress) vdpw_join_status();
}

void vdpw_q_remap_line(int line)
{
	emit_push(); emit_state();
	reserve(2); put(OP_REMAP); put((uint32) line);
	W.since_wake++;
	publish(0);
}

void vdpw_q_blank_line(int line, int offset, int width)
{
	emit_push(); emit_state();
	reserve(4); put(OP_BLANK); put((uint32) line); put((uint32) offset); put((uint32) width);
	W.since_wake++;
	publish(0);
}

void vdpw_q_parse_satb(int line)
{
	emit_world();
	reserve(2); put(OP_PARSE); put((uint32) line);
	W.merge_needed = 1;
	publish(0);
}

void vdpw_q_window_clip(unsigned data, unsigned sw)
{
	emit_state();
	reserve(3); put(OP_WCLIP); put(data); put(sw);
}

void vdpw_q_color(int m5, int index, unsigned data)
{
	emit_state();
	reserve(4); put(OP_COLOR); put((uint32) m5); put((uint32) index); put(data);
}

static void wait_idle(void);

/* run fn(arg) on the worker, after everything queued so far and with the worker's copy of the VDP brought level with the emulating thread's
 * (the wide view's analysis of the frame just finished: it reads only the worker's copy while the emulating thread goes on) */
void vdpw_q_call(void (*fn)(void *), void *arg)
{
	uint32 w[4] = { 0, 0, 0, 0 };
	emit_world();
	memcpy(&w[0], &fn, sizeof fn);
	memcpy(&w[2], &arg, sizeof arg);
	reserve(5); put(OP_CALL); put(w[0]); put(w[1]); put(w[2]); put(w[3]);
	publish(1);
}

void vdpw_q_render_init(void)
{
	reserve(1); put(OP_INIT);
	wait_idle();
}

void vdpw_q_render_reset(void)
{
	emit_state();
	reserve(1); put(OP_RESET);
	W.merge_needed = 1;
	wait_idle();   /* the worker reads reset_do_not_clear_buffers, which the emulating thread may change next */
}

static void wait_idle(void)
{
	unsigned spins = 0;
	const unsigned long long t0 = W.ops->now_us ? W.ops->now_us(W.ops->user) : 0;
	publish(1);
	while (LOADA(W.tail) != W.ph) {
		W.cen.join_wait_polls++;
		if (++spins > 64) W.ops->yield(W.ops->user);
	}
	W.tail_seen = W.ph;
	if (t0) W.cen.join_wait_us += (unsigned long) (W.ops->now_us(W.ops->user) - t0);
}

static void check_state(void)
{
	unsigned long e = 0;
	if (memcmp(vram, vw_vram, sizeof vram)) e++;
	if (memcmp(sat, vw_sat, sizeof sat)) e++;
	if (memcmp(vsram, vw_vsram, sizeof vsram)) e++;
	if (memcmp(reg, vw_reg, sizeof reg)) e++;
	if (e) { W.check_errors += e; fprintf(stderr, "vdpw check: %lu raw state difference(s) (vram %d sat %d vsram %d reg %d)\n", e,
	    memcmp(vram, vw_vram, sizeof vram) != 0, memcmp(sat, vw_sat, sizeof sat) != 0, memcmp(vsram, vw_vsram, sizeof vsram) != 0, memcmp(reg, vw_reg, sizeof reg) != 0); }
}

void vdpw_join(void)
{
	if (!vdpw_on) return;
	W.cen.joins++;
	wait_idle();
}

void vdpw_join_status(void)
{
	if (!vdpw_on) return;
	if (!W.merge_needed && LOADA(W.tail) == W.ph) return;
	W.cen.status_joins++;
	wait_idle();
	if (W.merge_needed) {
		W.merge_needed = 0;
		status |= (uint16) (vw_status & 0x60);
		if (vw_tms_status_set) { vw_tms_status_set = 0; status = (uint16) ((status & ~0x1F) | (vw_status & 0x1F)); }
		W.push_needed = 1;
	}
}

void vdpw_frame_end(void)
{
	if (!vdpw_on) return;
	W.cen.joins++;
	wait_idle();
	if (W.merge_needed) {
		W.merge_needed = 0;
		status |= (uint16) (vw_status & 0x60);
		if (vw_tms_status_set) { vw_tms_status_set = 0; status = (uint16) ((status & ~0x1F) | (vw_status & 0x1F)); }
		W.push_needed = 1;
	}
	if (W.check) {
		/* the data the worker has is what the last render call saw; bring it level with the emulating thread's and compare */
		emit_world(); wait_idle();
		check_state();
	}
}

/* the per line records of the wide view strip of the frame just finished (valid after vdpw_frame_end) */
const wide_line_t *vdpw_wide_rec(void) { return vdpw_on ? vw_wide_rec : wide_rec; }
const uint32 *vdpw_wide_opq(void) { return vdpw_on ? &vw_wide_opq[0][0][0] : &wide_opq[0][0][0]; }
/* the renderer's copy of the VDP memory (the worker's own while it runs: only its thread, or a joined emulating thread, may read it) */
const uint8 *vdpw_r_vram(void) { return vdpw_on ? vw_vram : vram; }
const uint8 *vdpw_r_vsram(void) { return vdpw_on ? vw_vsram : vsram; }
const uint8 *vdpw_r_reg(void) { return vdpw_on ? vw_reg : reg; }
const uint16 *vdpw_wide_pal(void) { return vdpw_on ? vw_wide_pal : wide_pal; }
void vdpw_wide_rows_clear(void) { if (vdpw_on) vw_wide_rows_clear(); else wide_rows_clear(); }
void vdpw_wide_row_set(int plane, int row, unsigned int addr, const uint16 *words, int n) { if (vdpw_on) vw_wide_row_set(plane, row, addr, words, n); else wide_row_set(plane, row, addr, words, n); }
void vdpw_wide_cache(int on, int check) { vw_wide_cache_on = on; vw_wide_cache_check = check; }
void vdpw_wide_cache_stat(unsigned long out[3]) { out[0] = vw_wide_cache_stat[0]; out[1] = vw_wide_cache_stat[1]; out[2] = vw_wide_cache_stat[2]; }

unsigned vdpw_spr_col(void) { return vdpw_on ? vw_spr_col : spr_col; }

/* the renderer's carried state (vdp_render.c, render_carry_t) for the save state and run-ahead: the copy that draws (the worker's while it
 * runs, the emulating thread's otherwise), so that a state is the same bytes whichever thread draws; a load puts it into both copies */
extern int render_carry_size(void);
extern void render_carry_save(void *p);
extern void render_carry_load(void *p);
extern void vw_render_carry_save(void *p);
extern void vw_render_carry_load(void *p);
int vdpw_carry_size(void) { return render_carry_size(); }
void vdpw_carry_save(void *buf)
{
	if (vdpw_on) { vdpw_q_call(vw_render_carry_save, buf); vdpw_join(); }
	else render_carry_save(buf);
}
void vdpw_carry_load(void *buf)
{
	render_carry_load(buf);
	if (vdpw_on) { vdpw_q_call(vw_render_carry_load, buf); vdpw_join(); }
}

void vdpw_census(vdpw_census_t *c) { if (c) { *c = W.cen; c->vdp_us = (unsigned long) __atomic_load_n(&W.vdp_busy, __ATOMIC_RELAXED); } }
void vdpw_set_check(int on) { W.check = on; }
void vdpw_set_stress(int on) { W.stress = on; }
unsigned long vdpw_check_errors(void) { return W.check_errors; }

/* ---- the worker ---------------------------------------------------------------------------------------------------------------------- */

static void apply_state(const vdpw_state_t *s)
{
	/* the strip cache: a state that differs in anything a strip is drawn from is a new generation (the frame buffer, the line counter and, off
	 * interlace, the field are not) */
	{
		static vdpw_state_t prev;
		vdpw_state_t k = *s;
		k.bm_data = 0; k.bm_pitch = 0; k.bm_width = 0; k.bm_height = 0; k.rst = 0; k.v_counter = 0;
		if (!k.interlaced) k.odd_frame = 0;
		if (memcmp(&k, &prev, sizeof k)) { prev = k; vw_wide_gen++; }
	}
	memcpy(vw_reg, s->reg, sizeof vw_reg);
	vw_bitmap.data = s->bm_data; vw_bitmap.pitch = s->bm_pitch; vw_bitmap.width = s->bm_width; vw_bitmap.height = s->bm_height;
	vw_bitmap.viewport.x = s->vx; vw_bitmap.viewport.y = s->vy; vw_bitmap.viewport.w = s->vw; vw_bitmap.viewport.h = s->vh;
	vw_vdp_fn_decode(s->fn);
	vw_ntab = s->ntab; vw_ntbb = s->ntbb; vw_ntwb = s->ntwb; vw_satb = s->satb; vw_hscb = s->hscb; vw_playfield_row_mask = s->playfield_row_mask;
	vw_v_counter = s->v_counter; vw_vscroll = s->vscroll; vw_lines_per_frame = s->lines_per_frame; vw_max_sprite_pixels = s->max_sprite_pixels;
	vw_hscroll_mask = s->hscroll_mask; vw_playfield_shift = s->playfield_shift; vw_playfield_col_mask = s->playfield_col_mask;
	vw_odd_frame = s->odd_frame; vw_interlaced = s->interlaced;
	vw_wide_m = s->wide_m; vw_wide_planes = s->wide_planes;
}

static void worker_main(void *arg)
{
	unsigned tail = 0, head = 0;
	uint32 rec[1 + 8 + 4];
	(void) arg;
	for (;;) {
		while (tail == head) {
			head = LOADA(W.head);
			if (tail != head) break;
			__atomic_store_n(&W.waiting, 1, __ATOMIC_SEQ_CST);
			head = __atomic_load_n(&W.head, __ATOMIC_SEQ_CST);
			if (tail != head) {
				if (__atomic_exchange_n(&W.waiting, 0, __ATOMIC_SEQ_CST) == 0) W.ops->sem_wait(W.ops->user, W.sem);   /* the producer posted meanwhile */
				break;
			}
			W.ops->sem_wait(W.ops->user, W.sem);
			head = LOADA(W.head);
		}
		{ const unsigned long long t0 = W.ops->now_us ? W.ops->now_us(W.ops->user) : 0;
		while (tail != head) {
			const unsigned op = W.ring[tail & W.mask];
			unsigned n = 1, i;
			switch (op) {
			case OP_STATE: {
				vdpw_state_t s;
				uint32 *d = (uint32 *) &s;
				for (i = 0; i < STATE_WORDS; i++) d[i] = W.ring[(tail + 1 + i) & W.mask];
				apply_state(&s);
				n = 1 + STATE_WORDS;
				break;
			}
			case OP_PUSH:
				vw_status = (uint16) ((vw_status & ~0x7F) | (W.ring[(tail + 1) & W.mask] & 0x7F));
				n = 2;
				break;
			case OP_VBLK: {
				const unsigned name = W.ring[(tail + 1) & W.mask] & 0x7FF;
				uint32 *d = (uint32 *) (vw_vram + (name << 5));
				for (i = 0; i < 8; i++) d[i] = W.ring[(tail + 2 + i) & W.mask];
				n = 10;
				vw_wide_gen++;
				break;
			}
			case OP_VALL: {
				uint32 *d = (uint32 *) vw_vram;
				for (i = 0; i < 0x4000; i++) d[i] = W.ring[(tail + 1 + i) & W.mask];
				n = 1 + 0x4000;
				vw_wide_gen++;
				break;
			}
			case OP_SAT: case OP_VSRAM: {
				const unsigned c = W.ring[(tail + 1) & W.mask];
				uint32 *d = (uint32 *) ((op == OP_SAT ? vw_sat : vw_vsram) + c * 32);
				for (i = 0; i < 8; i++) d[i] = W.ring[(tail + 2 + i) & W.mask];
				n = 10;
				vw_wide_gen++;
				break;
			}
			case OP_VMARK: {
				const unsigned name = W.ring[(tail + 1) & W.mask] & 0x7FF;
				const unsigned m = W.ring[(tail + 2) & W.mask] & 0xFF;
				if (vw_bg_name_dirty[name] == 0) vw_bg_name_list[vw_bg_list_index++] = (uint16) name;
				vw_bg_name_dirty[name] |= (uint8) m;
				n = 3;
				vw_wide_gen++;
				break;
			}
			case OP_RENDER: vw_render_line((int) W.ring[(tail + 1) & W.mask]); n = 2; break;
			case OP_REMAP: vw_remap_line((int) W.ring[(tail + 1) & W.mask]); n = 2; break;
			case OP_BLANK: vw_blank_line((int) W.ring[(tail + 1) & W.mask], (int) W.ring[(tail + 2) & W.mask], (int) W.ring[(tail + 3) & W.mask]); n = 4; break;
			case OP_PARSE: vw_parse_satb((int) W.ring[(tail + 1) & W.mask]); vw_wide_parse_gen = vw_wide_gen; n = 2; break;
			case OP_WCLIP: vw_window_clip(W.ring[(tail + 1) & W.mask], W.ring[(tail + 2) & W.mask]); vw_wide_gen++; n = 3; break;
			case OP_COLOR:
				if (W.ring[(tail + 1) & W.mask]) vw_color_update_m5((int) W.ring[(tail + 2) & W.mask], W.ring[(tail + 3) & W.mask]);
				else vw_color_update_m4((int) W.ring[(tail + 2) & W.mask], W.ring[(tail + 3) & W.mask]);
				n = 4;
				vw_wide_gen++;
				break;
			case OP_INIT: vw_render_init(); vw_wide_gen++; n = 1; break;
			case OP_RESET: vw_render_reset(); vw_wide_gen++; n = 1; break;
			case OP_CALL: {
				uint32 w[4];
				void (*fn)(void *) = 0;
				void *arg = 0;
				for (i = 0; i < 4; i++) w[i] = W.ring[(tail + 1 + i) & W.mask];
				memcpy(&fn, &w[0], sizeof fn);
				memcpy(&arg, &w[2], sizeof arg);
				if (fn) fn(arg);
				n = 5;
				break;
			}
			case OP_STOP: STOREA(W.tail, tail + 1); return;
			default: n = 1; break;
			}
			tail += n;
			STOREA(W.tail, tail);
		}
		if (t0) __atomic_store_n(&W.vdp_busy, W.vdp_busy + (W.ops->now_us(W.ops->user) - t0), __ATOMIC_RELAXED); }
	}
	(void) rec;
}

int vdpw_start(const md_thread_ops *ops)
{
	if (vdpw_on || !ops || !ops->spawn || !ops->sem_create) return 1;
	{ const int chk = W.check, str = W.stress; memset(&W, 0, sizeof W); W.check = chk; W.stress = str; }
	W.ops = ops;
	W.ring = (uint32 *) malloc(QCAP_WORDS * sizeof(uint32));
	if (!W.ring) return 2;
	W.mask = QCAP_WORDS - 1;
	W.sem = ops->sem_create(ops->user);
	if (!W.sem) { free(W.ring); return 3; }
	memset(&vw_bitmap, 0, sizeof vw_bitmap);
	W.push_needed = 1;
	W.vall = 1;                                  /* the first drawing call carries the whole VRAM */
	vw_wide_cache_on = 1;
	vdpw_on = 1;
	if (ops->spawn(ops->user, MD_WORKER_VDP, worker_main, NULL)) {
		vdpw_on = 0;
		ops->sem_destroy(ops->user, W.sem);
		free(W.ring);
		return 4;
	}
	W.started = 1;
	return 0;
}

void vdpw_stop(void)
{
	if (!vdpw_on) return;
	wait_idle();
	reserve(1); put(OP_STOP);
	publish(1);
	W.ops->wait_end(W.ops->user, MD_WORKER_VDP);
	vdpw_on = 0;
	W.ops->sem_destroy(W.ops->user, W.sem);
	free(W.ring);
	W.ring = 0;
	W.started = 0;
}
