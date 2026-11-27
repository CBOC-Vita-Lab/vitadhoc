/* Copyright (c) 2026 CBOC-Vita contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Gambatte as the app's Accurate Game Boy core: see gambatte_host.h.  One machine at a time (the app never runs two
 * of this core: a cable session is TGB Dual's). */
#include "gambatte_host.h"
#include "gambatte.h"
#include "serial_io.h"

#include <stdlib.h>
#include <string.h>
#include <new>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define GAM_AUDIO_NEON 1
#else
#define GAM_AUDIO_NEON 0
#endif

/* the cartridge's rumble motor: no motor on this build */
void cartridge_set_rumble(unsigned) {}

/* at file scope: inside the anonymous namespace these would get internal names */
extern "C" {
extern const unsigned char tgbshared_cgb_lcd_curve[32];
extern const unsigned char tgbshared_cgb_lcd_green[32 * 32];
}

namespace {

enum {
  VW = 160, VH = 144, VPITCH = 160,
  SOUND_FRAME = 35112,                 /* stereo samples a frame at 2 MiHz */
  SOUND_RUN = 2064,                    /* what one runFor asks for */
  SOUND_BUF = SOUND_FRAME + SOUND_RUN,
  RTC_TRAILER = 48,
  STATE_HEAD = 16,
  MAX_DECIM = 128
};
const unsigned kNative = 2097152;     /* the machine's sample rate */
const int64_t kRtcSpan = 512ll * 86400;
const int64_t kRtcEpochMin = 852076800;
const int64_t kRtcUnknown = 0x7fffffff7fffffffll;

const char kMagicGam[4] = { 'G', 'B', 'G', 'M' };
const char kMagicTgb[4] = { 'G', 'B', 'A', 'T' };

/* the frontend's buttons (gbcore.h GBCORE_BUTTON_*) to Gambatte's */
unsigned pad_map(uint16_t b)
{
  unsigned k = 0;
  if (b & (1u << 0)) k |= gambatte::InputGetter::RIGHT;
  if (b & (1u << 1)) k |= gambatte::InputGetter::LEFT;
  if (b & (1u << 2)) k |= gambatte::InputGetter::UP;
  if (b & (1u << 3)) k |= gambatte::InputGetter::DOWN;
  if (b & (1u << 4)) k |= gambatte::InputGetter::A;
  if (b & (1u << 5)) k |= gambatte::InputGetter::B;
  if (b & (1u << 6)) k |= gambatte::InputGetter::SELECT;
  if (b & (1u << 7)) k |= gambatte::InputGetter::START;
  return k;
}

struct Input : gambatte::InputGetter {
  unsigned keys;
  Input() : keys(0) {}
  virtual unsigned operator()() { return keys; }
};

struct Serial : gambatte::SerialIO {
  gambatte_host_cb_t cb;
  Serial() { memset(&cb, 0, sizeof cb); }
  virtual bool check(unsigned char, unsigned char &, bool &) { return false; }
  virtual unsigned char send(unsigned char data, bool)
  {
    uint8_t in = 0xFF;
    if (cb.serial_transfer && cb.serial_transfer(cb.userdata, data, 1, &in))
      return in;
    return 0xFF;
  }
};

struct Machine {
  gambatte::GB gb;
  Input input;
  Serial serial;
  gambatte_host_cb_t cb;
  int cgb_console, cgb;                       /* asked for / running as */
  unsigned decim, rate;                       /* output sample rate, input samples per output sample */
  unsigned decim_pow2, decim_shift;           /* precomputed exact divisor for (1024 * decim * decim) */
  uint32_t decim_mul;                         /* Granlund-Montgomery low 32-bit reciprocal for non-pow2 decim */
  uint8_t header[0x50];                       /* 0x100..0x14F */
  uint32_t cart_id;
  uint64_t frames;
  int skip, quiet;
  size_t rom_bytes;
  /* the triangle filter (two box filters in a row, so two running sums per channel instead of 2*decim-1 taps):
   * over a block of decim samples x_j, U = sum j*x_j and V = sum x_j; an output is U(previous block) + decim*V - U */
  int32_t u[2], v[2], pu[2];                  /* this block's sums, the last block's U (L, R) */
  unsigned since;                             /* samples into this block */
  int16_t out[2 * (SOUND_FRAME / 16 + 8)];
  size_t out_n;
  /* cheats */
  gambatte_host_cheat_t ram_cheat[512];
  unsigned ram_cheat_n;
  struct Undo { uint32_t offset; uint8_t old; } undo[4096];
  unsigned undo_n;
  uint16_t video[VPITCH * VH];
  uint32_t sound[SOUND_BUF];
  Machine() : cgb_console(0), cgb(0), decim(64), rate(32768), decim_pow2(1), decim_shift(22), decim_mul(0),
              cart_id(0), frames(0), skip(0), quiet(0), rom_bytes(0),
              since(0), out_n(0), ram_cheat_n(0), undo_n(0)
  {
    memset(&cb, 0, sizeof cb);
    memset(header, 0, sizeof header);
    memset(u, 0, sizeof u); memset(v, 0, sizeof v); memset(pu, 0, sizeof pu);
    memset(out, 0, sizeof out);
    memset(video, 0xFF, sizeof video);
    memset(sound, 0, sizeof sound);
  }
};

Machine *g_m;
uint16_t g_dmg[12] = { 0xFFFF, 0xAD55, 0x52AA, 0x0000, 0xFFFF, 0xAD55, 0x52AA, 0x0000, 0xFFFF, 0xAD55, 0x52AA, 0x0000 };
int g_cc = 1;
time_t (*g_wall)(void);
int64_t g_wall_last;
int g_wall_shift;                            /* the clock was started before the first reading */

#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* ---- the Game Boy Color LCD model: the same tables the other core draws with (tgbdual/_cgb_lcd_table.h) ---- */

}  // namespace

extern "C" uint16_t gambatte_gbc_pixel(unsigned c)
{
  const unsigned r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
  if (g_cc)
    return RGB565(tgbshared_cgb_lcd_curve[r], tgbshared_cgb_lcd_green[(g << 5) | b], tgbshared_cgb_lcd_curve[b]);
  return RGB565((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2));
}

/* The wall clock for the cartridge clocks: UTC seconds, the last good reading while there is none */
extern "C" uint64_t gambatte_wall_time(void)
{
  int64_t n = (int64_t)(g_wall ? g_wall() : time(NULL));
  if (n > 0)
    g_wall_last = n;
  return (uint64_t)g_wall_last;
}

namespace {

uint32_t fnv(const uint8_t *p, size_t n, uint32_t h)
{
  while (n--)
    h = (h ^ *p++) * 16777619u;
  return h;
}

int has_clock(const Machine *m)
{
  const unsigned t = m->header[0x47];
  return t == 0x0F || t == 0x10;
}

int has_battery(const Machine *m)
{
  switch (m->header[0x47]) {
    case 0x03: case 0x06: case 0x09: case 0x0F: case 0x10: case 0x13: case 0x1B: case 0x1E: case 0xFE: case 0xFF:
      return 1;
  }
  return 0;
}

/* Cartridge RAM as TGB Dual and SameBoy before it sized it (gbcore_tgbdual.c gbcore_cart_ram_size): the battery file
 * is the same file for both cores.  Gambatte keeps more for some headers (a whole bank for MBC2); the rest is not
 * saved. */
size_t cart_ram_size(const Machine *m)
{
  static const size_t sizes[6] = { 0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000 };
  unsigned n = 0;
  size_t want;
  const unsigned t = m->header[0x47], r = m->header[0x49];
  if (t == 0x05 || t == 0x06)
    want = 0x200;
  else
    want = r <= 5 ? sizes[r] : 0;
  m->gb.gbadhocCartRam(&n);
  return want < n ? want : n;
}

/* the live clock registers for a count of seconds */
void rtc_regs(int64_t c, unsigned halted, unsigned carry, uint32_t out[5])
{
  const int64_t days = c / 86400;
  out[0] = (uint32_t)(c % 60);
  out[1] = (uint32_t)((c / 60) % 60);
  out[2] = (uint32_t)((c / 3600) % 24);
  out[3] = (uint32_t)(days & 0xff);
  out[4] = (uint32_t)(((days >> 8) & 1) | (halted ? 0x40 : 0) | (carry ? 0x80 : 0));
}

void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
uint32_t get32(const uint8_t *p)
{
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A clock that was started with no reading of the wall clock (base 0) moves to the first reading: no time passed */
void wall_poll(Machine *m)
{
  gambatte::GbadhocRtc r;
  if (!g_wall_shift)
    return;
  const int64_t now = (int64_t)gambatte_wall_time();
  if (now <= 0)
    return;
  g_wall_shift = 0;
  if (m->gb.gbadhocRtcGet(r)) {
    r.baseTime += (uint64_t)now;
    r.haltTime += (uint64_t)now;
    m->gb.gbadhocRtcSet(r);
  }
}

/* ---- the sound: 2 MiHz stereo down to the asked rate by a triangle filter of two output periods ---- */
void audio_reset(Machine *m)
{
  memset(m->u, 0, sizeof m->u); memset(m->v, 0, sizeof m->v); memset(m->pu, 0, sizeof m->pu);
  m->since = 0;
}

enum { GAIN_NUM = 1111 };

/* Precompute division-free scaling for (t * GAIN_NUM) / (1024 * d * d), 1 <= d <= MAX_DECIM. */
void audio_init_decim(Machine *m, unsigned d)
{
  m->decim = d;
  if ((d & (d - 1u)) == 0u) {
    unsigned logd = 0;
    while ((1u << logd) < d)
      logd++;
    m->decim_pow2 = 1;
    m->decim_mul = 0;
    m->decim_shift = 10u + 2u * logd;
  } else {
    const uint32_t d2 = d * d;
    unsigned l = 1;
    while ((1u << l) <= d2)
      l++;
    const uint64_t magic = (((uint64_t)1 << (32u + l)) + d2 - 1u) / d2;
    m->decim_pow2 = 0;
    m->decim_mul = (uint32_t)(magic - ((uint64_t)1 << 32));
    m->decim_shift = l - 1u;
  }
}

/* GAIN_NUM/1024: the level TGB Dual's mixer gives the same channels (a full-volume square wave: rms 8203
 * there, 7562 here before the gain; tests/suites.c AUDIO line), so the two cores sound as loud as each other.
 * Bit-identical to (int32_t)((int64_t)t * GAIN_NUM / (int64_t)(1024 * d * d)) clamped to [-32768, 32767]. */
static inline int32_t audio_scale(const Machine *m, int32_t t)
{
  const int64_t p = (int64_t)t * GAIN_NUM;
  int32_t o;
  if (m->decim_pow2) {
    const unsigned k = m->decim_shift;
    const int64_t bias = (p >> 63) & (((int64_t)1 << k) - 1);
    o = (int32_t)((p + bias) >> k);
  } else {
    const int neg = p < 0;
    const uint64_t ap = neg ? (uint64_t)(-p) : (uint64_t)p;
    const uint32_t u = (uint32_t)(ap >> 10);
    const uint32_t t1 = (uint32_t)(((uint64_t)u * m->decim_mul) >> 32);
    const uint32_t q = (t1 + ((u - t1) >> 1)) >> m->decim_shift;
    o = neg ? -(int32_t)q : (int32_t)q;
  }
  return o > 32767 ? 32767 : o < -32768 ? -32768 : o;
}

void audio_feed(Machine *m, const int16_t *in, unsigned n)
{
  const unsigned d = m->decim;
  const size_t out_cap = sizeof m->out / sizeof m->out[0] / 2;

  /* Finish any partial block in progress so the fast path starts aligned at since == 0. */
  if (m->since != 0) {
    unsigned need = d - m->since;
    unsigned take = n < need ? n : need;
    n -= take;
    while (take--) {
      const int32_t l = in[0], r = in[1], j = (int32_t)m->since;
      in += 2;
      m->u[0] += j * l; m->u[1] += j * r;
      m->v[0] += l;     m->v[1] += r;
      m->since++;
    }
    if (m->since == d) {
      const int32_t o0 = audio_scale(m, m->pu[0] + (int32_t)d * m->v[0] - m->u[0]);
      const int32_t o1 = audio_scale(m, m->pu[1] + (int32_t)d * m->v[1] - m->u[1]);
      m->pu[0] = m->u[0]; m->u[0] = 0; m->v[0] = 0;
      m->pu[1] = m->u[1]; m->u[1] = 0; m->v[1] = 0;
      m->since = 0;
      if (m->out_n < out_cap) {
        m->out[2 * m->out_n] = (int16_t)o0;
        m->out[2 * m->out_n + 1] = (int16_t)o1;
        m->out_n++;
      }
    }
  }

#if GAM_AUDIO_NEON
  /* Full d-sample blocks starting at since == 0: 4 stereo pairs per NEON iteration. */
  if (d >= 4 && n >= d) {
    static const int16_t kInitJ[4] = { 0, 1, 2, 3 };
    const int16x4_t vj0 = vld1_s16(kInitJ);
    const int16x4_t vfour = vdup_n_s16(4);
    const unsigned d4 = d & ~3u;
    int32_t pu0 = m->pu[0], pu1 = m->pu[1];
    size_t out_n = m->out_n;

    while (n >= d) {
      int32x4_t vu_l = vdupq_n_s32(0), vu_r = vdupq_n_s32(0);
      int32x2_t vv_l = vdup_n_s32(0), vv_r = vdup_n_s32(0);
      int16x4_t vj = vj0;
      for (unsigned k = 0; k < d4; k += 4) {
        int16x4x2_t s = vld2_s16(in);
        in += 8;
        vu_l = vmlal_s16(vu_l, vj, s.val[0]);
        vu_r = vmlal_s16(vu_r, vj, s.val[1]);
        vv_l = vpadal_s16(vv_l, s.val[0]);
        vv_r = vpadal_s16(vv_r, s.val[1]);
        vj = vadd_s16(vj, vfour);
      }
      int32x2_t ul2 = vadd_s32(vget_low_s32(vu_l), vget_high_s32(vu_l));
      int32x2_t ur2 = vadd_s32(vget_low_s32(vu_r), vget_high_s32(vu_r));
      int32x2_t u_lr = vpadd_s32(ul2, ur2);
      int32x2_t v_lr = vpadd_s32(vv_l, vv_r);
      int32_t u0 = vget_lane_s32(u_lr, 0), u1 = vget_lane_s32(u_lr, 1);
      int32_t v0 = vget_lane_s32(v_lr, 0), v1 = vget_lane_s32(v_lr, 1);

      for (unsigned j = d4; j < d; ++j) {
        const int32_t l = in[0], r = in[1];
        in += 2;
        u0 += (int32_t)j * l; u1 += (int32_t)j * r;
        v0 += l;              v1 += r;
      }

      const int32_t o0 = audio_scale(m, pu0 + (int32_t)d * v0 - u0);
      const int32_t o1 = audio_scale(m, pu1 + (int32_t)d * v1 - u1);
      pu0 = u0;
      pu1 = u1;
      if (out_n < out_cap) {
        m->out[2 * out_n] = (int16_t)o0;
        m->out[2 * out_n + 1] = (int16_t)o1;
        out_n++;
      }
      n -= d;
    }

    m->pu[0] = pu0;
    m->pu[1] = pu1;
    m->out_n = out_n;
  }
#endif

  /* Remaining samples (and full blocks when d < 4 or non-NEON). */
  while (n--) {
    const int32_t l = in[0], r = in[1], j = (int32_t)m->since;
    in += 2;
    m->u[0] += j * l; m->u[1] += j * r;
    m->v[0] += l;     m->v[1] += r;
    if (++m->since == d) {
      const int32_t o0 = audio_scale(m, m->pu[0] + (int32_t)d * m->v[0] - m->u[0]);
      const int32_t o1 = audio_scale(m, m->pu[1] + (int32_t)d * m->v[1] - m->u[1]);
      m->pu[0] = m->u[0]; m->u[0] = 0; m->v[0] = 0;
      m->pu[1] = m->u[1]; m->u[1] = 0; m->v[1] = 0;
      m->since = 0;
      if (m->out_n < out_cap) {
        m->out[2 * m->out_n] = (int16_t)o0;
        m->out[2 * m->out_n + 1] = (int16_t)o1;
        m->out_n++;
      }
    }
  }
}

/* ---- the memory the CPU sees ---- */
uint8_t *ptr_for(Machine *m, unsigned a, int write)
{
  unsigned n = 0;
  if (a >= 0xFFFF || (a >= 0xFEA0 && a < 0xFF80) || (a >= 0xFF00 && a < 0xFF80))
    return NULL;
  if (a >= 0xFF80)
    return (uint8_t *)m->gb.zeropage_ptr() + (a - 0xFF80);
  if (a >= 0xFE00)
    return (uint8_t *)m->gb.oamram_ptr() + (a - 0xFE00);
  if (a >= 0xF000)
    a -= 0x2000;                            /* the echo of work RAM */
  if (a >= 0x8000 && a < 0xA000)
    return m->gb.gbadhocVram() + a;
  if (write) {
    if (a < 0x8000)
      return NULL;
    uint8_t *w = m->gb.gbadhocWmem(a >> 12);
    if (!w)
      return NULL;
    if (a >= 0xA000 && a < 0xC000) {
      uint8_t *ram = m->gb.gbadhocCartRam(&n);
      if (!n || w + a < ram || w + a >= ram + n)
        return NULL;                       /* the clock registers, or nothing */
    }
    return w + a;
  }
  const uint8_t *r = m->gb.gbadhocRmem(a >> 12);
  if (!r)
    return NULL;
  if (a >= 0xA000 && a < 0xC000) {
    uint8_t *ram = m->gb.gbadhocCartRam(&n);
    if (!n || r + a < ram || r + a >= ram + n)
      return NULL;
  }
  return (uint8_t *)(r + a);
}

uint8_t ra_byte(Machine *m, unsigned a)
{
  unsigned n = 0;
  const size_t cart = cart_ram_size(m);
  uint8_t *ram = m->gb.gbadhocCartRam(&n);
  uint8_t *wram = (uint8_t *)m->gb.rambank0_ptr();
  if (a >= 0xE000 && a < 0xFE00)
    a -= 0x2000;
  if (a >= 0xA000 && a < 0xC000)
    return (size_t)(a - 0xA000) < cart ? ram[a - 0xA000] : 0;
  if (a >= 0xC000 && a < 0xE000)
    return wram[a - 0xC000];
  if (a >= 0xFF00 && a < 0xFF80)
    return 0;
  if (a < 0x10000) {
    uint8_t *p;
    if (a == 0xFFFF)
      return 0;
    p = ptr_for(m, a, 0);
    return p ? *p : 0;
  }
  if (a < 0x16000)
    return m->cgb ? ((uint8_t *)m->gb.rambank2_ptr())[a - 0x10000] : 0;
  if (a < 0x34000 && (size_t)(0x2000 + (a - 0x16000)) < cart)
    return ram[0x2000 + (a - 0x16000)];
  return 0;
}

void cheats_rom_restore(Machine *m)
{
  unsigned n;
  uint8_t *rom = m->gb.gbadhocRom(&n);
  while (m->undo_n) {
    --m->undo_n;
    rom[m->undo[m->undo_n].offset] = m->undo[m->undo_n].old;
  }
}

int cheats_rom_patch(Machine *m, const gambatte_host_cheat_t *c)
{
  unsigned total;
  uint8_t *rom = m->gb.gbadhocRom(&total);
  const unsigned a = c->address & 0x7FFF;
  size_t offset = a, step = 0x4000;
  int full = 0;
  if (a < 0x4000)
    step = total;                           /* the fixed bank: one place */
  for (; offset < total; offset += step) {
    if (c->has_compare && rom[offset] != c->compare)
      continue;
    if (rom[offset] == c->value)
      continue;
    if (m->undo_n >= sizeof m->undo / sizeof m->undo[0]) {
      full = 1;
      break;
    }
    m->undo[m->undo_n].offset = (uint32_t)offset;
    m->undo[m->undo_n].old = rom[offset];
    m->undo_n++;
    rom[offset] = c->value;
  }
  return full;
}

/* One GameShark write: 8n = cartridge RAM bank n, 9n = work RAM bank n (Color), else what is mapped now */
void cheat_poke(Machine *m, const gambatte_host_cheat_t *c)
{
  const unsigned a = c->address;
  if ((c->type & 0xF0) == 0x90 && a >= 0xD000 && a < 0xE000 && m->cgb) {
    const unsigned bank = c->type & 7;
    ((uint8_t *)m->gb.rambank0_ptr())[(bank ? bank : 1) * 0x1000 + (a & 0x0FFF)] = c->value;
    return;
  }
  if ((c->type & 0xF0) == 0x80 && a >= 0xA000 && a < 0xC000) {
    unsigned n = 0;
    uint8_t *ram = m->gb.gbadhocCartRam(&n);
    const size_t offset = (size_t)(c->type & 0x0F) * 0x2000 + (a & 0x1FFF);
    if (offset < n && offset < cart_ram_size(m))
      ram[offset] = c->value;
    return;
  }
  uint8_t *p = ptr_for(m, a, 1);
  if (p)
    *p = c->value;
}

}  // namespace

/* ------------------------------------------------------------------ the API */

extern "C" {

int gambatte_host_running(void) { return g_m != NULL; }
const uint8_t *gambatte_host_header(void) { return g_m ? g_m->header + 0x34 : NULL; }

void gambatte_host_set_wallclock(time_t (*wallclock)(void)) { g_wall = wallclock; }

int gambatte_host_create(const uint8_t *rom, size_t size, int cgb_console, unsigned rate,
                         const gambatte_host_cb_t *cb)
{
  Machine *m;
  unsigned d;
  if (g_m || !rom || size < 0x4000 || !cb)
    return -1;
  m = new (std::nothrow) Machine;
  if (!m)
    return -1;
  /* (the wall clock: a clock that starts before the first reading is moved to it) */
  g_wall_last = 0;
  g_wall_shift = 0;
  if (gambatte_wall_time() == 0)
    g_wall_shift = 1;
  m->cb = *cb;
  m->cgb_console = cgb_console != 0;
  m->serial.cb = *cb;
  m->gb.setInputGetter(&m->input);
  if (cb->serial_transfer)
    m->gb.setSerialIO(&m->serial);
  if (m->gb.load(rom, (unsigned)size, m->cgb_console ? 0 : gambatte::GB::FORCE_DMG) != 0) {
    delete m;
    return -1;
  }
  m->cgb = m->gb.isCgb();
  memcpy(m->header, rom + 0x100, sizeof m->header);
  m->rom_bytes = size;
  m->cart_id = fnv(rom + 0x134, 0x1C, 2166136261u) ^ (uint32_t)(size * 2654435761u) ^ (m->cgb ? 0x5bd1e995u : 0);
  d = rate ? (kNative + rate / 2) / rate : 64;
  if (d < 1) d = 1;
  if (d > MAX_DECIM) d = MAX_DECIM;
  audio_init_decim(m, d);
  m->rate = kNative / d;
  g_m = m;
  /* palettes of a Game Boy running as a Game Boy (the DMG-hardware path only) */
  if (!m->cgb) {
    for (unsigned i = 0; i < 12; ++i)
      m->gb.setDmgPaletteColor(i / 4, i % 4, g_dmg[i]);
  }
  wall_poll(m);
  return 0;
}

void gambatte_host_shutdown(void)
{
  Machine *m = g_m;
  g_m = NULL;
  if (m) {
    delete m;
  }
}

void gambatte_host_set_skip_render(int skip) { if (g_m) g_m->skip = skip != 0; }
void gambatte_host_set_quiet(int quiet) { if (g_m) g_m->quiet = quiet != 0; }
unsigned gambatte_host_sample_rate(void) { return g_m ? g_m->rate : 0; }
unsigned gambatte_host_frame_width(void) { return VW; }
unsigned gambatte_host_frame_height(void) { return VH; }
uint64_t gambatte_host_frame_number(void) { return g_m ? g_m->frames : 0; }

int gambatte_host_rom_title(char *dst, size_t cap)
{
  size_t i;
  if (!g_m || !dst || cap < 17)
    return -1;
  for (i = 0; i < 16; i++) {
    const uint8_t c = g_m->header[0x34 + i];
    if (c < 0x20 || c > 0x7E)
      break;
    dst[i] = (char)c;
  }
  dst[i] = '\0';
  while (i > 0 && dst[i - 1] == ' ')
    dst[--i] = '\0';
  return 0;
}

int gambatte_host_run_frame(uint16_t buttons)
{
  Machine *m = g_m;
  unsigned samples;
  if (!m)
    return -1;
  wall_poll(m);
  m->input.keys = pad_map(buttons);
  /* GameShark codes: at the start of every frame */
  for (unsigned i = 0; i < m->ram_cheat_n; ++i)
    cheat_poke(m, &m->ram_cheat[i]);
  m->out_n = 0;
  samples = SOUND_RUN;
  while (m->gb.runFor(m->skip ? NULL : (gambatte::video_pixel_t *)m->video, VPITCH, m->sound, SOUND_BUF, samples) < 0) {
    if (!m->quiet)
      audio_feed(m, (const int16_t *)m->sound, samples);
    samples = SOUND_RUN;
  }
  if (!m->quiet)
    audio_feed(m, (const int16_t *)m->sound, samples);
  m->frames++;
  if (m->cb.video)
    m->cb.video(m->cb.userdata, m->video, VW, VH, VPITCH * 2);
  if (!m->quiet && m->cb.audio_batch)
    m->cb.audio_batch(m->cb.userdata, m->out, m->out_n);
  return 0;
}

/* ---- the battery ---- */

size_t gambatte_host_cart_ram_size(void) { return g_m ? cart_ram_size(g_m) : 0; }

size_t gambatte_host_save_ram_size(void)
{
  size_t ram;
  int clock;
  if (!g_m || !has_battery(g_m))
    return 0;
  ram = cart_ram_size(g_m);
  clock = has_clock(g_m);
  if (!ram && !clock)
    return 0;
  return ram + (clock ? RTC_TRAILER : 0);
}

int gambatte_host_save_ram_read(void *dst, size_t cap)
{
  Machine *m = g_m;
  const size_t need = gambatte_host_save_ram_size();
  const size_t ram = gambatte_host_cart_ram_size();
  uint8_t *out = (uint8_t *)dst;
  unsigned n = 0;
  if (!m || !need)
    return 0;
  if (!dst || cap < need)
    return -1;
  memcpy(out, m->gb.gbadhocCartRam(&n), ram);
  if (has_clock(m)) {
    gambatte::GbadhocRtc r;
    int64_t now, c;
    uint32_t live[5];
    uint8_t *t = out + ram;
    unsigned carry;
    memset(t, 0, RTC_TRAILER);
    if (m->gb.gbadhocRtcGet(r)) {
      wall_poll(m);
      now = (int64_t)gambatte_wall_time();
      c = ((r.dh & 0x40) ? (int64_t)r.haltTime : now) - (int64_t)r.baseTime;
      if (c < 0)
        c = 0;
      carry = (r.dh & 0x80) != 0;
      if (c >= kRtcSpan) {
        carry = 1;
        c %= kRtcSpan;
      }
      rtc_regs(c, (r.dh & 0x40) != 0, carry, live);
      /* no clock reading yet: the value the format gives a writer that cannot tell the time */
      if (now <= 0)
        now = kRtcUnknown;
      for (int i = 0; i < 5; i++)
        put32(t + i * 4, live[i]);
      put32(t + 20, r.s);
      put32(t + 24, r.m);
      put32(t + 28, r.h);
      put32(t + 32, r.dl);
      put32(t + 36, r.dh);
      put32(t + 40, (uint32_t)((uint64_t)now & 0xffffffffu));
      put32(t + 44, (uint32_t)((uint64_t)now >> 32));
    }
  }
  return 0;
}

int gambatte_host_save_ram_write(const void *src, size_t size)
{
  Machine *m = g_m;
  const uint8_t *in = (const uint8_t *)src;
  size_t ram;
  unsigned n = 0;
  if (!m || (!src && size))
    return -1;
  if (!size)
    return 0;
  ram = cart_ram_size(m);
  memcpy(m->gb.gbadhocCartRam(&n), in, size < ram ? size : ram);
  if (!has_clock(m))
    return 0;
  if (size == ram + RTC_TRAILER || size == ram + RTC_TRAILER - 4) {
    const uint8_t *t = in + ram;
    int64_t stamp = get32(t + 40);
    int64_t now, c;
    const unsigned high = (unsigned)get32(t + 16) & 0xFF;
    unsigned carry = (high >> 7) & 1;
    gambatte::GbadhocRtc r;
    if (size == ram + RTC_TRAILER)
      stamp |= (int64_t)get32(t + 44) << 32;
    if (stamp < kRtcEpochMin)
      return 0;                             /* not real clock data: the clock starts over */
    wall_poll(m);
    now = (int64_t)gambatte_wall_time();
    c = (int64_t)(get32(t) % 60) + (int64_t)(get32(t + 4) % 60) * 60 + (int64_t)(get32(t + 8) % 24) * 3600 +
        ((int64_t)(get32(t + 12) & 0xff) | ((int64_t)(high & 1) << 8)) * 86400;
    if (!m->gb.gbadhocRtcGet(r))
      return 0;
    if (high & 0x40) {
      r.haltTime = (uint64_t)now;
    } else {
      /* the time the console was off; a stamp from the future adds nothing */
      if (stamp < now)
        c += now - stamp;
      if (c >= kRtcSpan) {
        carry = 1;
        c %= kRtcSpan;
      }
    }
    r.baseTime = (uint64_t)(now - c);
    if (high & 0x40)
      r.haltTime = (uint64_t)now;
    r.s = (unsigned char)get32(t + 20);
    r.m = (unsigned char)get32(t + 24);
    r.h = (unsigned char)get32(t + 28);
    r.dl = (unsigned char)get32(t + 32);
    r.dh = (unsigned char)((get32(t + 36) & 0x01) | (high & 0x40) | (carry ? 0x80 : 0));
    m->gb.gbadhocRtcSet(r);
  }
  return 0;
}

/* ---- states ---- */

int gambatte_host_state_kind(const void *src, size_t size)
{
  if (!src || size < 4)
    return 0;
  if (!memcmp(src, kMagicTgb, 4))
    return 1;
  if (!memcmp(src, kMagicGam, 4))
    return 2;
  return 0;
}

enum { AUDIO_TRAILER = 28 };

static void audio_save(const Machine *m, uint8_t *o)
{
  put32(o + 0, (uint32_t)m->u[0]);
  put32(o + 4, (uint32_t)m->u[1]);
  put32(o + 8, (uint32_t)m->v[0]);
  put32(o + 12, (uint32_t)m->v[1]);
  put32(o + 16, (uint32_t)m->pu[0]);
  put32(o + 20, (uint32_t)m->pu[1]);
  put32(o + 24, (uint32_t)m->since);
}

static void audio_load(Machine *m, const uint8_t *p)
{
  const unsigned since = get32(p + 24);
  if (since < m->decim) {
    m->u[0] = (int32_t)get32(p + 0);
    m->u[1] = (int32_t)get32(p + 4);
    m->v[0] = (int32_t)get32(p + 8);
    m->v[1] = (int32_t)get32(p + 12);
    m->pu[0] = (int32_t)get32(p + 16);
    m->pu[1] = (int32_t)get32(p + 20);
    m->since = since;
  } else {
    audio_reset(m);
  }
}

size_t gambatte_host_state_size(void)
{
  return g_m ? STATE_HEAD + g_m->gb.stateSize() + AUDIO_TRAILER : 0;
}

long gambatte_host_state_save(void *dst, size_t cap)
{
  Machine *m = g_m;
  uint8_t *o = (uint8_t *)dst;
  const size_t gb_n = m ? m->gb.stateSize() : 0;
  const size_t n = STATE_HEAD + gb_n + AUDIO_TRAILER;
  if (!m || !dst || cap < n)
    return -1;
  memcpy(o, kMagicGam, 4);
  put32(o + 4, 2);
  put32(o + 8, m->cgb ? 1u : 0u);
  put32(o + 12, m->cart_id);
  m->gb.saveState(o + STATE_HEAD);
  audio_save(m, o + STATE_HEAD + gb_n);
  return (long)n;
}

/* Returns state format version (1 = core only, 2 = core + audio filter trailer), or 0 if invalid. */
static int state_check(Machine *m, const void *src, size_t size, size_t *gb_sz)
{
  const uint8_t *p = (const uint8_t *)src;
  const size_t raw = m ? m->gb.stateSize() : 0;
  uint32_t ver;
  if (!m || !src || size < STATE_HEAD)
    return 0;
  ver = get32(p + 4);
  if (memcmp(p, kMagicGam, 4) || get32(p + 8) != (m->cgb ? 1u : 0u) || get32(p + 12) != m->cart_id)
    return 0;
  if (ver == 2 && size == STATE_HEAD + raw + AUDIO_TRAILER) {
    if (gb_sz) *gb_sz = raw;
    return 2;
  }
  if (ver == 1 && size == STATE_HEAD + raw) {
    if (gb_sz) *gb_sz = raw;
    return 1;
  }
  return 0;
}

int gambatte_host_state_load(const void *src, size_t size)
{
  Machine *m = g_m;
  const uint8_t *p = (const uint8_t *)src;
  size_t gb_n = 0;
  const int ver = state_check(m, src, size, &gb_n);
  if (!ver)
    return -1;
  if (!m->gb.loadState(p + STATE_HEAD, gb_n))
    return -1;
  if (ver >= 2)
    audio_load(m, p + STATE_HEAD + gb_n);
  else
    audio_reset(m);
  return 0;
}

size_t gambatte_host_ahead_size(void)
{
  return g_m ? gambatte_host_state_size() + 8 : 0;
}

long gambatte_host_ahead_save(void *dst, size_t cap)
{
  Machine *m = g_m;
  const size_t n = gambatte_host_state_size();
  uint8_t *o = (uint8_t *)dst;
  if (!m || !dst || cap < n + 8 || gambatte_host_state_save(dst, cap) != (long)n)
    return -1;
  put32(o + n, (uint32_t)m->frames);
  put32(o + n + 4, (uint32_t)(m->frames >> 32));
  return (long)(n + 8);
}

int gambatte_host_ahead_load(const void *src, size_t size)
{
  Machine *m = g_m;
  const size_t n = gambatte_host_state_size();
  const uint8_t *p = (const uint8_t *)src;
  size_t gb_n = 0;
  if (!m || size != n + 8 || state_check(m, src, n, &gb_n) != 2)
    return -1;
  if (!m->gb.loadState(p + STATE_HEAD, gb_n))
    return -1;
  audio_load(m, p + STATE_HEAD + gb_n);
  m->frames = (uint64_t)get32(p + n) | (uint64_t)get32(p + n + 4) << 32;
  return 0;
}

/* ---- display ---- */

void gambatte_host_set_dmg_colors(const uint16_t rgb565[12])
{
  memcpy(g_dmg, rgb565, sizeof g_dmg);
  if (g_m && !g_m->cgb) {
    for (unsigned i = 0; i < 12; ++i)
      g_m->gb.setDmgPaletteColor(i / 4, i % 4, g_dmg[i]);
  }
}

void gambatte_host_set_color_correction(int on)
{
  g_cc = on != 0;
  /* every Color palette entry is converted again: setting a Game Boy palette entry rebuilds them all */
  if (g_m)
    g_m->gb.setDmgPaletteColor(0, 0, g_m->cgb ? 0 : g_dmg[0]);
}

/* ---- cheats and memory ---- */

int gambatte_host_cheats_set(const gambatte_host_cheat_t *codes, unsigned count)
{
  Machine *m = g_m;
  int dropped = 0;
  if (!m || (count && !codes))
    return -1;
  cheats_rom_restore(m);
  m->ram_cheat_n = 0;
  for (unsigned i = 0; i < count; i++) {
    if (codes[i].rom)
      dropped += cheats_rom_patch(m, &codes[i]);
    else if (m->ram_cheat_n < sizeof m->ram_cheat / sizeof m->ram_cheat[0])
      m->ram_cheat[m->ram_cheat_n++] = codes[i];
    else
      dropped++;
  }
  return dropped;
}

uint32_t gambatte_host_ra_read(uint32_t addr, uint8_t *out, uint32_t len)
{
  uint32_t i;
  if (!g_m || !out || addr >= 0x34000)
    return 0;
  if (len > 0x34000 - addr)
    len = 0x34000 - addr;
  for (i = 0; i < len; i++)
    out[i] = ra_byte(g_m, addr + i);
  return len;
}

int gambatte_host_peek(uint16_t addr, void *out, unsigned len)
{
  uint8_t *o = (uint8_t *)out;
  if (!g_m || !out)
    return -1;
  for (unsigned i = 0; i < len; i++) {
    const uint8_t *p = ptr_for(g_m, (unsigned)addr + i, 0);
    if (!p)
      return -1;
    o[i] = *p;
  }
  return 0;
}

int gambatte_host_poke(uint16_t addr, const void *data, unsigned len)
{
  const uint8_t *s = (const uint8_t *)data;
  if (!g_m || !data)
    return -1;
  for (unsigned i = 0; i < len; i++)
    if (!ptr_for(g_m, (unsigned)addr + i, 1))
      return -1;
  for (unsigned i = 0; i < len; i++)
    *ptr_for(g_m, (unsigned)addr + i, 1) = s[i];
  return 0;
}

void gambatte_host_regs(uint8_t out[8])
{
  if (g_m)
    g_m->gb.gbadhocRegs(out, NULL);
}

}  // extern "C"
