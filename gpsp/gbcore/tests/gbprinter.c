/* Game Boy Printer (frontend-common/fe_gbprinter.h): packets in, replies and
 * the printed strip out; then the same through the core's serial port with a
 * synthetic cartridge that sends a packet stream byte by byte. */
#include "../gbcore.h"
#include "fe_gbprinter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *what)
{
  fprintf(stderr, "FAIL gbprinter: %s\n", what);
  return 1;
}

static struct { int n; int w, h; uint8_t *px; } got;

static void on_done(void *u, const uint8_t *shades, int w, int h)
{
  (void)u;
  free(got.px);
  got.px = (uint8_t *)malloc((size_t)w * h);
  memcpy(got.px, shades, (size_t)w * h);
  got.w = w; got.h = h; got.n++;
}

/* Builds a packet; returns its length. */
static size_t packet(uint8_t *o, uint8_t cmd, uint8_t comp, const uint8_t *d,
                     unsigned len, int bad_sum)
{
  unsigned i, sum = cmd + comp + (len & 255) + (len >> 8);
  size_t n = 0;
  o[n++] = 0x88; o[n++] = 0x33; o[n++] = cmd; o[n++] = comp;
  o[n++] = (uint8_t)len; o[n++] = (uint8_t)(len >> 8);
  for (i = 0; i < len; i++) { o[n++] = d[i]; sum += d[i]; }
  if (bad_sum) sum ^= 0x0101;
  o[n++] = (uint8_t)sum; o[n++] = (uint8_t)(sum >> 8);
  o[n++] = 0; o[n++] = 0;                /* the printer answers these */
  return n;
}

/* Sends a packet; returns the status byte (the last reply) and checks the
 * acknowledge. */
static int send(gbprn *p, const uint8_t *b, size_t n, uint8_t *ack)
{
  size_t i;
  uint8_t rx = 0;
  for (i = 0; i < n; i++)
  {
    uint8_t r = gbprn_byte(p, b[i]);
    if (i + 2 == n) *ack = r;
    rx = r;
    if (i + 2 < n && r != 0) return -1;   /* every earlier byte answers 00 */
  }
  return rx;
}

static int expect(gbprn *p, uint8_t cmd, uint8_t comp, const uint8_t *d,
                  unsigned len, int bad, int want_status, const char *what)
{
  uint8_t b[0x1000], ack = 0;
  size_t n = packet(b, cmd, comp, d, len, bad);
  int st = send(p, b, n, &ack);
  if (st < 0 || ack != 0x81 || st != want_status)
  {
    fprintf(stderr, "%s: ack %02X status %02X, wanted 81 / %02X\n", what, ack, st, want_status);
    return 1;
  }
  return 0;
}

/* A tile row of the test picture: tile t of the buffer, row y, is pixel
 * colour (x + y + t) & 3 at column x. */
static void fill_tiles(uint8_t *buf, unsigned tiles)
{
  unsigned t, y, x;
  memset(buf, 0, tiles * 16);
  for (t = 0; t < tiles; t++)
    for (y = 0; y < 8; y++)
      for (x = 0; x < 8; x++)
      {
        unsigned c = (x + y + t) & 3;
        buf[t * 16 + y * 2] |= (uint8_t)((c & 1) << (7 - x));
        buf[t * 16 + y * 2 + 1] |= (uint8_t)(((c >> 1) & 1) << (7 - x));
      }
}

static int shade_of(unsigned pal, unsigned c) { return (pal >> (c * 2)) & 3; }

/* ---- the integration through the core ---------------------------------- */

static gbprn *g_prn;

static int on_serial(void *u, uint8_t out, int internal, uint8_t *rx)
{
  (void)u;
  if (!internal) return 0;
  *rx = gbprn_byte(g_prn, out);
  return 1;
}
static void on_video(void *u, const gbcore_video_frame_t *f) { (void)u; (void)f; }
static void on_audio(void *u, const int16_t *s, size_t n) { (void)u; (void)s; (void)n; }

/* The program sends g_stream (copied into ROM at 0x4000) byte by byte:
 *   LD HL,$4000 ; LD BC,count ; loop: LD A,(HL+) ; LDH ($01),A ; LD A,$81 ;
 *   LDH ($02),A ; wait for bit 7 of SC to clear ; LDH A,($01) ; store in
 *   WRAM at $C000.. ; DEC BC ; until zero ; then done flag at $CFFF. */
static int through_core(const uint8_t *stream, size_t n, uint8_t *replies)
{
  static uint8_t rom[0x10000];
  static const uint8_t logo[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,
    0x00,0x0C,0x00,0x0D,0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,
    0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,0xBB,0xBB,0x67,0x63,
    0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
  };
  uint8_t prog[] = {
    0x31, 0xFE, 0xFF,             /* LD SP,$FFFE            */
    0x21, 0x00, 0x40,             /* LD HL,$4000            */
    0x11, 0x00, 0xC0,             /* LD DE,$C000            */
    0x01, 0, 0,                   /* LD BC,count   (patched)*/
    /* loop: 0x15C */
    0x2A,                         /* LD A,(HL+)             */
    0xE0, 0x01,                   /* LDH ($01),A            */
    0x3E, 0x81, 0xE0, 0x02,       /* LD A,$81 ; LDH ($02),A */
    /* wait: */
    0xF0, 0x02, 0xCB, 0x7F,       /* LDH A,($02) ; BIT 7,A  */
    0x20, 0xFA,                   /* JR NZ,wait             */
    0xF0, 0x01, 0x12, 0x13,       /* LDH A,($01) ; LD (DE),A ; INC DE */
    0x0B,                         /* DEC BC                 */
    0x78, 0xB1,                   /* LD A,B ; OR C          */
    0x20, 0xEA,                   /* JR NZ,loop             */
    0x3E, 0x01, 0xEA, 0xFF, 0xCF, /* done flag              */
    0x18, 0xFE
  };
  gbcore_callbacks_t cb;
  gbcore_t *c;
  unsigned i, sum = 0;
  uint8_t check = 0, flag = 0;
  int f;

  memset(rom, 0, sizeof(rom));
  rom[0x100] = 0xC3; rom[0x101] = 0x50; rom[0x102] = 0x01;
  memcpy(rom + 0x104, logo, 48);
  memcpy(rom + 0x134, "PRINTTEST", 9);
  rom[0x148] = 0x01;
  prog[10] = (uint8_t)n; prog[11] = (uint8_t)(n >> 8);
  memcpy(rom + 0x150, prog, sizeof(prog));
  memcpy(rom + 0x4000, stream, n);
  for (i = 0x134; i <= 0x14C; i++) check = (uint8_t)(check - rom[i] - 1);
  rom[0x14D] = check;
  for (i = 0; i < sizeof(rom); i++) if (i != 0x14E && i != 0x14F) sum += rom[i];
  rom[0x14E] = (uint8_t)(sum >> 8); rom[0x14F] = (uint8_t)sum;

  memset(&cb, 0, sizeof(cb));
  cb.video = on_video; cb.audio_batch = on_audio; cb.serial_transfer = on_serial;
  c = gbcore_create(NULL, rom, sizeof(rom), FE_CONSOLE_GB, 32768, &cb);
  if (!c) return -1;
  for (f = 0; f < 3000; f++)
  {
    gbcore_run_frame(c, 0);
    gbprn_frame(g_prn);
    gbcore_peek(c, 0xCFFF, &flag, 1);
    if (flag) break;
  }
  if (!flag) { gbcore_shutdown(c); return -2; }
  gbcore_peek(c, 0xC000, replies, (unsigned)n);
  gbcore_shutdown(c);
  return 0;
}

int main(void)
{
  gbprn *p = (gbprn *)calloc(1, sizeof(*p));
  uint8_t tiles[0x280], pr[4];
  int x, y, st;

  gbprn_init(p, on_done, NULL);

  /* 1. Detection: the status inquiry answers 81 and a clear status. */
  if (expect(p, 0x0F, 0, NULL, 0, 0, 0x00, "detect")) return fail("detection");

  /* 2. A camera-style print: init, 0x280 bytes of tiles (2 tile rows),
   * an empty data packet, print with margins 0x13 and the palette E4. */
  if (expect(p, 1, 0, NULL, 0, 0, 0x00, "init")) return fail("init");
  fill_tiles(tiles, 40);
  if (expect(p, 4, 0, tiles, 0x280, 0, 0x08, "data")) return fail("data: ready status 08");
  if (expect(p, 4, 0, NULL, 0, 0, 0x08, "empty data")) return fail("empty data");
  pr[0] = 1; pr[1] = 0x13; pr[2] = 0xE4; pr[3] = 0x40;
  if (expect(p, 2, 0, pr, 4, 0, 0x06, "print")) return fail("print: busy status 06");
  if (got.n != 1) return fail("a margin after the picture tears the strip off");
  if (got.w != 160 || got.h != 8 + 16 + 24) return fail("strip size: 1 + 2 tile rows + 3 margins of 8");
  for (y = 0; y < 48; y++)
    for (x = 0; x < 160; x++)
    {
      int want = 0;
      if (y >= 8 && y < 24)
      {
        unsigned py = y - 8, t = (py / 8) * 20 + x / 8;
        want = shade_of(0xE4, ((x % 8) + (py % 8) + t) & 3);
      }
      if (got.px[y * 160 + x] != want) { fprintf(stderr, "pixel %d,%d: %d, wanted %d\n", x, y, got.px[y * 160 + x], want); return fail("pixels"); }
    }
  puts("print: 160x48 strip, every pixel through palette E4");
  for (x = 0; x < 20; x++) gbprn_frame(p);
  if (expect(p, 0x0F, 0, NULL, 0, 0, 0x04, "status after")) return fail("status 04 once finished");

  /* 3. The palette: 1B inverts the four shades. */
  if (expect(p, 1, 0, NULL, 0, 0, 0x00, "init 2")) return fail("init 2");
  expect(p, 4, 0, tiles, 0x280, 0, 0x08, "data 2");
  expect(p, 4, 0, NULL, 0, 0, 0x08, "empty 2");
  pr[1] = 0x03; pr[2] = 0x1B;
  expect(p, 2, 0, pr, 4, 0, 0x06, "print 2");
  if (got.n != 2 || got.h != 16 + 24) return fail("second strip");
  if (got.px[0] != shade_of(0x1B, 0) || got.px[1] != shade_of(0x1B, 1))
    return fail("palette 1B");
  puts("palette 1B: shades inverted");

  /* 4. Compression: literal runs and repeats give the same buffer. */
  {
    uint8_t comp[0x300], plain[0x100];
    unsigned i, n = 0;
    for (i = 0; i < 0x100; i++) plain[i] = (uint8_t)(i < 0xC0 ? 0x55 : i);
    /* 0xC0 bytes of 0x55 as two repeats (0x80+ gives up to 129), then
     * 0x40 literals */
    comp[n++] = 0xFF; comp[n++] = 0x55;          /* 129 x 55 */
    comp[n++] = 0x80 + (0xC0 - 129 - 2); comp[n++] = 0x55;  /* 63 x 55 */
    comp[n++] = 0x3F;                            /* 64 literals */
    for (i = 0xC0; i < 0x100; i++) comp[n++] = plain[i];
    gbprn_init(p, on_done, NULL);
    got.n = 0;
    expect(p, 1, 0, NULL, 0, 0, 0, "init 3");
    if (expect(p, 4, 1, comp, n, 0, 0x08, "compressed data")) return fail("compressed data");
    if (p->buf_len != 256 || memcmp(p->buf, plain, 256) != 0)
      return fail("run-length decoding");
    puts("compression: repeats and literals decode to the same bytes");
  }

  /* 5. A bad checksum is flagged and the packet ignored; a good one clears
   * the flag. */
  gbprn_init(p, on_done, NULL);
  got.n = 0;
  expect(p, 1, 0, NULL, 0, 0, 0, "init 4");
  if (expect(p, 4, 0, tiles, 0x280, 1, 0x01, "bad sum")) return fail("checksum error: status 01");
  if (p->buf_len != 0) return fail("a packet with a bad checksum is not kept");
  if (expect(p, 4, 0, tiles, 0x280, 0, 0x08, "good again")) return fail("a good packet clears the error");
  if (p->buf_len != 0x280) return fail("the good packet is kept");
  puts("checksum: error flagged, data dropped, next packet fine");

  /* 6. Multi-page strip: three bands, no margin until the last. */
  gbprn_init(p, on_done, NULL);
  got.n = 0;
  expect(p, 1, 0, NULL, 0, 0, 0, "init 5");
  for (y = 0; y < 3; y++)
  {
    expect(p, 4, 0, tiles, 0x280, 0, 0x08, "band data");
    expect(p, 4, 0, NULL, 0, 0, 0x08, "band end");
    pr[0] = 1; pr[1] = y == 2 ? 0x02 : 0x00; pr[2] = 0xE4; pr[3] = 0x40;
    expect(p, 2, 0, pr, 4, 0, 0x06, "band print");
    if (y < 2 && got.n != 0) return fail("no strip before the last margin");
    for (x = 0; x < 20; x++) gbprn_frame(p);
  }
  if (got.n != 1 || got.h != 3 * 16 + 16) return fail("three bands make one strip with the last margin");
  puts("multi-page: three prints, one strip of 160x64");

  /* 7. An open strip is closed after a pause. */
  gbprn_init(p, on_done, NULL);
  got.n = 0;
  expect(p, 1, 0, NULL, 0, 0, 0, "init 6");
  expect(p, 4, 0, tiles, 0x280, 0, 0x08, "data 6");
  expect(p, 4, 0, NULL, 0, 0, 0x08, "empty 6");
  pr[1] = 0x00;
  expect(p, 2, 0, pr, 4, 0, 0x06, "print 6");
  if (got.n != 0) return fail("a print with no margin leaves the strip open");
  for (x = 0; x < GBPRN_STRIP_IDLE + 2; x++) gbprn_frame(p);
  if (got.n != 1 || got.h != 16) return fail("closed after two seconds");
  puts("an open strip is closed after a pause");

  /* 8. A packet that stops half way is dropped; the next starts clean. */
  gbprn_init(p, on_done, NULL);
  gbprn_byte(p, 0x88); gbprn_byte(p, 0x33); gbprn_byte(p, 0x04);
  for (x = 0; x < 20; x++) gbprn_frame(p);
  if (expect(p, 0x0F, 0, NULL, 0, 0, 0x00, "after drop")) return fail("a stalled packet is dropped");

  /* 9. Through the core: the cartridge sends a camera-style stream byte by
   * byte over the serial port and gets the printer's answers. */
  {
    static uint8_t stream[0x1000], replies[0x1000];
    size_t n = 0;
    gbprn_init(p, on_done, NULL);
    got.n = 0;
    g_prn = p;
    n += packet(stream + n, 1, 0, NULL, 0, 0);
    fill_tiles(tiles, 40);
    n += packet(stream + n, 4, 0, tiles, 0x280, 0);
    n += packet(stream + n, 4, 0, NULL, 0, 0);
    pr[0] = 1; pr[1] = 0x13; pr[2] = 0xE4; pr[3] = 0x40;
    n += packet(stream + n, 2, 0, pr, 4, 0);
    n += packet(stream + n, 0x0F, 0, NULL, 0, 0);
    st = through_core(stream, n, replies);
    if (st != 0) { fprintf(stderr, "core run %d\n", st); return fail("the cartridge finished sending"); }
    if (got.n != 1 || got.h != 48) { fprintf(stderr, "strips %d h %d, last replies %02X %02X buf %u\n", got.n, got.h, replies[n - 2], replies[n - 1], p->buf_len); return fail("the strip from the core's serial port"); }
    /* the status answers: the last packet is a status inquiry, answered
     * 81 then 06 (still busy) or 04 */
    if (replies[n - 2] != 0x81) return fail("acknowledge through the core");
    if (replies[n - 1] != 0x06 && replies[n - 1] != 0x04) return fail("status through the core");
    puts("through the serial port: the same strip, 81 + status answered");
  }
  gbprn_free(p);
  free(p);
  free(got.px);
  puts("gbprinter: ok");
  return 0;
}
