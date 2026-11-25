/* GAME BOY PRINTER on the serial port (Game Boy Camera, Pokemon's Pokedex
 * print, Zelda DX's photos and others).
 *
 * The printer is a device on the link cable that answers the game's bytes:
 * the game is the clock, so the byte the printer returns for each byte it is
 * sent is all there is to emulate.  Written for this project from the public
 * protocol description (Pan Docs, "Game Boy Printer"); no code from another
 * emulator.
 *
 *   packet: 88 33 | command | compression | length (2, little endian) |
 *           data | checksum (2: the sum of command .. data) | the printer
 *           answers 81 | then its status byte.  Every other byte it answers 00.
 *   commands: 1 initialise (clears the buffer), 4 data (appended to the 8 KiB
 *           buffer, optionally run-length coded), 2 print (sheets, margins,
 *           palette, exposure), 8 break, F status inquiry.
 *   run-length coding: a control byte below 0x80 is followed by that many
 *           plus one literal bytes; 0x80 and above by one byte repeated the
 *           control byte's low 7 bits plus two times.
 *   status: bit 0 checksum error, 1 printing, 2 print finished / buffer full,
 *           3 data waiting to be printed, 4 packet error.
 *
 * A print puts the buffered tiles (20 per row) through the print command's
 * palette onto a strip of paper.  A print whose bottom margin is not zero
 * ends the strip (the paper is torn off): the finished strip goes to the
 * `done` callback as one byte per pixel, 0 = white .. 3 = black, 160 wide.
 * A strip left open (a game printing a long picture in bands) is closed
 * after two seconds without a packet, and by gbprn_flush.  One margin step is
 * 8 pixels of paper (an estimate: the documentation gives no length), and a
 * print of several sheets is delivered once. */
#ifndef FE_GBPRINTER_H
#define FE_GBPRINTER_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define GBPRN_W          160
#define GBPRN_BUF        0x2000          /* the printer's RAM */
#define GBPRN_MAX_ROWS   4096            /* a strip longer than this is closed */
#define GBPRN_MARGIN_PX  8
#define GBPRN_PACKET_IDLE 8              /* frames mid-packet before it is dropped */
#define GBPRN_STRIP_IDLE 120             /* frames before an open strip is closed */

enum
{
   GBPRN_ST_CHECKSUM = 0x01,
   GBPRN_ST_BUSY     = 0x02,
   GBPRN_ST_DONE     = 0x04,
   GBPRN_ST_READY    = 0x08,
   GBPRN_ST_PACKET   = 0x10
};

typedef struct gbprn
{
   /* the packet in progress */
   int      state;        /* 0 88, 1 33, 2 cmd, 3 comp, 4 len lo, 5 len hi, 6 data, 7 sum lo, 8 sum hi, 9 ack, 10 status */
   uint8_t  cmd, comp;
   unsigned len, got;
   unsigned sum, want;
   unsigned staged;       /* decoded bytes of this packet, past buf_len */
   int      rle_left;     /* RLE: literal bytes left, or run length to give */
   int      rle_run;      /* 1: the next byte is the run's value */
   uint8_t  args[4];      /* the print command's data */
   unsigned args_n;
   /* the printer */
   uint8_t  buf[GBPRN_BUF];
   unsigned buf_len;
   uint8_t  status;
   unsigned busy_left;    /* frames until a print is finished */
   unsigned idle;         /* frames since the last byte */
   /* the strip of paper */
   uint8_t *strip;
   unsigned rows, cap;
   unsigned strip_idle;
   /* where finished strips go */
   void   (*done)(void *user, const uint8_t *shades, int w, int h);
   void    *user;
} gbprn;

static inline void gbprn_init(gbprn *p, void (*done)(void *, const uint8_t *, int, int),
                              void *user)
{
   memset(p, 0, sizeof(*p));
   p->done = done;
   p->user = user;
}

static inline void gbprn_free(gbprn *p)
{
   free(p->strip);
   p->strip = NULL;
   p->rows = p->cap = 0;
}

/* Closes an open strip: hands it over and clears it. */
static inline void gbprn_flush(gbprn *p)
{
   if (p->rows && p->strip && p->done)
      p->done(p->user, p->strip, GBPRN_W, (int)p->rows);
   p->rows = 0;
   p->strip_idle = 0;
}

static inline int gbprn_add_rows(gbprn *p, unsigned n, uint8_t **first)
{
   if (p->rows + n > GBPRN_MAX_ROWS)
      return 0;
   if (p->rows + n > p->cap)
   {
      unsigned cap = p->cap ? p->cap : 256;
      uint8_t *s;
      while (cap < p->rows + n)
         cap *= 2;
      s = (uint8_t *)realloc(p->strip, (size_t)cap * GBPRN_W);
      if (!s)
         return 0;
      p->strip = s;
      p->cap = cap;
   }
   *first = p->strip + (size_t)p->rows * GBPRN_W;
   memset(*first, 0, (size_t)n * GBPRN_W);
   p->rows += n;
   return 1;
}

static inline void gbprn_print(gbprn *p)
{
   const unsigned sheets = p->args[0], margin = p->args[1], pal = p->args[2];
   const unsigned before = (margin >> 4) * GBPRN_MARGIN_PX;
   const unsigned after = (margin & 15) * GBPRN_MARGIN_PX;
   const unsigned tiles = p->buf_len / 16, trows = (tiles + 19) / 20;
   uint8_t *row;
   unsigned t, y, x;

   p->strip_idle = 0;
   if (p->rows + before + trows * 8 + after > GBPRN_MAX_ROWS)
      gbprn_flush(p);          /* a strip this long is closed to start another */
   if (before)
      (void)gbprn_add_rows(p, before, &row);
   if (tiles && gbprn_add_rows(p, trows * 8, &row))
   {
      for (t = 0; t < tiles; t++)
      {
         const uint8_t *tile = p->buf + t * 16;
         uint8_t *dst = row + (size_t)(t / 20) * 8 * GBPRN_W + (t % 20) * 8;
         for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++)
            {
               unsigned c = ((tile[y * 2] >> (7 - x)) & 1) |
                            (((tile[y * 2 + 1] >> (7 - x)) & 1) << 1);
               dst[(size_t)y * GBPRN_W + x] = (uint8_t)((pal >> (c * 2)) & 3);
            }
      }
   }
   if (after)
      (void)gbprn_add_rows(p, after, &row);
   p->buf_len = 0;
   p->status &= (uint8_t)~GBPRN_ST_READY;
   if (sheets || tiles)
   {
      p->status |= GBPRN_ST_BUSY | GBPRN_ST_DONE;
      p->busy_left = 10 + trows * 4;
   }
   if (after)
      gbprn_flush(p);      /* the paper is torn off */
}

/* A packet has arrived whole: run it. */
static inline void gbprn_command(gbprn *p)
{
   if (p->want != (p->sum & 0xFFFF))
   {
      p->status |= GBPRN_ST_CHECKSUM;
      return;
   }
   p->status &= (uint8_t)~GBPRN_ST_CHECKSUM;
   switch (p->cmd)
   {
      case 1:                       /* initialise */
         p->buf_len = 0;
         p->status = 0;
         p->busy_left = 0;
         break;
      case 4:                       /* data */
         p->status &= (uint8_t)~GBPRN_ST_DONE;
         p->buf_len += p->staged;
         if (p->buf_len)
            p->status |= GBPRN_ST_READY;
         break;
      case 2:                       /* print */
         if (p->args_n >= 4 && !(p->status & GBPRN_ST_BUSY))
            gbprn_print(p);
         break;
      case 8:                       /* break */
         p->busy_left = 0;
         p->status &= (uint8_t)~(GBPRN_ST_BUSY | GBPRN_ST_DONE);
         break;
      default:                      /* F: the status only */
         break;
   }
}

/* One data byte of the packet. */
static inline void gbprn_data(gbprn *p, uint8_t b)
{
   p->sum += b;
   if (p->cmd == 2)
   {
      if (p->args_n < 4)
         p->args[p->args_n++] = b;
      return;
   }
   if (p->cmd != 4)
      return;
   if (!p->comp)
   {
      if (p->buf_len + p->staged < GBPRN_BUF)
         p->buf[p->buf_len + p->staged++] = b;
      return;
   }
   if (p->rle_run)
   {
      int n = p->rle_left;
      p->rle_run = 0;
      p->rle_left = 0;
      while (n-- > 0 && p->buf_len + p->staged < GBPRN_BUF)
         p->buf[p->buf_len + p->staged++] = b;
   }
   else if (p->rle_left > 0)
   {
      p->rle_left--;
      if (p->buf_len + p->staged < GBPRN_BUF)
         p->buf[p->buf_len + p->staged++] = b;
   }
   else if (b & 0x80)
   {
      p->rle_run = 1;
      p->rle_left = (b & 0x7F) + 2;
   }
   else
      p->rle_left = b + 1;
}

/* The game sent `tx`; returns what the printer answers. */
static inline uint8_t gbprn_byte(gbprn *p, uint8_t tx)
{
   uint8_t rx = 0;
   p->idle = 0;
   switch (p->state)
   {
      case 0: p->state = tx == 0x88 ? 1 : 0; break;
      case 1: p->state = tx == 0x33 ? 2 : tx == 0x88 ? 1 : 0; break;
      case 2:
         p->cmd = tx; p->sum = tx; p->state = 3;
         break;
      case 3:
         p->comp = tx & 1; p->sum += tx; p->state = 4;
         break;
      case 4:
         p->len = tx; p->sum += tx; p->state = 5;
         break;
      case 5:
         p->len |= (unsigned)tx << 8; p->sum += tx; p->got = 0;
         p->staged = 0; p->rle_left = 0; p->rle_run = 0; p->args_n = 0;
         p->state = p->len ? 6 : 7;
         break;
      case 6:
         gbprn_data(p, tx);
         if (++p->got >= p->len)
            p->state = 7;
         break;
      case 7: p->want = tx; p->state = 8; break;
      case 8:
         p->want |= (unsigned)tx << 8;
         gbprn_command(p);
         p->state = 9;
         break;
      case 9: rx = 0x81; p->state = 10; break;
      default:
         rx = p->status;
         p->state = 0;
         break;
   }
   return rx;
}

/* Once a frame: a print finishes after a while, a packet that stops
 * half-way is dropped, an open strip is closed after a pause. */
static inline void gbprn_frame(gbprn *p)
{
   if (p->busy_left && --p->busy_left == 0)
      p->status &= (uint8_t)~GBPRN_ST_BUSY;
   if (p->state && p->state < 9 && ++p->idle > GBPRN_PACKET_IDLE)
   {
      p->state = 0;
      p->idle = 0;
   }
   if (p->rows && ++p->strip_idle >= GBPRN_STRIP_IDLE)
      gbprn_flush(p);
}

/* The strip as an RGB picture through four colours (0 = paper). */
static inline void gbprn_rgb(const uint8_t *shades, int w, int h,
                             const uint32_t colour[4], uint8_t *rgb)
{
   int i;
   for (i = 0; i < w * h; i++)
   {
      uint32_t c = colour[shades[i] & 3];
      rgb[i * 3 + 0] = (uint8_t)(c >> 16);
      rgb[i * 3 + 1] = (uint8_t)(c >> 8);
      rgb[i * 3 + 2] = (uint8_t)c;
   }
}

#endif
