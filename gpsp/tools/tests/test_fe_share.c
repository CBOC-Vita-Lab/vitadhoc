/* test_fe_share.c -- "Share game data" (frontend-common/fe_share.c,
 * docs/SAVE-SHARE.md): the protocol and the file-safety rules, with both
 * ends in one process.
 *
 *   1. which files are game data, and which ROM owns them (classify);
 *   2. the game identity: same image under another name matches, a hack on
 *      the same header does not;
 *   3. the manifest: round trip, and every hostile blob refused;
 *   4. transfers over a fake radio at 0 / 10 / 30 % loss with duplicates
 *      and reordering: manifest, save, state + thumb + shot, an empty file
 *      and a file of exactly N chunks, byte-identical on arrival;
 *   5. corruption in the air (dropped by the datagram CRC, re-asked) and
 *      corruption the datagram CRC cannot see (the sharer's file changing
 *      between its checksum and its chunks): the fetch FAILS, nothing is
 *      committed, the joiner's own file is untouched;
 *   6. truncation: the sharer dies mid-transfer -> LOST, original intact;
 *      the sharer stops -> CLOSED; the file shrinks -> GONE;
 *   7. hostile requests: 200,000 well-formed-but-wrong and random datagrams
 *      at the server; it only ever opens objects its own manifest offers,
 *      and its io has no write at all;  random datagrams at the client;
 *   8. mixed versions: another protocol generation is REFUSED by name, a
 *      different build keeps saves and loses states, a second guest is BUSY,
 *      a repeated HELLO keeps its session;
 *   9. commit: exactly one .bak per save, a state keeps none, a failed
 *      rename restores the old file, a full stick (write fails) leaves the
 *      original and no temp. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "fe_share.h"

static int checks;
#define CHECK(c) do { checks++; if (!(c)) { \
   fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
   exit(1); } } while (0)

static uint32_t rng_s = 0x1234567u;
static uint32_t rnd(void)
{
   rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5;
   return rng_s;
}

/* ---- 1. classify ---------------------------------------------------------- */
static void t_classify(void)
{
   static const struct { const char *n; int ok, con, kind, slot; size_t key; }
   T[] = {
      { "Emerald.sav",            1, 0, FE_SHARE_OBJ_SAVE,  0, 7 },
      { "Pokemon/Emerald.SAV",    1, 0, FE_SHARE_OBJ_SAVE,  0, 15 },
      { "Red.gb.sav",             1, 1, FE_SHARE_OBJ_SAVE,  0, 6 },
      { "Crystal.gbc.sav",        1, 2, FE_SHARE_OBJ_SAVE,  0, 11 },
      { "Emerald.st0",            1, 0, FE_SHARE_OBJ_STATE, 1, 7 },
      { "Emerald.st4",            1, 0, FE_SHARE_OBJ_STATE, 5, 7 },
      { "Emerald.st5",            0, 0, 0, 0, 0 },
      { "Emerald.st2.thumb",      1, 0, FE_SHARE_OBJ_THUMB, 3, 7 },
      { "Emerald.st2.shot",       1, 0, FE_SHARE_OBJ_SHOT,  3, 7 },
      { "Red.gb.st1",             1, 1, FE_SHARE_OBJ_STATE, 2, 6 },
      { "Crystal.GBC.ST3.THUMB",  1, 2, FE_SHARE_OBJ_THUMB, 4, 11 },
      { "Emerald.gba",            0, 0, 0, 0, 0 },
      { "Red.gb",                 0, 0, 0, 0, 0 },
      { "Emerald.sav.bak",        0, 0, 0, 0, 0 },
      { "Emerald.sav.gsp",        0, 0, 0, 0, 0 },
      { "Emerald.sav.thumb",      0, 0, 0, 0, 0 },
      { ".sav",                   0, 0, 0, 0, 0 },
      { "dir/.st0",               0, 0, 0, 0, 0 },
      { ".gb.sav",                0, 0, 0, 0, 0 },
      { "favourites.txt",         0, 0, 0, 0, 0 },
      { "",                       0, 0, 0, 0, 0 },
   };
   unsigned i;
   for (i = 0; i < sizeof(T) / sizeof(T[0]); i++)
   {
      int con = -1, kind = -1, slot = -1;
      size_t key = 0;
      int ok = fe_share_classify(T[i].n, &con, &kind, &slot, &key);
      CHECK(ok == T[i].ok);
      if (ok)
      {
         CHECK(con == T[i].con && kind == T[i].kind && slot == T[i].slot);
         CHECK(key == T[i].key);
      }
   }
   CHECK(!fe_share_classify(NULL, NULL, NULL, NULL, NULL));
   CHECK(fe_share_key_matches_rom("Emerald", 7, 0, "emerald.GBA"));
   CHECK(!fe_share_key_matches_rom("Emerald", 7, 0, "Emerald2.gba"));
   CHECK(!fe_share_key_matches_rom("Emerald", 7, 0, "Emerald.gb"));
   CHECK(fe_share_key_matches_rom("Red.gb", 6, 1, "RED.GB"));
   CHECK(!fe_share_key_matches_rom("Red.gb", 6, 2, "Red.gb"));
   CHECK(!fe_share_key_matches_rom("Red.gbc", 7, 1, "Red.gbc"));
   CHECK(fe_share_key_matches_rom("Red.gbc", 7, 2, "Red.gbc"));
}

/* ---- 2. identity ---------------------------------------------------------- */
typedef struct { const uint8_t *p; uint32_t n; } mem_t;
static int mem_read(void *u, uint32_t off, void *buf, uint32_t len)
{
   mem_t *m = (mem_t *)u;
   if (off > m->n || len > m->n - off)
      return -1;
   memcpy(buf, m->p + off, len);
   return (int)len;
}

static void t_identity(void)
{
   uint32_t n = 1u << 20, i;
   uint8_t *rom = malloc(n), *hack = malloc(n);
   uint8_t a[FE_SHARE_ID_LEN], b[FE_SHARE_ID_LEN], c[FE_SHARE_ID_LEN];
   mem_t m;
   for (i = 0; i < n; i++)
      rom[i] = (uint8_t)rnd();
   memcpy(rom + 0xAC, "BPEE", 4);
   memcpy(hack, rom, n);
   m.p = rom; m.n = n;
   CHECK(fe_share_game_id(0, n, mem_read, &m, a) == 0);
   CHECK(fe_share_game_id(0, n, mem_read, &m, b) == 0);
   CHECK(memcmp(a, b, sizeof(a)) == 0);          /* same image, any name */
   CHECK(memcmp(a + 12, "BPEE", 4) == 0);
   /* a hack: same header, same size, other contents */
   for (i = 0x1000; i < n; i++)
      hack[i] ^= 0x5A;
   m.p = hack;
   CHECK(fe_share_game_id(0, n, mem_read, &m, c) == 0);
   CHECK(memcmp(a, c, sizeof(a)) != 0);
   /* another size */
   m.p = rom; m.n = n / 2;
   CHECK(fe_share_game_id(0, n / 2, mem_read, &m, c) == 0);
   CHECK(memcmp(a, c, sizeof(a)) != 0);
   /* the same bytes as another console are another game */
   m.n = n;
   CHECK(fe_share_game_id(1, n, mem_read, &m, c) == 0);
   CHECK(memcmp(a, c, sizeof(a)) != 0);
   /* too short to hold a header; a short read */
   CHECK(fe_share_game_id(0, 0x40, mem_read, &m, c) != 0);
   m.n = 0x80;
   CHECK(fe_share_game_id(0, n, mem_read, &m, c) != 0);
   CHECK(fe_share_game_id(3, n, mem_read, &m, c) != 0);
   free(rom);
   free(hack);
}

/* ---- 3. manifest ---------------------------------------------------------- */
static void fill_entry(fe_share_entry *e, int con, unsigned seed)
{
   int k;
   memset(e, 0, sizeof(*e));
   e->console = (uint8_t)con;
   for (k = 0; k < FE_SHARE_ID_LEN; k++)
      e->id[k] = (uint8_t)(seed * 7 + k);
   e->save_size = 131072;
   e->save_time = fe_share_time_pack(2026, 10, 9, 21, 5);
   for (k = 0; k < FE_SHARE_SLOTS; k++)
      if ((seed >> k) & 1)
      {
         e->st[k].size = 400000 + seed + k;
         e->st[k].time = fe_share_time_pack(2026, 10, 1 + k, 8, 44);
         e->st[k].flags = FE_SHARE_ST_THUMB | FE_SHARE_ST_SHOT;
      }
   snprintf(e->name, sizeof(e->name), "Game %u", seed);
}

static void t_manifest(void)
{
   fe_share_entry in[40], out[40];
   uint8_t blob[8 + 40 * FE_SHARE_ENTRY_WIRE], bad[sizeof(blob)];
   size_t n;
   unsigned i, it;
   char t[24];
   for (i = 0; i < 40; i++)
      fill_entry(&in[i], (int)(i % 3), i + 1);
   n = fe_share_manifest_build(in, 40, blob, sizeof(blob));
   CHECK(n == fe_share_manifest_size(40));
   CHECK(fe_share_manifest_parse(blob, n, out, 40) == 40);
   CHECK(memcmp(in, out, sizeof(in)) == 0);
   CHECK(fe_share_manifest_build(in, 40, blob, n - 1) == 0);
   /* zero entries */
   CHECK(fe_share_manifest_build(NULL, 0, bad, sizeof(bad)) == 8);
   CHECK(fe_share_manifest_parse(bad, 8, out, 40) == 0);
   /* hostile: length, magic, count, console, sizes, flags, more than asked */
   CHECK(fe_share_manifest_parse(blob, n - 1, out, 40) == -1);
   CHECK(fe_share_manifest_parse(blob, 7, out, 40) == -1);
   CHECK(fe_share_manifest_parse(blob, n, out, 39) == -1);
   memcpy(bad, blob, n); bad[0] = 'X';
   CHECK(fe_share_manifest_parse(bad, n, out, 40) == -1);
   memcpy(bad, blob, n); bad[4] = 41;
   CHECK(fe_share_manifest_parse(bad, n, out, 40) == -1);
   memcpy(bad, blob, n); bad[8] = 3;
   CHECK(fe_share_manifest_parse(bad, n, out, 40) == -1);
   memcpy(bad, blob, n); bad[8 + 17 + 3] = 0x7F;       /* save 2 GiB */
   CHECK(fe_share_manifest_parse(bad, n, out, 40) == -1);
   memcpy(bad, blob, n); bad[8 + 33] = 0x80;           /* unknown flag */
   CHECK(fe_share_manifest_parse(bad, n, out, 40) == -1);
   /* a name with no NUL and control bytes comes out printable and ended */
   memcpy(bad, blob, n);
   memset(bad + 8 + 70, 0x01, FE_SHARE_NAME_CAP);
   CHECK(fe_share_manifest_parse(bad, n, out, 40) == 40);
   CHECK(strlen(out[0].name) == FE_SHARE_NAME_CAP - 1 && out[0].name[0] == '?');
   /* a preview flag on an empty slot is dropped */
   memcpy(bad, blob, n);
   memset(bad + 8 + 25, 0, 8); bad[8 + 33] = 3;
   CHECK(fe_share_manifest_parse(bad, n, out, 40) == 40);
   CHECK(out[0].st[0].size == 0 && out[0].st[0].flags == 0);
   /* fuzz: never crashes (ASAN), and whatever is accepted is in range */
   for (it = 0; it < 20000; it++)
   {
      int r, k;
      memcpy(bad, blob, n);
      for (k = 0; k < 6; k++)
         bad[rnd() % n] = (uint8_t)rnd();
      r = fe_share_manifest_parse(bad, (rnd() & 7) ? n : rnd() % (n + 1),
                                  out, 40);
      for (i = 0; r > 0 && i < (unsigned)r; i++)
      {
         CHECK(out[i].console < FE_SHARE_CONSOLES);
         CHECK(out[i].save_size <= FE_SHARE_MAX_OBJECT);
         CHECK(out[i].name[FE_SHARE_NAME_CAP - 1] == '\0');
      }
   }
   CHECK(fe_share_match(in, 40, in[7].console, in[7].id) == 7);
   CHECK(fe_share_match(in, 40, (in[7].console + 1) % 3, in[7].id) == -1);
   fe_share_time_text(fe_share_time_pack(2026, 10, 9, 21, 5), 1, t, sizeof(t));
   CHECK(strcmp(t, "9 Oct, 21:05") == 0);
   fe_share_time_text(fe_share_time_pack(2026, 10, 9, 21, 5), 0, t, sizeof(t));
   CHECK(strcmp(t, "9 Oct") == 0);
   fe_share_time_text(0, 1, t, sizeof(t));
   CHECK(t[0] == '\0');
   CHECK(fe_share_time_pack(2026, 13, 1, 0, 0) == 0);
}

/* ---- the rig: two consoles, a fake radio, a real directory ---------------- */
#define NQ 4096
typedef struct { uint8_t d[FE_SHARE_DGRAM_MAX]; size_t n; int to_srv; } pkt;
static pkt  *Q;
static int   q_n;
static int   loss_pct, dup_pct, reorder, corrupt_pct;
static int   radio_dead;            /* 1 = server -> client is silent */
static uint32_t dropped, corrupted;

static char  DIRA[256], DIRB[256];   /* the sharer's stick, the joiner's */

static void q_push(int to_srv, const void *buf, size_t len)
{
   pkt *p;
   CHECK(len <= FE_SHARE_DGRAM_MAX);
   if ((int)(rnd() % 100) < loss_pct) { dropped++; return; }
   if (radio_dead && !to_srv)
      return;
   CHECK(q_n < NQ);
   p = &Q[q_n++];
   memcpy(p->d, buf, len);
   p->n = len;
   p->to_srv = to_srv;
   if ((int)(rnd() % 100) < corrupt_pct)
   {
      p->d[rnd() % len] ^= (uint8_t)(1u << (rnd() & 7));
      corrupted++;
   }
   if ((int)(rnd() % 100) < dup_pct && q_n < NQ)
   {
      Q[q_n] = *p;
      q_n++;
   }
   if (reorder && q_n >= 2 && (rnd() & 3) == 0)
   {
      pkt t = Q[q_n - 1];
      Q[q_n - 1] = Q[q_n - 2];
      Q[q_n - 2] = t;
   }
}

/* the sharer's files: entry -> a name table the CLIENT never sees */
typedef struct
{
   fe_share_entry e[8];
   const char *stem[8];             /* DIRA/<stem>.<suffix> */
   unsigned n;
   FILE *f;
   unsigned opens, bad_opens;
   int shrink_after;                /* reads before the file "shrinks" */
} srv_fs;
static srv_fs SF;
static fe_share_server SRV;
static fe_share_client CL, CL2;

static void srv_path(fe_share_obj o, char *out, size_t n)
{
   const char *stem = SF.stem[o.entry];
   if (o.kind == FE_SHARE_OBJ_SAVE)
      snprintf(out, n, "%s/%s.sav", DIRA, stem);
   else
      snprintf(out, n, "%s/%s.st%d%s", DIRA, stem, o.slot - 1,
               o.kind == FE_SHARE_OBJ_THUMB ? ".thumb" :
               o.kind == FE_SHARE_OBJ_SHOT ? ".shot" : "");
}

static void s_send(void *u, const uint8_t mac[6], const void *b, size_t n)
{
   (void)u; (void)mac;
   q_push(0, b, n);
}
static int s_open(void *u, fe_share_obj o, uint32_t *size)
{
   char p[512];
   (void)u;
   SF.opens++;
   /* THE INVARIANT of test 7: the server never asks the platform for
    * anything its own manifest does not offer. */
   if (!fe_share_obj_valid(SF.e, SF.n, o) || o.kind == FE_SHARE_OBJ_MANIFEST)
   {
      SF.bad_opens++;
      return -1;
   }
   CHECK(SF.f == NULL);
   srv_path(o, p, sizeof(p));
   SF.f = fopen(p, "rb");
   if (!SF.f)
      return -1;
   fseek(SF.f, 0, SEEK_END);
   *size = (uint32_t)ftell(SF.f);
   return 0;
}
static int s_read(void *u, uint32_t off, void *buf, uint32_t len)
{
   (void)u;
   if (!SF.f)
      return -1;
   if (SF.shrink_after > 0 && --SF.shrink_after == 0)
      return 0;
   fseek(SF.f, (long)off, SEEK_SET);
   return (int)fread(buf, 1, len, SF.f);
}
static void s_close(void *u)
{
   (void)u;
   if (SF.f)
      fclose(SF.f);
   SF.f = NULL;
}

static void c_send(void *u, const uint8_t mac[6], const void *b, size_t n)
{
   (void)mac;
   if (u == &CL2)
      q_push(2, b, n);       /* the second guest: to the server, marked */
   else
      q_push(1, b, n);
}

/* the joiner's sink: a temp FILE (or memory), with an optional full stick */
typedef struct { FILE *f; uint8_t *mem; uint32_t cap, full_at, written; } snk;
static int k_write(void *u, uint32_t off, const void *buf, uint32_t len)
{
   snk *s = (snk *)u;
   s->written += len;
   if (s->full_at && s->written > s->full_at)
      return -1;
   if (s->mem)
   {
      if (off + len > s->cap)
         return -1;
      memcpy(s->mem + off, buf, len);
      return 0;
   }
   fseek(s->f, (long)off, SEEK_SET);
   return fwrite(buf, 1, len, s->f) == len ? 0 : -1;
}
static int k_read(void *u, uint32_t off, void *buf, uint32_t len)
{
   snk *s = (snk *)u;
   if (s->mem)
   {
      memcpy(buf, s->mem + off, len);
      return (int)len;
   }
   fflush(s->f);
   fseek(s->f, (long)off, SEEK_SET);
   return (int)fread(buf, 1, len, s->f);
}

static uint32_t NOW;
static uint32_t cl2_refused;

/* One "frame": deliver what is in the air, step both ends, 16 ms pass. */
static void frame(unsigned budget)
{
   int i, n = q_n;
   static pkt batch[NQ];
   memcpy(batch, Q, (size_t)n * sizeof(pkt));
   q_n = 0;
   for (i = 0; i < n; i++)
   {
      if (batch[i].to_srv)
         fe_share_server_rx(&SRV, batch[i].d, batch[i].n, NOW);
      else
      {
         fe_share_client_rx(&CL, batch[i].d, batch[i].n, NOW);
         if (CL2.state != FE_SHARE_CL_IDLE)
            fe_share_client_rx(&CL2, batch[i].d, batch[i].n, NOW);
      }
   }
   fe_share_server_step(&SRV, NOW, budget);
   fe_share_client_step(&CL, NOW);
   if (CL2.state != FE_SHARE_CL_IDLE)
      fe_share_client_step(&CL2, NOW);
   NOW += 16;
}

static void put_file(const char *dir, const char *name, uint32_t size,
                     unsigned seed)
{
   char p[512];
   FILE *f;
   uint32_t i;
   snprintf(p, sizeof(p), "%s/%s", dir, name);
   f = fopen(p, "wb");
   CHECK(f != NULL);
   for (i = 0; i < size; i++)
      fputc((int)((i * 2654435761u + seed * 97u) >> 13) & 0xFF, f);
   fclose(f);
}

/* A file with the head of a GBA state of format `ver` (savestate.c): the
 * rest is put_file's noise, as far as the share is concerned. */
static void put_gba_state(const char *dir, const char *name, uint32_t size,
                          unsigned seed, uint32_t ver)
{
   static const uint8_t magic[4] = { 0xDE, 0xC0, 0xBA, 0x06 };
   uint8_t h[38], *p = h;
   char path[512];
   FILE *f;
   put_file(dir, name, size, seed);
   *p++ = (uint8_t)size; *p++ = (uint8_t)(size >> 8);
   *p++ = (uint8_t)(size >> 16); *p++ = (uint8_t)(size >> 24);
   *p++ = 0x10; memcpy(p, "info-magic", 11); p += 11;
   memcpy(p, magic, 4); p += 4;
   *p++ = 0x10; memcpy(p, "info-version", 13); p += 13;
   *p++ = (uint8_t)ver; *p++ = (uint8_t)(ver >> 8);
   *p++ = (uint8_t)(ver >> 16); *p++ = (uint8_t)(ver >> 24);
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   f = fopen(path, "r+b");
   CHECK(f != NULL);
   CHECK(fwrite(h, 1, sizeof(h), f) == sizeof(h));
   fclose(f);
}

static int same_file(const char *a, const char *b)
{
   FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
   int ca, cb, same = fa && fb;
   while (same)
   {
      ca = fgetc(fa); cb = fgetc(fb);
      if (ca != cb) same = 0;
      if (ca == EOF) break;
   }
   if (fa) fclose(fa);
   if (fb) fclose(fb);
   return same;
}

static int exists(const char *p)
{
   struct stat st;
   return stat(p, &st) == 0;
}

static uint32_t file_crc(const char *p)
{
   FILE *f = fopen(p, "rb");
   uint8_t b[4096];
   size_t n;
   uint32_t c = 0;
   CHECK(f != NULL);
   while ((n = fread(b, 1, sizeof(b), f)) > 0)
      c = fe_share_crc32(c, b, n);
   fclose(f);
   return c;
}

static fe_share_ident ident(const char *build, int mac)
{
   fe_share_ident id;
   memset(&id, 0, sizeof(id));
   snprintf(id.build, sizeof(id.build), "%s", build);
   id.state_fmt[0] = 0x00010004; id.state_fmt[1] = 7; id.state_fmt[2] = 7;
   id.mac[0] = 2; id.mac[5] = (uint8_t)mac;
   return id;
}

static uint8_t MANIFEST[8 + 8 * FE_SHARE_ENTRY_WIRE];
static uint32_t MANIFEST_N;

/* The sharer's stick: three games.  Entry 0 Emerald (GBA): save + slots
 * 1,2,3 with previews; entry 1 Red (GB): save + slot 1, no previews; entry
 * 2 Kart (GBA): no save, slot 4, an EMPTY state file and a state of exactly
 * 3 chunks. */
static void rig_files(void)
{
   put_file(DIRA, "Emerald.sav", 131072, 1);
   put_file(DIRA, "Emerald.st0", 416321, 2);
   put_file(DIRA, "Emerald.st0.thumb", 5392, 3);
   put_file(DIRA, "Emerald.st0.shot", 81936, 4);
   put_file(DIRA, "Emerald.st1", 416321, 5);
   put_file(DIRA, "Emerald.st1.thumb", 5392, 6);
   put_file(DIRA, "Emerald.st2", 3 * 1024, 7);
   put_gba_state(DIRA, "Emerald.st3", FE_SHARE_GBA_STATE_SIZE, 11, 0x00010004);
   put_gba_state(DIRA, "Emerald.st4", FE_SHARE_GBA_STATE_SIZE, 12, 0x00010003);
   put_file(DIRA, "Red.gb.sav", 32816, 8);
   put_file(DIRA, "Red.gb.st0", 50000, 9);
   put_file(DIRA, "Kart.st3", 0, 10);
}

static void rig_up(const char *srv_build, const char *cl_build)
{
   static const fe_share_server_io sio = { NULL, s_send, s_open, s_read,
                                           s_close };
   fe_share_client_io cio = { &CL, c_send };
   fe_share_ident a = ident(srv_build, 1), b = ident(cl_build, 2);
   memset(&SF, 0, sizeof(SF));
   SF.n = 3;
   SF.stem[0] = "Emerald"; SF.stem[1] = "Red.gb"; SF.stem[2] = "Kart";
   fill_entry(&SF.e[0], 0, 0);
   SF.e[0].st[0].size = 416321; SF.e[0].st[0].flags = 3;
   SF.e[0].st[1].size = 416321; SF.e[0].st[1].flags = 1;
   SF.e[0].st[2].size = 3072;
   SF.e[0].st[3].size = FE_SHARE_GBA_STATE_SIZE;
   SF.e[0].st[4].size = FE_SHARE_GBA_STATE_SIZE;
   fill_entry(&SF.e[1], 1, 0);
   SF.e[1].save_size = 32816; SF.e[1].st[0].size = 50000;
   SF.e[1].id[0] = 9;
   fill_entry(&SF.e[2], 0, 0);
   SF.e[2].save_size = 0; SF.e[2].st[3].size = 1;   /* manifest says 1 ... */
   SF.e[2].id[0] = 7;
   MANIFEST_N = (uint32_t)fe_share_manifest_build(SF.e, SF.n, MANIFEST,
                                                  sizeof(MANIFEST));
   fe_share_server_init(&SRV, &sio, &a, SF.e, SF.n, MANIFEST, MANIFEST_N, 77);
   fe_share_client_init(&CL, &cio, &b, 0x1234);
   memset(&CL2, 0, sizeof(CL2));
   q_n = 0;
   loss_pct = dup_pct = reorder = corrupt_pct = radio_dead = 0;
   NOW = 1000;
}

static void rig_join(void)
{
   int i;
   fe_share_client_hello(&CL, NOW);
   for (i = 0; i < 2000 && CL.state == FE_SHARE_CL_HELLO; i++)
      frame(8);
   CHECK(CL.state == FE_SHARE_CL_READY);
   CHECK(SRV.state == FE_SHARE_SRV_CONNECTED);
   CHECK(CL.n_entries == 3 && CL.manifest_size == MANIFEST_N);
   CHECK(CL.n_saves == 2 && CL.n_states == 7);
}

/* fe_share_client_expect for the next fetch_file (0 = none) */
static uint32_t g_expect;
/* fe_share_client_expect_state: console + 1 (0 = none), and the format */
static int g_expect_state;
static uint32_t g_expect_fmt;

/* Fetch `o` into DIRB/<tmpname>; returns the client's final state. */
static int fetch_file(fe_share_obj o, const char *tmpname, uint32_t full_at,
                      int max_frames)
{
   char p[512];
   snk s;
   fe_share_sink sink;
   int i;
   memset(&s, 0, sizeof(s));
   snprintf(p, sizeof(p), "%s/%s", DIRB, tmpname);
   s.f = fopen(p, "w+b");
   CHECK(s.f != NULL);
   s.full_at = full_at;
   sink.user = &s; sink.write = k_write; sink.read = k_read; sink.cap = 0;
   CHECK(fe_share_client_fetch(&CL, o, &sink, NOW) == 0);
   if (g_expect)
      fe_share_client_expect(&CL, g_expect);
   if (g_expect_state)
      fe_share_client_expect_state(&CL, g_expect_state - 1, g_expect_fmt);
   for (i = 0; i < max_frames && (CL.state == FE_SHARE_CL_STAT ||
                                  CL.state == FE_SHARE_CL_DATA); i++)
      frame(8);
   fclose(s.f);
   return CL.state;
}

static fe_share_obj OBJ(int entry, int kind, int slot)
{
   fe_share_obj o;
   o.entry = (uint16_t)entry; o.kind = (uint8_t)kind; o.slot = (uint8_t)slot;
   return o;
}

static void join_paths(char *a, char *b, const char *an, const char *bn)
{
   snprintf(a, 512, "%s/%s", DIRA, an);
   snprintf(b, 512, "%s/%s", DIRB, bn);
}

/* ---- 4. transfers --------------------------------------------------------- */
/* fe_share_state_check alone: every field it reads, both formats. */
static void t_state_check(void)
{
   uint8_t g[40], b[40];
   const uint32_t S = FE_SHARE_GBA_STATE_SIZE, V = 0x00010004;
   memset(g, 0, sizeof(g));
   g[0] = (uint8_t)S; g[1] = (uint8_t)(S >> 8); g[2] = (uint8_t)(S >> 16);
   g[4] = 0x10; memcpy(g + 5, "info-magic", 11);
   g[16] = 0xDE; g[17] = 0xC0; g[18] = 0xBA; g[19] = 0x06;
   g[20] = 0x10; memcpy(g + 21, "info-version", 13);
   g[34] = 0x04; g[35] = 0x00; g[36] = 0x01; g[37] = 0x00;
   CHECK(fe_share_state_check(0, V, g, 40, S) == 1);
   CHECK(fe_share_state_check(0, V, g, 38, S) == 1);
   CHECK(fe_share_state_check(0, V, g, 37, S) == 0);       /* cut short */
   CHECK(fe_share_state_check(0, V, NULL, 40, S) == 0);
   CHECK(fe_share_state_check(0, V + 1, g, 40, S) == 0);   /* other format */
   CHECK(fe_share_state_check(0, V, g, 40, S - 1) == 0);   /* other size */
   CHECK(fe_share_state_check(0, V, g, 40, 0) == 0);
   CHECK(fe_share_state_check(1, V, g, 40, S) == 0);       /* not a GB one */
   CHECK(fe_share_state_check(3, V, g, 40, S) == 0);
   CHECK(fe_share_state_check(-1, V, g, 40, S) == 0);
   memcpy(b, g, 40); b[1] ^= 1;                            /* doc length */
   CHECK(fe_share_state_check(0, V, b, 40, S) == 0);
   memcpy(b, g, 40); b[4] = 0x05;                          /* element type */
   CHECK(fe_share_state_check(0, V, b, 40, S) == 0);
   memcpy(b, g, 40); b[9] = 'M';                           /* key */
   CHECK(fe_share_state_check(0, V, b, 40, S) == 0);
   memcpy(b, g, 40); b[16] = 0;                            /* magic */
   CHECK(fe_share_state_check(0, V, b, 40, S) == 0);
   memcpy(b, g, 40); b[25] = 'x';                          /* version key */
   CHECK(fe_share_state_check(0, V, b, 40, S) == 0);
   CHECK(fe_share_state_size_ok(0, S) && !fe_share_state_size_ok(0, S + 1));

   /* GB / GBC: 40 + core + 96 */
   memset(g, 0, sizeof(g));
   memcpy(g, "GBAT", 4); g[4] = 1; g[6] = 1; g[7] = 1;
   g[36] = 0x10; g[37] = 0x27;                             /* core 10000 */
   CHECK(fe_share_state_check(1, 1, g, 40, 10136) == 1);
   CHECK(fe_share_state_check(2, 1, g, 40, 10136) == 0);   /* GB, not GBC */
   CHECK(fe_share_state_check(1, 1, g, 39, 10136) == 0);
   CHECK(fe_share_state_check(1, 1, g, 40, 10137) == 0);   /* size disagrees */
   CHECK(fe_share_state_check(1, 1, g, 40, 136) == 0);
   memcpy(b, g, 40); b[0] = 'X';
   CHECK(fe_share_state_check(1, 1, b, 40, 10136) == 0);
   memcpy(b, g, 40); b[4] = 2;                             /* version */
   CHECK(fe_share_state_check(1, 1, b, 40, 10136) == 0);
   g[6] = 2;
   CHECK(fe_share_state_check(2, 1, g, 40, 10136) == 1);
   CHECK(!fe_share_state_size_ok(1, 136) && fe_share_state_size_ok(1, 137));
   CHECK(!fe_share_state_size_ok(2, FE_SHARE_GBA_STATE_SIZE + 1));
}

static void t_transfers(int loss, int dup, int reo)
{
   static uint8_t mbuf[sizeof(MANIFEST)];
   fe_share_entry got[8];
   char a[512], b[512];
   snk ms;
   fe_share_sink sink;
   int i;
   rig_up("3.5.0-abc1234", "3.5.0-abc1234");
   loss_pct = loss; dup_pct = dup; reorder = reo;
   rig_join();
   CHECK(fe_share_states_ok(&CL.me, &CL.host, 0));
   /* the manifest, into memory */
   memset(&ms, 0, sizeof(ms));
   ms.mem = mbuf; ms.cap = sizeof(mbuf);
   sink.user = &ms; sink.write = k_write; sink.read = k_read;
   sink.cap = sizeof(mbuf);
   CHECK(fe_share_client_fetch(&CL, OBJ(0, FE_SHARE_OBJ_MANIFEST, 0), &sink,
                               NOW) == 0);
   for (i = 0; i < 20000 && CL.state != FE_SHARE_CL_DONE; i++)
   {
      CHECK(CL.state != FE_SHARE_CL_ERROR);
      frame(8);
   }
   CHECK(CL.state == FE_SHARE_CL_DONE && CL.size == MANIFEST_N);
   CHECK(fe_share_manifest_parse(mbuf, CL.size, got, 8) == 3);
   CHECK(memcmp(got, SF.e, 3 * sizeof(got[0])) == 0);
   /* a save, a big state, its previews, a 3-chunk state, a GB save */
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_SAVE, 0), "t.sav", 0, 200000)
         == FE_SHARE_CL_DONE);
   join_paths(a, b, "Emerald.sav", "t.sav");
   CHECK(same_file(a, b));
   /* A LYING SHARER (review finding 4): the size its STAT answer gives is
    * not the one its manifest listed.  The join requires the manifest's
    * size: equal passes; anything else, an empty file included, fails with
    * "changed" before a byte is written, and the next fetch is unaffected. */
   g_expect = SF.e[0].save_size;
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_SAVE, 0), "t2.sav", 0, 200000)
         == FE_SHARE_CL_DONE);
   g_expect = SF.e[0].save_size + 1;       /* the manifest said otherwise */
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_SAVE, 0), "t3.sav", 0, 200000)
         == FE_SHARE_CL_ERROR && CL.err == FE_SHARE_E_CHANGED);
   {
      struct stat st3; char p3[512];
      snprintf(p3, sizeof(p3), "%s/t3.sav", DIRB);
      CHECK(stat(p3, &st3) == 0 && st3.st_size == 0);   /* nothing written */
   }
   g_expect = 0;
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_SAVE, 0), "t4.sav", 0, 200000)
         == FE_SHARE_CL_DONE);
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_STATE, 1), "t.st", 0, 400000)
         == FE_SHARE_CL_DONE);
   join_paths(a, b, "Emerald.st0", "t.st");
   CHECK(same_file(a, b));
   /* A STATE MUST BE A STATE OF THIS BUILD (finding 4, second half).  The
    * builds agreed in HELLO; the file is what is looked at.  Slot 4 is a
    * GBA state of this format: fetched.  Slot 5 has the right size and an
    * older format's header: fetched, checksum good, and refused -- never
    * DONE, so never committed.  Slot 1 is no state's size: refused before a
    * byte is written.  A GB "state" of noise: refused the same way. */
   g_expect_state = 1; g_expect_fmt = 0x00010004;
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_STATE, 4), "t.s4", 0, 400000)
         == FE_SHARE_CL_DONE);
   join_paths(a, b, "Emerald.st3", "t.s4");
   CHECK(same_file(a, b));
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_STATE, 5), "t.s5", 0, 400000)
         == FE_SHARE_CL_ERROR && CL.err == FE_SHARE_E_FORMAT);
   CHECK(CL.got == CL.nchunks && CL.nchunks != 0);   /* it all arrived */
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_STATE, 1), "t.s1", 0, 400000)
         == FE_SHARE_CL_ERROR && CL.err == FE_SHARE_E_FORMAT);
   {
      struct stat st1; char p1[512];
      snprintf(p1, sizeof(p1), "%s/t.s1", DIRB);
      CHECK(stat(p1, &st1) == 0 && st1.st_size == 0);   /* nothing written */
   }
   g_expect_state = 2; g_expect_fmt = 1;
   CHECK(fetch_file(OBJ(1, FE_SHARE_OBJ_STATE, 1), "t.g1", 0, 400000)
         == FE_SHARE_CL_ERROR && CL.err == FE_SHARE_E_FORMAT);
   g_expect_state = 0;
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_THUMB, 1), "t.th", 0, 200000)
         == FE_SHARE_CL_DONE);
   join_paths(a, b, "Emerald.st0.thumb", "t.th");
   CHECK(same_file(a, b));
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_SHOT, 1), "t.sh", 0, 200000)
         == FE_SHARE_CL_DONE);
   join_paths(a, b, "Emerald.st0.shot", "t.sh");
   CHECK(same_file(a, b));
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_STATE, 3), "t.s3", 0, 200000)
         == FE_SHARE_CL_DONE);
   join_paths(a, b, "Emerald.st2", "t.s3");
   CHECK(same_file(a, b));
   CHECK(fetch_file(OBJ(1, FE_SHARE_OBJ_SAVE, 0), "t.gb", 0, 200000)
         == FE_SHARE_CL_DONE);
   join_paths(a, b, "Red.gb.sav", "t.gb");
   CHECK(same_file(a, b));
   /* an empty file: DONE with zero chunks (the manifest's size was stale;
    * INFO's is what counts) */
   CHECK(fetch_file(OBJ(2, FE_SHARE_OBJ_STATE, 4), "t.e", 0, 200000)
         == FE_SHARE_CL_DONE);
   CHECK(CL.size == 0);
   /* what the manifest does not offer is refused, not served */
   CHECK(fetch_file(OBJ(2, FE_SHARE_OBJ_SAVE, 0), "t.x", 0, 2000)
         == FE_SHARE_CL_ERROR);
   CHECK(CL.err == FE_SHARE_E_BAD);
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_SHOT, 2), "t.x", 0, 2000)
         == FE_SHARE_CL_ERROR);
   CHECK(fe_share_client_linked(&CL));      /* the link itself is fine */
   CHECK(SF.bad_opens == 0);
   if (loss)
      CHECK(dropped > 0 && CL.retries > 0);
   printf("  loss=%d%% dup=%d%% reorder=%d: ok (GETs %u, retries %u, dups %u)\n",
          loss, dup, reo, CL.tx_get, CL.retries, CL.rx_dup);
}

/* ---- 5. corruption -------------------------------------------------------- */
static void t_corruption(void)
{
   char a[512], b[512], mine[512];
   uint32_t before;
   rig_up("b", "b");
   rig_join();
   /* in the air: caught per datagram, re-asked, the file still arrives */
   corrupt_pct = 15;
   corrupted = 0;
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_STATE, 1), "c.st", 0, 400000)
         == FE_SHARE_CL_DONE);
   join_paths(a, b, "Emerald.st0", "c.st");
   CHECK(same_file(a, b));
   CHECK(corrupted > 20 && CL.rx_bad > 0);
   corrupt_pct = 0;

   /* the sharer's file changes after its checksum was taken: every
    * datagram is valid, the payload is not what INFO promised */
   put_file(DIRB, "Emerald.sav", 131072, 99);      /* the joiner's own save */
   snprintf(mine, sizeof(mine), "%s/Emerald.sav", DIRB);
   before = file_crc(mine);
   {
      snk s;
      fe_share_sink sink;
      int i, poked = 0;
      memset(&s, 0, sizeof(s));
      snprintf(b, sizeof(b), "%s/Emerald.sav.gsp", DIRB);
      s.f = fopen(b, "w+b");
      sink.user = &s; sink.write = k_write; sink.read = k_read; sink.cap = 0;
      CHECK(fe_share_client_fetch(&CL, OBJ(0, FE_SHARE_OBJ_SAVE, 0), &sink,
                                  NOW) == 0);
      for (i = 0; i < 100000 && (CL.state == FE_SHARE_CL_STAT ||
                                 CL.state == FE_SHARE_CL_DATA); i++)
      {
         if (!poked && CL.state == FE_SHARE_CL_DATA && CL.got >= 4)
         {
            put_file(DIRA, "Emerald.sav", 131072, 1234);   /* same size */
            poked = 1;
         }
         frame(8);
      }
      fclose(s.f);
      CHECK(poked);
      CHECK(CL.state == FE_SHARE_CL_ERROR && CL.err == FE_SHARE_E_CHANGED);
   }
   /* the caller never commits a failed fetch: the joiner's save is as it
    * was, byte for byte */
   CHECK(file_crc(mine) == before);
   /* A new fetch must checksum the changed file again, without requiring
    * the sharer to restore its previous bytes or restart the session. */
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_SAVE, 0), "c.sav", 0, 100000)
         == FE_SHARE_CL_DONE);
   join_paths(a, b, "Emerald.sav", "c.sav");
   CHECK(same_file(a, b));
   put_file(DIRA, "Emerald.sav", 131072, 1);
}

/* ---- 6. truncation -------------------------------------------------------- */
static void t_truncation(void)
{
   snk s;
   fe_share_sink sink;
   char b[512];
   int i;
   uint32_t t0;
   rig_up("b", "b");
   rig_join();
   memset(&s, 0, sizeof(s));
   snprintf(b, sizeof(b), "%s/x.gsp", DIRB);
   s.f = fopen(b, "w+b");
   sink.user = &s; sink.write = k_write; sink.read = k_read; sink.cap = 0;
   CHECK(fe_share_client_fetch(&CL, OBJ(0, FE_SHARE_OBJ_STATE, 1), &sink,
                               NOW) == 0);
   for (i = 0; i < 100000 && CL.got < 100; i++)
      frame(8);
   CHECK(CL.state == FE_SHARE_CL_DATA && CL.got >= 100 && CL.got < CL.nchunks);
   radio_dead = 1;                           /* the sharer is switched off */
   t0 = NOW;
   for (i = 0; i < 100000 && CL.state == FE_SHARE_CL_DATA; i++)
      frame(8);
   fclose(s.f);
   CHECK(CL.state == FE_SHARE_CL_ERROR && CL.err == FE_SHARE_E_LOST);
   CHECK(NOW - t0 >= FE_SHARE_LOST_AFTER && NOW - t0 < FE_SHARE_LOST_AFTER + 600);
   CHECK(!fe_share_client_linked(&CL));
   /* nothing can be fetched on a lost link */
   CHECK(fe_share_client_fetch(&CL, OBJ(0, FE_SHARE_OBJ_SAVE, 0), &sink, NOW)
         != 0);
   /* the server forgets the silent guest and waits again */
   radio_dead = 0;
   for (i = 0; i < 1000; i++)
      frame(8);
   CHECK(SRV.state == FE_SHARE_SRV_WAITING && SF.f == NULL);

   /* the sharer stops: CLOSED, at once */
   rig_up("b", "b");
   rig_join();
   CHECK(fe_share_client_fetch(&CL, OBJ(0, FE_SHARE_OBJ_STATE, 1), &sink,
                               NOW) == 0);
   s.f = fopen(b, "w+b");
   for (i = 0; i < 20; i++)
      frame(8);
   fe_share_server_stop(&SRV);
   for (i = 0; i < 10; i++)
      frame(8);
   fclose(s.f);
   CHECK(CL.state == FE_SHARE_CL_ERROR && CL.err == FE_SHARE_E_CLOSED);
   CHECK(SF.f == NULL);

   /* the file shrinks / is pulled mid-transfer: GONE */
   rig_up("b", "b");
   rig_join();
   s.f = fopen(b, "w+b");
   SF.shrink_after = 416321 / 1024 + 1 + 50;   /* the checksum pass + 50 */
   CHECK(fe_share_client_fetch(&CL, OBJ(0, FE_SHARE_OBJ_STATE, 1), &sink,
                               NOW) == 0);
   for (i = 0; i < 100000 && (CL.state == FE_SHARE_CL_STAT ||
                              CL.state == FE_SHARE_CL_DATA); i++)
      frame(8);
   fclose(s.f);
   CHECK(CL.state == FE_SHARE_CL_ERROR && CL.err == FE_SHARE_E_GONE);
   CHECK(fe_share_client_linked(&CL));
   remove(b);
}

/* ---- 7. hostile ----------------------------------------------------------- */
static size_t forge(uint8_t *d, int proto, int type, unsigned session,
                    const uint8_t *body, size_t bl)
{
   uint32_t c;
   size_t hdr = proto == 1 || type == 1 || type == 3 ? 8u : FE_SHARE_HDR;
   memcpy(d, FE_SHARE_MAGIC, 4);
   d[4] = (uint8_t)proto; d[5] = (uint8_t)type;
   d[6] = (uint8_t)session; d[7] = (uint8_t)(session >> 8);
   if (hdr == FE_SHARE_HDR) {
      d[8] = (uint8_t)(session >> 16); d[9] = (uint8_t)(session >> 24);
   }
   if (bl) memcpy(d + hdr, body, bl);
   c = fe_share_crc32(0, d, hdr + bl);
   d[hdr + bl] = (uint8_t)c; d[hdr + bl + 1] = (uint8_t)(c >> 8);
   d[hdr + bl + 2] = (uint8_t)(c >> 16); d[hdr + bl + 3] = (uint8_t)(c >> 24);
   return hdr + 4 + bl;
}

static void t_session_replay(void)
{
   pkt hello, offer, confirm, welcome, new_offer;
   uint8_t d[128], body[15] = {0};
   uint32_t session, last_rx, opens;
   int i;
   rig_up("b", "b");
   SRV.session_seed = 0xABCD004Du;
   CL.nonce = 0xBEEF1234u;
   fe_share_client_hello(&CL, NOW);
   CHECK(q_n == 1); hello = Q[0]; q_n = 0;
   fe_share_server_rx(&SRV, hello.d, hello.n, NOW);
   CHECK(SRV.state == FE_SHARE_SRV_WAITING && q_n == 1);
   offer = Q[0]; q_n = 0;
   fe_share_client_rx(&CL, offer.d, offer.n, NOW);
   CHECK(q_n == 1); confirm = Q[0]; q_n = 0;
   /* An expired confirmation cannot admit anyone. */
   NOW += 8000;
   fe_share_server_rx(&SRV, confirm.d, confirm.n, NOW);
   CHECK(SRV.state == FE_SHARE_SRV_WAITING && q_n == 0);
   fe_share_server_rx(&SRV, hello.d, hello.n, NOW);
   CHECK(q_n == 1); new_offer = Q[0]; q_n = 0;
   fe_share_client_rx(&CL, new_offer.d, new_offer.n, NOW);
   CHECK(q_n == 1);
   fe_share_server_rx(&SRV, Q[0].d, Q[0].n, NOW);
   welcome = Q[1]; q_n = 0;
   session = SRV.session;
   CHECK(session > 65535 && SRV.peer_nonce == 0xBEEF1234u);
   /* Reordered obsolete OFFER must recover through periodic HELLO even
    * when the first final WELCOME was lost. */
   fe_share_client_rx(&CL, offer.d, offer.n, NOW);
   CHECK(q_n == 1);
   fe_share_server_rx(&SRV, Q[0].d, Q[0].n, NOW); q_n = 0;
   CHECK(SRV.session == session);
   for (i = 0; i < 200 && CL.state == FE_SHARE_CL_HELLO; i++) frame(8);
   CHECK(CL.state == FE_SHARE_CL_READY && CL.session == session);
   /* Duplicate final admission is harmless once linked. */
   fe_share_client_rx(&CL, welcome.d, welcome.n, NOW);
   CHECK(CL.state == FE_SHARE_CL_READY && CL.session == session);
   /* A deliberate reconnect gets a new nonce/session. */
   rig_join(); q_n = 0;
   CHECK(CL.nonce == 0xBEEF1235u && SRV.session != session);
   body[2] = FE_SHARE_OBJ_SAVE; body[4] = 9;
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 4, SRV.session, body, 6), NOW);
   CHECK(SRV.have_obj && SRV.req == 9);
   body[10] = 1;
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 6, SRV.session, body, 14), NOW);
   opens = SF.opens; last_rx = SRV.last_rx_ms; q_n = 0;
   NOW += 7;
   fe_share_server_rx(&SRV, hello.d, hello.n, NOW);
   fe_share_server_rx(&SRV, confirm.d, confirm.n, NOW);
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 4, session, body, 6), NOW);
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 6, session, body, 14), NOW);
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 10, session, body, 0), NOW);
   /* Same low 16 bits, wrong high 16 bits: still an obsolete instance. */
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 10,
                                   SRV.session ^ 0x10000u, body, 0), NOW);
   CHECK(SRV.state == FE_SHARE_SRV_CONNECTED && SRV.session == CL.session);
   CHECK(SRV.have_obj && SRV.req == 9 && SRV.win_mask == 1);
   CHECK(SF.opens == opens && SRV.last_rx_ms == last_rx);
   last_rx = CL.t_last_rx;
   fe_share_client_rx(&CL, offer.d, offer.n, NOW);
   fe_share_client_rx(&CL, welcome.d, welcome.n, NOW);
   fe_share_client_rx(&CL, d, forge(d, FE_SHARE_PROTO, 9, session, body, 0), NOW);
   fe_share_client_rx(&CL, d, forge(d, FE_SHARE_PROTO, 10, session, body, 0), NOW);
   fe_share_client_rx(&CL, d, forge(d, FE_SHARE_PROTO, 5, session, body, 15), NOW);
   fe_share_client_rx(&CL, d, forge(d, FE_SHARE_PROTO, 7, session, body, 12), NOW);
   CHECK(CL.state == FE_SHARE_CL_READY && CL.t_last_rx == last_rx);
   q_n = 0;
   printf("  stale share handshakes/session replay: ok\n");
}

static void t_request_order(void)
{
   uint8_t d[64], body[14] = {0};
   uint32_t opens;
   rig_up("b", "b"); rig_join(); q_n = 0;
   body[2] = FE_SHARE_OBJ_SAVE; body[4] = 7;
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 4, SRV.session, body, 6), NOW);
   CHECK(SRV.have_obj && SRV.req == 7);
   opens = SF.opens;
   /* Exact retransmission reuses the snapshot; a new request reopens it. */
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 4, SRV.session, body, 6), NOW);
   CHECK(SF.opens == opens);
   body[4] = 8;
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 4, SRV.session, body, 6), NOW);
   CHECK(SF.opens == opens + 1 && SRV.req == 8);
   body[10] = 1;
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 6, SRV.session, body, 14), NOW);
   CHECK(SRV.win_mask == 1);
   /* Delayed requests cannot replace the newest request or its window. */
   body[4] = 7; body[10] = 2;
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 6, SRV.session, body, 14), NOW);
   CHECK(SRV.req == 8 && SRV.win_mask == 1);
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 4, SRV.session, body, 6), NOW);
   CHECK(SRV.req == 8 && SRV.win_mask == 1 && SF.opens == opens + 1);
   /* Request serial numbers wrap through zero. */
   SRV.req = 65535; body[4] = body[5] = 0;
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 4, SRV.session, body, 6), NOW);
   CHECK(SRV.req == 0 && SF.opens == opens + 2);
   q_n = 0;
}

static void t_hostile(void)
{
   uint8_t d[FE_SHARE_DGRAM_MAX + 64], body[64];
   unsigned it, sess;
   uint32_t data_before;
   rig_up("b", "b");
   rig_join();
   sess = SRV.session;
   /* Well-formed, valid CRC, the right session, and wrong in every other
    * way: types 0..15, any entry / kind / slot / chunk / mask, any length. */
   for (it = 0; it < 200000; it++)
   {
      size_t bl = rnd() % 20, k, n;
      int type = (int)(rnd() % 16);
      for (k = 0; k < sizeof(body); k++)
         body[k] = (uint8_t)rnd();
      if (rnd() & 1)
      {
         /* shaped like a real STAT / GET, with hostile fields */
         static const unsigned ent[] = { 0, 1, 2, 3, 7, 255, 1023, 65535 };
         unsigned e = ent[rnd() % 8];
         type = (rnd() & 1) ? 4 : 6;
         body[0] = (uint8_t)e; body[1] = (uint8_t)(e >> 8);
         body[2] = (uint8_t)(rnd() % 8);
         body[3] = (uint8_t)(rnd() % 8 == 0 ? rnd() : rnd() % 7);
         bl = type == 4 ? 6 : 14;
         if ((rnd() & 15) == 0)
            bl = bl + rnd() % 3 - 1;
      }
      n = forge(d, FE_SHARE_PROTO, type, (rnd() & 7) ? sess : rnd(), body, bl);
      fe_share_server_rx(&SRV, d, n, NOW);
      fe_share_server_step(&SRV, NOW, 4);
      q_n = 0;
      if (SRV.state != FE_SHARE_SRV_CONNECTED)   /* a forged BYE: rejoin */
      {
         fe_share_client_io io = CL.io;
         fe_share_ident me = CL.me;
         fe_share_client_init(&CL, &io, &me, (uint16_t)(0x2000 + it));
         rig_join();
         sess = SRV.session;
      }
   }
   CHECK(SF.opens > 100);
   CHECK(SF.bad_opens == 0);
   /* pure noise, truncations of real datagrams, wrong magic, bad CRC */
   data_before = SRV.tx_data;
   for (it = 0; it < 100000; it++)
   {
      size_t n = rnd() % sizeof(d), k;
      for (k = 0; k < n; k++)
         d[k] = (uint8_t)rnd();
      if (rnd() & 1)
         memcpy(d, FE_SHARE_MAGIC, 4);
      fe_share_server_rx(&SRV, d, n, NOW);
      fe_share_client_rx(&CL, d, n, NOW);
   }
   {
      size_t n, k;
      body[0] = 0; body[1] = 0; body[2] = FE_SHARE_OBJ_SAVE; body[3] = 0;
      body[4] = 1; body[5] = 0;
      n = forge(d, FE_SHARE_PROTO, 4, SRV.session, body, 6);
      for (k = 0; k < n; k++)
         fe_share_server_rx(&SRV, d, k, NOW);       /* every truncation */
      d[n - 1] ^= 1;
      fe_share_server_rx(&SRV, d, n, NOW);          /* bad CRC */
      fe_share_server_rx(&SRV, d, FE_SHARE_DGRAM_MAX + 1, NOW);
      fe_share_server_rx(&SRV, NULL, 0, NOW);
   }
   fe_share_server_step(&SRV, NOW, 64);
   CHECK(SRV.tx_data == data_before);
   CHECK(SF.bad_opens == 0);
   /* a chunk index past the end, and a mask reaching past it, send nothing
    * beyond the file. Start a new session after injecting arbitrary valid
    * request IDs: its request watermark belongs to those synthetic requests,
    * not the untouched client's sequence. */
   rig_up("b", "b"); rig_join(); q_n = 0;
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_STATE, 3), "h.s3", 0, 20000)
         == FE_SHARE_CL_DONE);                       /* 3 chunks, now open */
   q_n = 0;
   data_before = SRV.tx_data;
   memset(body, 0, sizeof(body));
   body[2] = FE_SHARE_OBJ_STATE; body[3] = 3;
   body[4] = (uint8_t)SRV.req; body[5] = (uint8_t)(SRV.req >> 8);
   body[6] = 3;                                      /* first = 3: past end */
   memset(body + 10, 0xFF, 4);
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 6, SRV.session, body,
                                     14), NOW);
   fe_share_server_step(&SRV, NOW, 64);
   CHECK(SRV.tx_data == data_before);
   body[6] = 1;                                      /* first = 1, mask all */
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 6, SRV.session, body,
                                     14), NOW);
   fe_share_server_step(&SRV, NOW, 64);
   CHECK(SRV.tx_data == data_before + 2);            /* chunks 1 and 2 only */
}

/* ---- 8. mixed versions ---------------------------------------------------- */
static void t_versions(void)
{
   uint8_t d[128], body[64];
   fe_share_client_io cio2 = { &CL2, c_send };
   fe_share_ident id2 = ident("b", 3);
   uint32_t sess;
   int i;
   /* a DIFFERENT BUILD on the same protocol: joins, saves cross, states
    * are not offered */
   rig_up("3.5.0-aaaaaaa", "4.0.0-bbbbbbb");
   rig_join();
   CHECK(!fe_share_states_ok(&CL.me, &CL.host, 0));
   CHECK(strcmp(CL.host.build, "3.5.0-aaaaaaa") == 0);
   CHECK(strcmp(SRV.peer_ident.build, "4.0.0-bbbbbbb") == 0);
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_SAVE, 0), "v.sav", 0, 100000)
         == FE_SHARE_CL_DONE);
   /* same build, another state format on one console only */
   {
      fe_share_ident a = ident("x", 1), b = ident("x", 2);
      b.state_fmt[1] = 8;
      CHECK(fe_share_states_ok(&a, &b, 0) && !fe_share_states_ok(&a, &b, 1));
      CHECK(!fe_share_states_ok(&a, &b, 3));
   }

   /* A delayed HELLO stays provisional; only its current challenge can
    * replace the active guest. Same-nonce HELLO preserves the transfer. */
   sess = SRV.session;
   memcpy(body, CL.me.mac, 6);
   body[6] = 0x34; body[7] = 0x12; body[8] = body[9] = 0;
   memset(body + 10, 0, 36);
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 1, 0, body, 46), NOW);
   CHECK(SRV.session == sess);
   body[6] = 0x35;
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 1, 0, body, 46), NOW);
   CHECK(SRV.session == sess && SRV.offer_session != sess);
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO, 12, SRV.offer_session,
                                   body, 46), NOW);
   CHECK(SRV.session != sess);
   q_n = 0;

   /* a SECOND GUEST while one is connected: BUSY, and the first carries on */
   rig_up("b", "b");
   rig_join();
   fe_share_client_init(&CL2, &cio2, &id2, 0x7777);
   fe_share_client_hello(&CL2, NOW);
   for (i = 0; i < 100 && CL2.state == FE_SHARE_CL_HELLO; i++)
      frame(8);
   CHECK(CL2.state == FE_SHARE_CL_ERROR && CL2.err == FE_SHARE_E_BUSY);
   CHECK(SRV.state == FE_SHARE_SRV_CONNECTED &&
         memcmp(SRV.peer, CL.me.mac, 6) == 0);
   memset(&CL2, 0, sizeof(CL2));
   CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_SAVE, 0), "v2.sav", 0, 100000)
         == FE_SHARE_CL_DONE);
   (void)cl2_refused;

   /* ANOTHER PROTOCOL GENERATION, both ways.  A newer joiner's HELLO (its
    * body may be anything after the MAC): the sharer answers REFUSE to
    * that MAC and stays waiting. */
   rig_up("b", "b");
   memset(body, 0xAB, sizeof(body));
   memcpy(body, CL.me.mac, 6);
   fe_share_server_rx(&SRV, d, forge(d, FE_SHARE_PROTO + 1, 1, 0, body, 60),
                      NOW);
   CHECK(SRV.state == FE_SHARE_SRV_WAITING && SRV.rx_refused == 1);
   CHECK(q_n == 1 && Q[0].d[5] == 3 /* T_REFUSE */);
   /* ...which this joiner reads as REFUSED, with the sharer's build */
   fe_share_client_hello(&CL, NOW);
   fe_share_client_rx(&CL, Q[0].d, Q[0].n, NOW);
   CHECK(CL.state == FE_SHARE_CL_ERROR && CL.err == FE_SHARE_E_REFUSED);
   CHECK(CL.refused_proto == FE_SHARE_PROTO);
   CHECK(strcmp(CL.host.build, "b") == 0);
   /* An OLDER/NEWER sharer answering our HELLO with something of its own
    * generation: also REFUSED, never mis-parsed. */
   rig_up("b", "b");
   fe_share_client_hello(&CL, NOW);
   memset(body, 0x11, sizeof(body));
   fe_share_client_rx(&CL, d, forge(d, FE_SHARE_PROTO + 1, 2, 5, body, 58),
                      NOW);
   CHECK(CL.state == FE_SHARE_CL_ERROR && CL.err == FE_SHARE_E_REFUSED);
   CHECK(CL.refused_proto == FE_SHARE_PROTO + 1);
   /* v1 framing stays readable in both directions. Old hosts see the
    * actual client MAC at offset 8, and old REFUSE remains parseable. */
   rig_up("b", "b");
   memcpy(body, CL.me.mac, 6); memset(body + 6, 0, 38);
   fe_share_server_rx(&SRV, d, forge(d, 1, 1, 0, body, 44), NOW);
   CHECK(SRV.state == FE_SHARE_SRV_WAITING && SRV.rx_refused == 1);
   CHECK(q_n == 1 && Q[0].n == 8 + 2 + FE_SHARE_BUILD_CAP + 4);
   CHECK(Q[0].d[8] == 1 && Q[0].d[9] == FE_SHARE_PROTO);
   q_n = 0;
   fe_share_client_hello(&CL, NOW);
   CHECK(q_n == 1 && !memcmp(Q[0].d + 8, CL.me.mac, 6));
   body[0] = 1; body[1] = 1; memset(body + 2, 0, FE_SHARE_BUILD_CAP);
   fe_share_client_rx(&CL, d, forge(d, 1, 3, 0, body,
                                  2 + FE_SHARE_BUILD_CAP), NOW);
   CHECK(CL.state == FE_SHARE_CL_ERROR && CL.err == FE_SHARE_E_REFUSED);
   CHECK(CL.refused_proto == 1);
   /* A WELCOME for somebody else's HELLO (wrong nonce) is not ours. */
   rig_up("b", "b");
   fe_share_client_hello(&CL, NOW);
   memset(body, 0, sizeof(body));
   body[6] = 0x99;
   fe_share_client_rx(&CL, d, forge(d, FE_SHARE_PROTO, 2, 5, body, 58), NOW);
   CHECK(CL.state == FE_SHARE_CL_HELLO);
   /* Nobody there at all (an old build's room, a link host): give up. */
   for (i = 0; i < 2000 && CL.state == FE_SHARE_CL_HELLO; i++)
   {
      q_n = 0;                                       /* nothing is delivered */
      fe_share_client_step(&CL, NOW);
      NOW += 16;
   }
   CHECK(CL.state == FE_SHARE_CL_ERROR && CL.err == FE_SHARE_E_LOST);
   /* An old build's link traffic (netdrv frames, no "GSHR") at the sharer:
    * counted as noise, nothing answered, nobody connected. */
   rig_up("b", "b");
   for (i = 0; i < 1000; i++)
   {
      size_t n = 12 + rnd() % 140, k;
      for (k = 0; k < n; k++)
         d[k % sizeof(d)] = (uint8_t)rnd();
      d[0] = 'N'; d[1] = 'D';
      fe_share_server_rx(&SRV, d, n > sizeof(d) ? sizeof(d) : n, NOW);
   }
   CHECK(SRV.state == FE_SHARE_SRV_WAITING && q_n == 0 && SRV.rx_ok == 0);
}

/* ---- 9. commit ------------------------------------------------------------ */
static int fail_rename_n;          /* fail the Nth rename from now (1 = next) */
static int x_exists(void *u, const char *p) { (void)u; return exists(p); }
static int x_rename(void *u, const char *a, const char *b)
{
   (void)u;
   if (fail_rename_n && --fail_rename_n == 0)
      return -1;
   return rename(a, b);
}
static int x_remove(void *u, const char *p) { (void)u; return remove(p); }

static void t_commit(void)
{
   static const fe_share_fs fs = { NULL, x_exists, x_rename, x_remove };
   char fin[512], tmp[512], bak[512], old[512], scratch[520];
   uint32_t c_old, c_new, c_new2;
   snprintf(fin, sizeof(fin), "%s/Game.sav", DIRB);
   snprintf(tmp, sizeof(tmp), "%s/Game.sav.gsp", DIRB);
   snprintf(bak, sizeof(bak), "%s/Game.sav.bak", DIRB);
   snprintf(old, sizeof(old), "%s/Game.sav.old", DIRB);
   remove(fin); remove(bak);

   /* no save yet: the new one lands, no .bak appears */
   put_file(DIRB, "Game.sav.gsp", 1000, 1);
   c_new = file_crc(tmp);
   CHECK(fe_share_commit(&fs, fin, tmp, 1, scratch, sizeof(scratch)) == 0);
   CHECK(file_crc(fin) == c_new && !exists(tmp) && !exists(bak));

   /* a save exists: it becomes the .bak */
   c_old = c_new;
   put_file(DIRB, "Game.sav.gsp", 2000, 2);
   c_new = file_crc(tmp);
   CHECK(fe_share_commit(&fs, fin, tmp, 1, scratch, sizeof(scratch)) == 0);
   CHECK(file_crc(fin) == c_new && file_crc(bak) == c_old && !exists(tmp));

   /* again: still exactly ONE .bak, holding the save just replaced */
   put_file(DIRB, "Game.sav.gsp", 3000, 3);
   c_new2 = file_crc(tmp);
   CHECK(fe_share_commit(&fs, fin, tmp, 1, scratch, sizeof(scratch)) == 0);
   CHECK(file_crc(fin) == c_new2 && file_crc(bak) == c_new && !exists(tmp));
   CHECK(!exists(old));

   /* RECOVERY (review finding 5).  Power lost between the two renames: the
    * save is gone, the one it replaced is the .bak, a temp is left over.
    * fe_share_recover puts the .bak back and removes the temp; run again it
    * finds nothing to do; and it never touches a .bak beside a live save. */
   {
      uint32_t c_live = file_crc(fin), c_bak = file_crc(bak);
      CHECK(fe_share_recover(&fs, fin, ".gsp", scratch, sizeof(scratch)) == 0);
      CHECK(file_crc(fin) == c_live && file_crc(bak) == c_bak);
      remove(fin);                               /* the window */
      put_file(DIRB, "Game.sav.gsp", 500, 9);    /* an orphaned temp */
      CHECK(fe_share_recover(&fs, fin, ".gsp", scratch, sizeof(scratch)) == 1);
      CHECK(file_crc(fin) == c_bak && !exists(bak) && !exists(tmp));
      CHECK(fe_share_recover(&fs, fin, ".gsp", scratch, sizeof(scratch)) == 0);
      /* a state: its .old comes back; a stray .old beside a live file goes */
      remove(fin);
      put_file(DIRB, "Game.sav.old", 700, 8);
      c_bak = file_crc(old);
      CHECK(fe_share_recover(&fs, fin, ".gsp", scratch, sizeof(scratch)) == 1);
      CHECK(file_crc(fin) == c_bak && !exists(old));
      put_file(DIRB, "Game.sav.old", 700, 7);
      CHECK(fe_share_recover(&fs, fin, ".gsp", scratch, sizeof(scratch)) == 0);
      CHECK(file_crc(fin) == c_bak && !exists(old));
      /* neither file: nothing appears */
      remove(fin);
      CHECK(fe_share_recover(&fs, fin, ".gsp", scratch, sizeof(scratch)) == 0);
      CHECK(!exists(fin));
      put_file(DIRB, "Game.sav", 3000, 3);       /* as the cases below expect */
   }

   /* the move-aside fails: nothing changed, the temp is gone */
   put_file(DIRB, "Game.sav.gsp", 4000, 4);
   fail_rename_n = 1;
   CHECK(fe_share_commit(&fs, fin, tmp, 1, scratch, sizeof(scratch)) != 0);
   CHECK(file_crc(fin) == c_new2 && !exists(tmp));
   /* the move-in fails: the old save is put back */
   put_file(DIRB, "Game.sav.gsp", 4000, 4);
   fail_rename_n = 2;
   CHECK(fe_share_commit(&fs, fin, tmp, 1, scratch, sizeof(scratch)) != 0);
   CHECK(file_crc(fin) == c_new2 && !exists(tmp));
   fail_rename_n = 0;

   /* a state: replaced, and NO backup is left behind */
   snprintf(fin, sizeof(fin), "%s/Game.st1", DIRB);
   snprintf(tmp, sizeof(tmp), "%s/Game.st1.gsp", DIRB);
   snprintf(old, sizeof(old), "%s/Game.st1.old", DIRB);
   snprintf(bak, sizeof(bak), "%s/Game.st1.bak", DIRB);
   put_file(DIRB, "Game.st1", 5000, 5);
   c_old = file_crc(fin);
   put_file(DIRB, "Game.st1.gsp", 6000, 6);
   c_new = file_crc(tmp);
   CHECK(fe_share_commit(&fs, fin, tmp, 0, scratch, sizeof(scratch)) == 0);
   CHECK(file_crc(fin) == c_new && !exists(old) && !exists(bak) &&
         !exists(tmp));
   put_file(DIRB, "Game.st1.gsp", 7000, 7);
   fail_rename_n = 2;
   CHECK(fe_share_commit(&fs, fin, tmp, 0, scratch, sizeof(scratch)) != 0);
   CHECK(file_crc(fin) == c_new && !exists(old) && !exists(tmp));
   fail_rename_n = 0;
   /* no temp: refused; a path that cannot hold the suffix: refused */
   CHECK(fe_share_commit(&fs, fin, tmp, 0, scratch, sizeof(scratch)) != 0);
   put_file(DIRB, "Game.st1.gsp", 7000, 7);
   CHECK(fe_share_commit(&fs, fin, tmp, 0, scratch, strlen(fin) + 4) != 0);
   CHECK(file_crc(fin) == c_new && !exists(tmp));
   (void)c_old;

   /* A FULL STICK: the sink's write fails part-way.  The fetch is an
    * error, and the joiner's own save was never touched. */
   {
      char mine[512];
      uint32_t before;
      rig_up("b", "b");
      rig_join();
      put_file(DIRB, "Emerald.sav", 131072, 42);
      snprintf(mine, sizeof(mine), "%s/Emerald.sav", DIRB);
      before = file_crc(mine);
      CHECK(fetch_file(OBJ(0, FE_SHARE_OBJ_SAVE, 0), "Emerald.sav.gsp",
                       50000, 100000) == FE_SHARE_CL_ERROR);
      CHECK(CL.err == FE_SHARE_E_WRITE && fe_share_client_linked(&CL));
      CHECK(file_crc(mine) == before);
      /* a sink too small for the object refuses it before a byte lands */
      {
         static uint8_t small[4096];
         snk s;
         fe_share_sink sink;
         int i;
         memset(&s, 0, sizeof(s));
         s.mem = small; s.cap = sizeof(small);
         sink.user = &s; sink.write = k_write; sink.read = k_read;
         sink.cap = sizeof(small);
         CHECK(fe_share_client_fetch(&CL, OBJ(0, FE_SHARE_OBJ_SAVE, 0),
                                     &sink, NOW) == 0);
         for (i = 0; i < 1000 && CL.state == FE_SHARE_CL_STAT; i++)
            frame(8);
         CHECK(CL.state == FE_SHARE_CL_ERROR && CL.err == FE_SHARE_E_TOO_BIG);
         CHECK(s.written == 0);
      }
   }
}

/* What the sharer's stick looked like before and after everything above. */
static uint32_t dir_a_crc(void)
{
   static const char *names[] = { "Emerald.sav", "Emerald.st0",
      "Emerald.st0.thumb", "Emerald.st0.shot", "Emerald.st1",
      "Emerald.st1.thumb", "Emerald.st2", "Red.gb.sav", "Red.gb.st0",
      "Kart.st3" };
   char p[512];
   uint32_t c = 0, x;
   unsigned i;
   for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
   {
      snprintf(p, sizeof(p), "%s/%s", DIRA, names[i]);
      x = file_crc(p);
      c = fe_share_crc32(c, &x, sizeof(x));
   }
   return c;
}

int main(int argc, char **argv)
{
   char base[] = "/tmp/fe_share_XXXXXX", cmd[600];
   uint32_t a0;
   CHECK(mkdtemp(base) != NULL);
   snprintf(DIRA, sizeof(DIRA), "%s/a", base);
   snprintf(DIRB, sizeof(DIRB), "%s/b", base);
   CHECK(mkdir(DIRA, 0777) == 0 && mkdir(DIRB, 0777) == 0);
   Q = malloc(sizeof(pkt) * NQ);
   CHECK(Q != NULL);

   if (argc > 1 && !strcmp(argv[1], "request-order"))
   {
      rig_files();
      t_request_order();
      fe_share_server_stop(&SRV);
      goto cleanup;
   }

   t_classify();
   t_identity();
   t_manifest();
   t_state_check();
   rig_files();
   a0 = dir_a_crc();
   t_transfers(0, 0, 0);
   t_transfers(10, 5, 1);
   t_transfers(30, 10, 1);
   t_corruption();
   t_truncation();
   t_request_order();
   t_session_replay();
   t_hostile();
   t_versions();
   t_commit();
   /* READ-ONLY: after every transfer, attack and failure above, the
    * sharer's files are what they were. */
   CHECK(dir_a_crc() == a0);

cleanup:
   free(Q);
   snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
   if (system(cmd) != 0)
      return 1;
   printf("PASS fe_share: %d checks\n", checks);
   return 0;
}
