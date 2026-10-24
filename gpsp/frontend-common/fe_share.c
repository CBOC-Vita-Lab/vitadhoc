/* fe_share.c -- see fe_share.h and docs/SAVE-SHARE.md. */
#include "fe_share.h"

#include <stdio.h>
#include <string.h>

/* ---- message types (the numbers are the wire) ---------------------------- */
enum
{
   T_HELLO = 1,   /* c->s  mac[6] nonce u32 build[24] fmt u32 x3            */
   T_WELCOME,     /* s->c  mac[6] nonce u32 build[24] fmt x3 n u16 saves u16
                           states u16 manifest_size u32 manifest_crc u32     */
   T_REFUSE,      /* s->c  code u8 proto u8 build[24]   LAYOUT NEVER CHANGES */
   T_STAT,        /* c->s  obj[4] req u16                                    */
   T_INFO,        /* s->c  obj[4] req u16 status u8 size u32 crc u32         */
   T_GET,         /* c->s  obj[4] req u16 first u32 mask u32                 */
   T_DATA,        /* s->c  obj[4] req u16 chunk u32 len u16 data             */
   T_PING,        /* c->s                                                    */
   T_PONG,        /* s->c                                                    */
   T_BYE,         /* either way                                              */
   T_OFFER,       /* s->c ident echoing nonce; header is provisional session */
   T_CONFIRM      /* c->s ident; echo OFFER session before admission          */
};
enum { REFUSE_PROTO = 1, REFUSE_BUSY = 2 };
enum { ST_OK = 0, ST_GONE, ST_TOO_BIG, ST_NOT_OPEN, ST_BAD };

/* The server forgets a guest that has been silent this long. */
#define SERVER_GUEST_TIMEOUT 8000u

static const uint8_t BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---- bytes ---------------------------------------------------------------- */

static void put16(uint8_t *p, unsigned v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8);
   p[2] = (uint8_t)(v >> 16);
   p[3] = (uint8_t)(v >> 24);
}

static unsigned get16(const uint8_t *p)
{
   return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static uint32_t get32(const uint8_t *p)
{
   return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
          (uint32_t)p[3] << 24;
}

uint32_t fe_share_crc32(uint32_t crc, const void *buf, size_t len)
{
   static uint32_t table[256];
   static int ready;
   const uint8_t *p = (const uint8_t *)buf;
   if (!ready)
   {
      uint32_t i, k, c;
      for (i = 0; i < 256; i++)
      {
         c = i;
         for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
         table[i] = c;
      }
      ready = 1;
   }
   crc = ~crc;
   while (len--)
      crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
   return ~crc;
}

/* ---- times ---------------------------------------------------------------- */

uint32_t fe_share_time_pack(int year, int month, int day, int hour, int min)
{
   if (year < 1 || year > 4095 || month < 1 || month > 12 || day < 1 ||
       day > 31 || hour < 0 || hour > 23 || min < 0 || min > 59)
      return 0;
   return (uint32_t)year << 20 | (uint32_t)month << 16 | (uint32_t)day << 11 |
          (uint32_t)hour << 6 | (uint32_t)min;
}

void fe_share_time_text(uint32_t t, int with_time, char *out, size_t n)
{
   static const char *mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
   unsigned m = (t >> 16) & 15, d = (t >> 11) & 31, h = (t >> 6) & 31,
            mi = t & 63;
   if (!out || !n)
      return;
   out[0] = '\0';
   if (!t || m < 1 || m > 12 || d < 1 || h > 23 || mi > 59)
      return;
   if (with_time)
      snprintf(out, n, "%u %s, %02u:%02u", d, mon[m - 1], h, mi);
   else
      snprintf(out, n, "%u %s", d, mon[m - 1]);
}

/* ---- which files are game data -------------------------------------------- */

static int lower(int c)
{
   return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* Does name[0..len) end with `suf` (case-insensitive)? */
static int ends_with(const char *name, size_t len, const char *suf)
{
   size_t n = strlen(suf), i;
   if (len < n)
      return 0;
   for (i = 0; i < n; i++)
      if (lower((unsigned char)name[len - n + i]) != suf[i])
         return 0;
   return 1;
}

int fe_share_classify(const char *name, int *console, int *kind, int *slot,
                      size_t *key_len)
{
   size_t len, base;
   int k = FE_SHARE_OBJ_SAVE, s = 0, c = 0;
   if (!name)
      return 0;
   len = strlen(name);
   if (ends_with(name, len, ".thumb"))
   {
      k = FE_SHARE_OBJ_THUMB;
      len -= 6;
   }
   else if (ends_with(name, len, ".shot"))
   {
      k = FE_SHARE_OBJ_SHOT;
      len -= 5;
   }
   if (ends_with(name, len, ".sav"))
   {
      if (k != FE_SHARE_OBJ_SAVE)
         return 0;                        /* Game.sav.thumb is nothing */
      len -= 4;
   }
   else if (len >= 4 && name[len - 4] == '.' &&
            lower((unsigned char)name[len - 3]) == 's' &&
            lower((unsigned char)name[len - 2]) == 't' &&
            name[len - 1] >= '0' && name[len - 1] < '0' + FE_SHARE_SLOTS)
   {
      s = name[len - 1] - '0' + 1;
      if (k == FE_SHARE_OBJ_SAVE)
         k = FE_SHARE_OBJ_STATE;
      len -= 4;
   }
   else
      return 0;
   /* What is left is "Game" (GBA) or "Game.gb" / "Game.gbc" (the ROM's own
    * name).  A key needs a name before the suffix, in its own folder. */
   if (ends_with(name, len, ".gbc"))
      c = 2;
   else if (ends_with(name, len, ".gb"))
      c = 1;
   base = len - (c == 2 ? 4 : c == 1 ? 3 : 0);
   if (!base || name[base - 1] == '/' || name[base - 1] == '\\' ||
       name[base - 1] == ':')
      return 0;
   if (console) *console = c;
   if (kind)    *kind = k;
   if (slot)    *slot = s;
   if (key_len) *key_len = len;
   return 1;
}

int fe_share_key_matches_rom(const char *key, size_t key_len, int console,
                             const char *rom)
{
   size_t n, i;
   if (!key || !rom)
      return 0;
   n = strlen(rom);
   if (console == 0)
   {
      if (n != key_len + 4 || !ends_with(rom, n, ".gba"))
         return 0;
   }
   else if (n != key_len ||
            !ends_with(rom, n, console == 2 ? ".gbc" : ".gb"))
      return 0;
   for (i = 0; i < key_len; i++)
      if (lower((unsigned char)key[i]) != lower((unsigned char)rom[i]))
         return 0;
   return 1;
}

/* ---- game identity -------------------------------------------------------- */

int fe_share_game_id(int console, uint32_t rom_size, fe_share_read_fn read,
                     void *user, uint8_t id[FE_SHARE_ID_LEN])
{
   uint8_t hdr[32], blk[256];
   uint32_t off = console == 0 ? 0xA0u : 0x134u;
   uint32_t n = console == 0 ? 32u : 28u, crc = 0;
   const uint8_t *code;
   int i;
   if (!read || !id || console < 0 || console >= FE_SHARE_CONSOLES ||
       rom_size < off + n)
      return -1;
   if (read(user, off, hdr, n) != (int)n)
      return -1;
   memset(id, 0, FE_SHARE_ID_LEN);
   put32(id, rom_size);
   put32(id + 4, fe_share_crc32(0, hdr, n));
   for (i = 1; i <= FE_SHARE_ID_SAMPLES; i++)
   {
      uint32_t so = (uint32_t)(((uint64_t)rom_size * (unsigned)i) /
                               (FE_SHARE_ID_SAMPLES + 1)) & ~0xFFu;
      if (rom_size < 4096u || so + sizeof(blk) > rom_size)
         continue;
      if (read(user, so, blk, sizeof(blk)) != (int)sizeof(blk))
         return -1;
      crc = fe_share_crc32(crc, blk, sizeof(blk));
   }
   put32(id + 8, crc);
   code = console == 0 ? hdr + 12 : hdr;     /* 0xAC game code / title */
   for (i = 0; i < 4; i++)
      id[12 + i] = (code[i] >= 0x20 && code[i] < 0x7F) ? code[i] : '_';
   return 0;
}

/* ---- manifest ------------------------------------------------------------- */

size_t fe_share_manifest_size(unsigned n)
{
   return 8u + (size_t)n * FE_SHARE_ENTRY_WIRE;
}

size_t fe_share_manifest_build(const fe_share_entry *e, unsigned n,
                               uint8_t *out, size_t cap)
{
   unsigned i, k;
   size_t need = fe_share_manifest_size(n);
   if (!out || need > cap || n > FE_SHARE_MAX_ENTRIES || (n && !e))
      return 0;
   memset(out, 0, need);
   memcpy(out, "GSM1", 4);
   put16(out + 4, n);
   for (i = 0; i < n; i++)
   {
      uint8_t *p = out + 8 + (size_t)i * FE_SHARE_ENTRY_WIRE;
      p[0] = e[i].console;
      memcpy(p + 1, e[i].id, FE_SHARE_ID_LEN);
      put32(p + 17, e[i].save_size);
      put32(p + 21, e[i].save_time);
      for (k = 0; k < FE_SHARE_SLOTS; k++)
      {
         put32(p + 25 + k * 9, e[i].st[k].size);
         put32(p + 29 + k * 9, e[i].st[k].time);
         p[33 + k * 9] = e[i].st[k].flags;
      }
      memcpy(p + 70, e[i].name, FE_SHARE_NAME_CAP);
      p[70 + FE_SHARE_NAME_CAP - 1] = '\0';
   }
   return need;
}

int fe_share_manifest_parse(const uint8_t *buf, size_t len,
                            fe_share_entry *out, unsigned max)
{
   unsigned n, i, k, kept = 0;
   if (!buf || len < 8 || memcmp(buf, "GSM1", 4) != 0)
      return -1;
   n = get16(buf + 4);
   if (n > FE_SHARE_MAX_ENTRIES || len != fe_share_manifest_size(n))
      return -1;
   for (i = 0; i < n; i++)
   {
      const uint8_t *p = buf + 8 + (size_t)i * FE_SHARE_ENTRY_WIRE;
      fe_share_entry e;
      memset(&e, 0, sizeof(e));
      if (p[0] >= FE_SHARE_CONSOLES)
         return -1;
      e.console = p[0];
      memcpy(e.id, p + 1, FE_SHARE_ID_LEN);
      e.save_size = get32(p + 17);
      e.save_time = get32(p + 21);
      if (e.save_size > FE_SHARE_MAX_OBJECT)
         return -1;
      for (k = 0; k < FE_SHARE_SLOTS; k++)
      {
         e.st[k].size  = get32(p + 25 + k * 9);
         e.st[k].time  = get32(p + 29 + k * 9);
         e.st[k].flags = p[33 + k * 9];
         if (e.st[k].size > FE_SHARE_MAX_OBJECT ||
             (e.st[k].flags & ~(FE_SHARE_ST_THUMB | FE_SHARE_ST_SHOT)))
            return -1;
         if (!e.st[k].size)
            e.st[k].flags = 0;      /* a preview of nothing is nothing */
      }
      memcpy(e.name, p + 70, FE_SHARE_NAME_CAP);
      e.name[FE_SHARE_NAME_CAP - 1] = '\0';
      for (k = 0; e.name[k]; k++)
         if ((unsigned char)e.name[k] < 0x20 || (unsigned char)e.name[k] > 0x7E)
            e.name[k] = '?';
      /* Entries stay at their index even past `max`: an index is the
       * object's name on the wire.  The caller sizes `out` from the count
       * WELCOME gave; a blob with more than that is refused. */
      if (kept >= max)
         return -1;
      if (out)
         out[kept] = e;
      kept++;
   }
   return (int)kept;
}

int fe_share_match(const fe_share_entry *e, unsigned n, int console,
                   const uint8_t id[FE_SHARE_ID_LEN])
{
   unsigned i;
   for (i = 0; e && i < n; i++)
      if (e[i].console == console &&
          memcmp(e[i].id, id, FE_SHARE_ID_LEN) == 0)
         return (int)i;
   return -1;
}

int fe_share_obj_valid(const fe_share_entry *e, unsigned n, fe_share_obj o)
{
   const fe_share_entry *x;
   if (o.kind == FE_SHARE_OBJ_MANIFEST)
      return o.entry == 0 && o.slot == 0;
   if (!e || o.entry >= n || o.kind >= FE_SHARE_OBJ_KINDS)
      return 0;
   x = &e[o.entry];
   if (o.kind == FE_SHARE_OBJ_SAVE)
      return o.slot == 0 && x->save_size > 0;
   if (o.slot < 1 || o.slot > FE_SHARE_SLOTS || !x->st[o.slot - 1].size)
      return 0;
   if (o.kind == FE_SHARE_OBJ_THUMB)
      return (x->st[o.slot - 1].flags & FE_SHARE_ST_THUMB) != 0;
   if (o.kind == FE_SHARE_OBJ_SHOT)
      return (x->st[o.slot - 1].flags & FE_SHARE_ST_SHOT) != 0;
   return 1;
}

int fe_share_states_ok(const fe_share_ident *a, const fe_share_ident *b,
                       int console)
{
   if (!a || !b || console < 0 || console >= FE_SHARE_CONSOLES)
      return 0;
   if (a->state_fmt[console] != b->state_fmt[console])
      return 0;
#if FE_SHARE_STATES_NEED_SAME_BUILD
   if (strncmp(a->build, b->build, FE_SHARE_BUILD_CAP) != 0)
      return 0;
#endif
   return 1;
}

int fe_share_state_size_ok(int console, uint32_t size)
{
   if (console == 0)
      return size == FE_SHARE_GBA_STATE_SIZE;
   if (console == 1 || console == 2)
      /* header + TGB's block + the extension block (40 + n + 96); never
       * larger than the GBA's, whose buffer stages every state */
      return size > 40u + 96u && size <= FE_SHARE_GBA_STATE_SIZE;
   return 0;
}

int fe_share_state_check(int console, uint32_t state_fmt,
                         const uint8_t *head, size_t head_len, uint32_t size)
{
   if (!head || !fe_share_state_size_ok(console, size))
      return 0;
   if (console == 0)
   {
      /* u32 document length; 0x10 "info-magic" u32; 0x10 "info-version"
       * u32 (the keys with their terminators) */
      static const char k_magic[] = "info-magic", k_ver[] = "info-version";
      const uint8_t *p = head;
      if (head_len < 4 + 1 + sizeof(k_magic) + 4 + 1 + sizeof(k_ver) + 4)
         return 0;
      if (get32(p) != size)
         return 0;
      p += 4;
      if (*p++ != 0x10 || memcmp(p, k_magic, sizeof(k_magic)) != 0)
         return 0;
      p += sizeof(k_magic);
      if (get32(p) != FE_SHARE_GBA_STATE_MAGIC)
         return 0;
      p += 4;
      if (*p++ != 0x10 || memcmp(p, k_ver, sizeof(k_ver)) != 0)
         return 0;
      p += sizeof(k_ver);
      return get32(p) == state_fmt;
   }
   /* GB / GBC (gbcore_tgbdual.c): "GBAT", u16 version (1), the console
    * byte (fe_console_t: 1 GB, 2 GBC), the model, the cartridge header,
    * and at 36 the length of TGB's block. */
   if (head_len < 40)
      return 0;
   if (memcmp(head, "GBAT", 4) != 0 || head[4] != 1 || head[5] != 0 ||
       head[6] != (uint8_t)console)
      return 0;
   return get32(head + 36) == size - 40u - 96u;
}

const char *fe_share_err_name(int e)
{
   switch (e)
   {
   case FE_SHARE_OK:        return "ok";
   case FE_SHARE_E_LOST:    return "lost";
   case FE_SHARE_E_REFUSED: return "refused_protocol";
   case FE_SHARE_E_BUSY:    return "busy";
   case FE_SHARE_E_GONE:    return "gone";
   case FE_SHARE_E_TOO_BIG: return "too_big";
   case FE_SHARE_E_CHANGED: return "checksum";
   case FE_SHARE_E_WRITE:   return "write";
   case FE_SHARE_E_BAD:     return "bad_answer";
   case FE_SHARE_E_CLOSED:  return "closed";
   case FE_SHARE_E_FORMAT:  return "format";
   default:                 return "?";
   }
}

/* ---- datagrams ------------------------------------------------------------ */

/* Header into d; returns the body pointer. */
static uint8_t *dg_begin(uint8_t *d, int type, unsigned session)
{
   memcpy(d, FE_SHARE_MAGIC, 4);
   d[4] = FE_SHARE_PROTO;
   d[5] = (uint8_t)type;
   /* Keep discovery/refusal framing readable by v1 rooms: neither has an
    * established session. Old hosts can then route REFUSE to the real MAC. */
   if (type == T_HELLO || type == T_REFUSE)
   {
      put16(d + 6, 0);
      return d + 8;
   }
   put32(d + 6, session);
   return d + FE_SHARE_HDR;
}

/* Append the CRC; returns the datagram's length. */
static size_t dg_end(uint8_t *d, uint8_t *body_end)
{
   size_t n = (size_t)(body_end - d);
   put32(body_end, fe_share_crc32(0, d, n));
   return n + 4;
}

/* Magic, minimum length and CRC.  On success *type, *session, *proto, and
 * the body (pointer + length). */
static int dg_check(const void *buf, size_t len, int *proto, int *type,
                    unsigned *session, const uint8_t **body, size_t *blen)
{
   const uint8_t *d = (const uint8_t *)buf;
   size_t hdr;
   if (!d || len < 12 || len > FE_SHARE_DGRAM_MAX ||
       memcmp(d, FE_SHARE_MAGIC, 4) != 0)
      return -1;
   hdr = d[4] == 1 || d[5] == T_HELLO || d[5] == T_REFUSE ? 8u : FE_SHARE_HDR;
   if (len < hdr + 4)
      return -1;
   if (get32(d + len - 4) != fe_share_crc32(0, d, len - 4))
      return -1;
   *proto = d[4];
   *type = d[5];
   *session = hdr == 8 ? get16(d + 6) : get32(d + 6);
   *body = d + hdr;
   *blen = len - hdr - 4;
   return 0;
}

static uint8_t *put_obj(uint8_t *p, fe_share_obj o)
{
   put16(p, o.entry);
   p[2] = o.kind;
   p[3] = o.slot;
   return p + 4;
}

static fe_share_obj get_obj(const uint8_t *p)
{
   fe_share_obj o;
   o.entry = (uint16_t)get16(p);
   o.kind = p[2];
   o.slot = p[3];
   return o;
}

static int obj_eq(fe_share_obj a, fe_share_obj b)
{
   return a.entry == b.entry && a.kind == b.kind && a.slot == b.slot;
}

static uint8_t *put_ident(uint8_t *p, const fe_share_ident *id, unsigned nonce)
{
   int i;
   memcpy(p, id->mac, 6);
   put32(p + 6, nonce);
   memcpy(p + 10, id->build, FE_SHARE_BUILD_CAP);
   for (i = 0; i < FE_SHARE_CONSOLES; i++)
      put32(p + 10 + FE_SHARE_BUILD_CAP + 4 * i, id->state_fmt[i]);
   return p + 10 + FE_SHARE_BUILD_CAP + 4 * FE_SHARE_CONSOLES;
}
#define IDENT_WIRE (10u + FE_SHARE_BUILD_CAP + 4u * FE_SHARE_CONSOLES)

static unsigned get_ident(const uint8_t *p, fe_share_ident *id)
{
   int i;
   memcpy(id->mac, p, 6);
   memcpy(id->build, p + 10, FE_SHARE_BUILD_CAP);
   id->build[FE_SHARE_BUILD_CAP - 1] = '\0';
   for (i = 0; id->build[i]; i++)
      if ((unsigned char)id->build[i] < 0x20 ||
          (unsigned char)id->build[i] > 0x7E)
         id->build[i] = '?';
   for (i = 0; i < FE_SHARE_CONSOLES; i++)
      id->state_fmt[i] = get32(p + 10 + FE_SHARE_BUILD_CAP + 4 * i);
   return get32(p + 6);
}

static uint32_t chunks_of(uint32_t size)
{
   return (size + FE_SHARE_CHUNK - 1) / FE_SHARE_CHUNK;
}

/* ====================== server ============================================ */

void fe_share_server_init(fe_share_server *s, const fe_share_server_io *io,
                          const fe_share_ident *me,
                          const fe_share_entry *entries, unsigned n,
                          const uint8_t *manifest, uint32_t manifest_size,
                          uint32_t session_seed)
{
   unsigned i, k;
   memset(s, 0, sizeof(*s));
   s->io = *io;
   s->me = *me;
   s->me.build[FE_SHARE_BUILD_CAP - 1] = '\0';
   s->entries = entries;
   s->n = n;
   s->manifest = manifest;
   s->manifest_size = manifest_size;
   s->manifest_crc = fe_share_crc32(0, manifest, manifest_size);
   s->session_seed = session_seed ? session_seed : 1;
   for (i = 0; i < n; i++)
   {
      s->n_saves += entries[i].save_size ? 1u : 0u;
      for (k = 0; k < FE_SHARE_SLOTS; k++)
         s->n_states += entries[i].st[k].size ? 1u : 0u;
   }
}

static void srv_close_obj(fe_share_server *s)
{
   if (s->have_obj && s->obj.kind != FE_SHARE_OBJ_MANIFEST && s->io.close)
      s->io.close(s->io.user);
   s->have_obj = 0;
   s->win_mask = 0;
}

static void srv_send(fe_share_server *s, uint8_t *d, uint8_t *end)
{
   size_t n = dg_end(d, end);
   s->io.send(s->io.user, s->peer, d, n);
}

static void srv_refuse(fe_share_server *s, const uint8_t mac[6], int code)
{
   uint8_t d[64], *p = dg_begin(d, T_REFUSE, 0);
   p[0] = (uint8_t)code;
   p[1] = FE_SHARE_PROTO;
   memcpy(p + 2, s->me.build, FE_SHARE_BUILD_CAP);
   s->io.send(s->io.user, mac, d, dg_end(d, p + 2 + FE_SHARE_BUILD_CAP));
   s->rx_refused++;
}

static void srv_info(fe_share_server *s, fe_share_obj o, unsigned req,
                     int status)
{
   uint8_t d[64], *p = dg_begin(d, T_INFO, s->session);
   p = put_obj(p, o);
   put16(p, req);
   p[2] = (uint8_t)status;
   put32(p + 3, status == ST_OK ? s->obj_size : 0);
   put32(p + 7, status == ST_OK ? s->obj_crc : 0);
   srv_send(s, d, p + 11);
}

static void srv_welcome(fe_share_server *s, unsigned nonce)
{
   uint8_t d[96], *p = dg_begin(d, T_WELCOME, s->session);
   p = put_ident(p, &s->me, nonce);
   put16(p, s->n);
   put16(p + 2, s->n_saves);
   put16(p + 4, s->n_states);
   put32(p + 6, s->manifest_size);
   put32(p + 10, s->manifest_crc);
   srv_send(s, d, p + 14);
}

/* Open `o`, size it and checksum the whole of it.  Returns a ST_* code. */
static int srv_open(fe_share_server *s, fe_share_obj o)
{
   uint8_t blk[FE_SHARE_CHUNK];
   uint32_t size = 0, off, crc = 0;
   srv_close_obj(s);
   if (!fe_share_obj_valid(s->entries, s->n, o))
      return ST_BAD;
   if (o.kind == FE_SHARE_OBJ_MANIFEST)
   {
      s->obj_size = s->manifest_size;
      s->obj_crc = s->manifest_crc;
   }
   else
   {
      if (s->io.open(s->io.user, o, &size) != 0)
         return ST_GONE;
      if (size > FE_SHARE_MAX_OBJECT)
      {
         s->io.close(s->io.user);
         return ST_TOO_BIG;
      }
      for (off = 0; off < size; off += FE_SHARE_CHUNK)
      {
         uint32_t n = size - off < FE_SHARE_CHUNK ? size - off
                                                  : FE_SHARE_CHUNK;
         if (s->io.read(s->io.user, off, blk, n) != (int)n)
         {
            s->io.close(s->io.user);
            return ST_GONE;
         }
         crc = fe_share_crc32(crc, blk, n);
      }
      s->obj_size = size;
      s->obj_crc = crc;
   }
   s->obj = o;
   s->have_obj = 1;
   s->sent_chunks = 0;
   s->hi_chunk = 0;
   s->objects_served++;
   return ST_OK;
}

void fe_share_server_rx(fe_share_server *s, const void *buf, size_t len,
                        uint32_t now_ms)
{
   const uint8_t *b;
   size_t bl;
   int proto, type;
   unsigned session;
   if (dg_check(buf, len, &proto, &type, &session, &b, &bl) != 0)
   {
      s->rx_bad++;
      return;
   }
   if (proto != FE_SHARE_PROTO)
   {
      /* Another generation.  Every generation's HELLO starts with the
       * sender's MAC, so it can at least be told why. */
      if (type == T_HELLO && bl >= 6)
         srv_refuse(s, b, REFUSE_PROTO);
      else
         s->rx_bad++;
      return;
   }
   if (type == T_HELLO)
   {
      fe_share_ident id;
      unsigned nonce;
      if (bl != IDENT_WIRE)
      {
         s->rx_bad++;
         return;
      }
      nonce = get_ident(b, &id);
      if (s->state == FE_SHARE_SRV_CONNECTED &&
          memcmp(id.mac, s->peer, 6) != 0)
      {
         srv_refuse(s, id.mac, REFUSE_BUSY);
         return;
      }
      if (!nonce || session)
      { s->rx_bad++; return; }
      if (s->state == FE_SHARE_SRV_CONNECTED && nonce == s->peer_nonce)
      {
         /* Repeat the active OFFER as well: a delayed obsolete OFFER may
          * have arrived before WELCOME. The next HELLO repairs that race. */
         uint8_t d[96], *p = dg_begin(d, T_OFFER, s->session);
         p = put_ident(p, &s->me, nonce);
         s->io.send(s->io.user, id.mac, d, dg_end(d, p));
         return;
      }
      if (!s->offer_session || nonce != s->offer_nonce ||
          memcmp(id.mac, s->offer_ident.mac, 6) ||
          (uint32_t)(now_ms - s->offer_ms) >= SERVER_GUEST_TIMEOUT)
      {
         s->offer_session = s->session_seed++;
         if (!s->offer_session)
            s->offer_session = s->session_seed++;
         s->offer_nonce = nonce;
         s->offer_ident = id;
      }
      s->offer_ms = now_ms;
      {
         uint8_t d[96], *p = dg_begin(d, T_OFFER, s->offer_session);
         p = put_ident(p, &s->me, nonce);
         s->io.send(s->io.user, id.mac, d, dg_end(d, p));
      }
      s->rx_ok++;
      return;
   }
   if (type == T_CONFIRM)
   {
      fe_share_ident id;
      unsigned nonce;
      if (bl != IDENT_WIRE)
      { s->rx_bad++; return; }
      nonce = get_ident(b, &id);
      if (s->state == FE_SHARE_SRV_CONNECTED && session == s->session &&
          nonce == s->peer_nonce && !memcmp(id.mac, s->peer, 6))
      { srv_welcome(s, nonce); return; }
      if (!session || session != s->offer_session || nonce != s->offer_nonce ||
          memcmp(id.mac, s->offer_ident.mac, 6) ||
          (uint32_t)(now_ms - s->offer_ms) >= SERVER_GUEST_TIMEOUT ||
          (s->state == FE_SHARE_SRV_CONNECTED && memcmp(id.mac, s->peer, 6)))
      { s->rx_bad++; return; }
      srv_close_obj(s);
      s->have_req = 0;
      s->session = session;
      s->peer_nonce = nonce;
      memcpy(s->peer, id.mac, 6);
      s->peer_ident = s->offer_ident;
      s->offer_session = 0;
      s->state = FE_SHARE_SRV_CONNECTED;
      s->last_rx_ms = now_ms;
      s->rx_ok++;
      srv_welcome(s, nonce);
      return;
   }
   if (s->state != FE_SHARE_SRV_CONNECTED || session != s->session)
   {
      s->rx_bad++;
      return;
   }
   switch (type)
   {
   case T_PING:
   {
      uint8_t d[16], *p = dg_begin(d, T_PONG, s->session);
      if (bl != 0)
         break;
      srv_send(s, d, p);
      goto good;
   }
   case T_BYE:
      if (bl != 0)
         break;
      srv_close_obj(s);
      s->offer_session = 0;
      s->state = FE_SHARE_SRV_WAITING;
      s->rx_ok++;
      return;
   case T_STAT:
   {
      fe_share_obj o;
      unsigned req;
      int st;
      if (bl != 6)
         break;
      o = get_obj(b);
      req = get16(b + 4);
      if (s->have_req && (int16_t)(req - s->req) < 0)
         break;                 /* delayed STAT from an earlier fetch */
      if (s->have_obj && req == s->req && !obj_eq(o, s->obj))
         break;                 /* one request identifies one object */
      if (s->have_obj && req == s->req && obj_eq(o, s->obj))
         st = ST_OK;                       /* a repeated STAT: same answer */
      else
         st = srv_open(s, o);
      srv_info(s, o, req, st);
      if (st == ST_BAD)
         break;
      s->req = (uint16_t)req;
      s->have_req = 1;
      goto good;
   }
   case T_GET:
   {
      fe_share_obj o;
      unsigned req;
      uint32_t first, mask, nch;
      if (bl != 14)
         break;
      o = get_obj(b);
      req = get16(b + 4);
      first = get32(b + 6);
      mask = get32(b + 10);
      if (!s->have_req || req != s->req)
         break;                 /* old GET must not replace a newer window */
      if (!s->have_obj || !obj_eq(o, s->obj))
      {
         if (!fe_share_obj_valid(s->entries, s->n, o))
            break;
         srv_info(s, o, req, ST_NOT_OPEN);
         goto good;
      }
      nch = chunks_of(s->obj_size);
      if (first >= nch)
         break;
      if (nch - first < 32u)
         mask &= (1u << (nch - first)) - 1u;
      s->req = (uint16_t)req;
      s->win_first = first;
      s->win_mask = mask;
      goto good;
   }
   default:
      break;
   }
   s->rx_bad++;
   return;
good:
   s->rx_ok++;
   s->last_rx_ms = now_ms;
}

void fe_share_server_step(fe_share_server *s, uint32_t now_ms,
                          unsigned budget)
{
   if (s->state != FE_SHARE_SRV_CONNECTED)
      return;
   if ((uint32_t)(now_ms - s->last_rx_ms) > SERVER_GUEST_TIMEOUT)
   {
      srv_close_obj(s);
      s->state = FE_SHARE_SRV_WAITING;
      return;
   }
   while (budget && s->have_obj && s->win_mask)
   {
      uint8_t d[FE_SHARE_DGRAM_MAX], *p;
      uint32_t bit = 0, chunk, off, n;
      while (!(s->win_mask & (1u << bit)))
         bit++;
      s->win_mask &= ~(1u << bit);
      chunk = s->win_first + bit;
      off = chunk * FE_SHARE_CHUNK;
      if (off >= s->obj_size)
         continue;
      n = s->obj_size - off < FE_SHARE_CHUNK ? s->obj_size - off
                                             : FE_SHARE_CHUNK;
      p = dg_begin(d, T_DATA, s->session);
      p = put_obj(p, s->obj);
      put16(p, s->req);
      put32(p + 2, chunk);
      put16(p + 6, n);
      if (s->obj.kind == FE_SHARE_OBJ_MANIFEST)
         memcpy(p + 8, s->manifest + off, n);
      else if (s->io.read(s->io.user, off, p + 8, n) != (int)n)
      {
         /* The file shrank or went away under us: say so, serve no more. */
         fe_share_obj o = s->obj;
         unsigned req = s->req;
         srv_close_obj(s);
         srv_info(s, o, req, ST_GONE);
         return;
      }
      srv_send(s, d, p + 8 + n);
      s->tx_data++;
      if (chunk >= s->hi_chunk)
      {
         s->hi_chunk = chunk + 1;
         s->sent_chunks = s->hi_chunk;
      }
      budget--;
   }
}

void fe_share_server_stop(fe_share_server *s)
{
   if (s->state == FE_SHARE_SRV_CONNECTED)
   {
      uint8_t d[16], *p = dg_begin(d, T_BYE, s->session);
      srv_send(s, d, p);
   }
   srv_close_obj(s);
   s->offer_session = 0;
   s->state = FE_SHARE_SRV_WAITING;
}

/* ====================== client ============================================ */

void fe_share_client_init(fe_share_client *c, const fe_share_client_io *io,
                          const fe_share_ident *me, uint32_t nonce)
{
   memset(c, 0, sizeof(*c));
   c->io = *io;
   c->me = *me;
   c->me.build[FE_SHARE_BUILD_CAP - 1] = '\0';
   c->nonce = nonce ? nonce : 1;
}

static void cl_send(fe_share_client *c, const uint8_t mac[6], uint8_t *d,
                    uint8_t *end, uint32_t now_ms)
{
   c->io.send(c->io.user, mac, d, dg_end(d, end));
   c->t_last_tx = now_ms;
}

static void cl_fail(fe_share_client *c, int err)
{
   c->err = err;
   c->state = FE_SHARE_CL_ERROR;
   c->win_mask = 0;
}

int fe_share_client_linked(const fe_share_client *c)
{
   return c->session != 0 && c->err != FE_SHARE_E_LOST &&
          c->err != FE_SHARE_E_CLOSED && c->err != FE_SHARE_E_REFUSED &&
          c->err != FE_SHARE_E_BUSY && c->state != FE_SHARE_CL_IDLE &&
          c->state != FE_SHARE_CL_HELLO;
}

static void cl_hello(fe_share_client *c, uint32_t now_ms)
{
   uint8_t d[96], *p = dg_begin(d, T_HELLO, 0);
   p = put_ident(p, &c->me, c->nonce);
   cl_send(c, BROADCAST, d, p, now_ms);
}

void fe_share_client_hello(fe_share_client *c, uint32_t now_ms)
{
   if (c->hello_started && !++c->nonce)
      ++c->nonce;
   c->hello_started = 1;
   c->offer_session = 0;
   c->state = FE_SHARE_CL_HELLO;
   c->err = 0;
   c->session = 0;
   c->t_start = c->t_last_rx = now_ms;
   cl_hello(c, now_ms);
}

static void cl_stat(fe_share_client *c, uint32_t now_ms)
{
   uint8_t d[32], *p = dg_begin(d, T_STAT, c->session);
   p = put_obj(p, c->obj);
   put16(p, c->req);
   cl_send(c, c->host.mac, d, p + 2, now_ms);
}

static int cl_has(const fe_share_client *c, uint32_t chunk)
{
   return (c->have[chunk >> 3] >> (chunk & 7)) & 1;
}

/* Ask for the next window: from the lowest chunk still missing. */
static void cl_get(fe_share_client *c, uint32_t now_ms, int fresh)
{
   uint8_t d[32], *p;
   uint32_t first = 0, mask = 0, i;
   if (fresh || !c->win_mask)
   {
      while (first < c->nchunks && cl_has(c, first))
         first++;
      if (first >= c->nchunks)
         return;
      for (i = 0; i < FE_SHARE_WINDOW && first + i < c->nchunks; i++)
         if (!cl_has(c, first + i))
            mask |= 1u << i;
      c->win_first = first;
      c->win_mask = mask;
   }
   p = dg_begin(d, T_GET, c->session);
   p = put_obj(p, c->obj);
   put16(p, c->req);
   put32(p + 2, c->win_first);
   put32(p + 6, c->win_mask);
   cl_send(c, c->host.mac, d, p + 10, now_ms);
   c->t_last_data = now_ms;
   c->tx_get++;
}

int fe_share_client_fetch(fe_share_client *c, fe_share_obj o,
                          const fe_share_sink *sink, uint32_t now_ms)
{
   if (!fe_share_client_linked(c) || !sink || !sink->write || !sink->read ||
       c->state == FE_SHARE_CL_STAT || c->state == FE_SHARE_CL_DATA)
      return -1;
   c->obj = o;
   c->sink = *sink;
   c->expect = 0;
   c->exp_state = 0;
   c->req = (uint16_t)(c->req + 1);
   c->size = c->crc = c->nchunks = c->got = 0;
   c->win_mask = 0;
   c->err = 0;
   c->state = FE_SHARE_CL_STAT;
   cl_stat(c, now_ms);
   return 0;
}

void fe_share_client_expect(fe_share_client *c, uint32_t size)
{
   c->expect = size;
}

void fe_share_client_expect_state(fe_share_client *c, int console,
                                  uint32_t state_fmt)
{
   c->exp_state = console + 1;
   c->exp_fmt = state_fmt;
}

void fe_share_client_cancel(fe_share_client *c)
{
   if (c->state == FE_SHARE_CL_STAT || c->state == FE_SHARE_CL_DATA)
   {
      c->req = (uint16_t)(c->req + 1);   /* late answers no longer match */
      c->win_mask = 0;
      c->state = FE_SHARE_CL_READY;
   }
}

/* Every chunk is in: read back what the sink holds and compare the whole
 * payload's CRC with the sharer's. */
static void cl_verify(fe_share_client *c)
{
   uint8_t blk[FE_SHARE_CHUNK];
   uint32_t off, crc = 0;
   for (off = 0; off < c->size; off += FE_SHARE_CHUNK)
   {
      uint32_t n = c->size - off < FE_SHARE_CHUNK ? c->size - off
                                                  : FE_SHARE_CHUNK;
      if (c->sink.read(c->sink.user, off, blk, n) != (int)n)
      {
         cl_fail(c, FE_SHARE_E_WRITE);
         return;
      }
      crc = fe_share_crc32(crc, blk, n);
   }
   if (crc != c->crc)
   {
      cl_fail(c, FE_SHARE_E_CHANGED);
      return;
   }
   if (c->exp_state)
   {
      /* complete and as the sharer sent it -- but is it a state? */
      uint32_t n = c->size < FE_SHARE_STATE_HEAD ? c->size
                                                 : FE_SHARE_STATE_HEAD;
      if ((n && c->sink.read(c->sink.user, 0, blk, n) != (int)n) ||
          !fe_share_state_check(c->exp_state - 1, c->exp_fmt, blk, n,
                                c->size))
      {
         cl_fail(c, FE_SHARE_E_FORMAT);
         return;
      }
   }
   c->state = FE_SHARE_CL_DONE;
}

void fe_share_client_rx(fe_share_client *c, const void *buf, size_t len,
                        uint32_t now_ms)
{
   const uint8_t *b;
   size_t bl;
   int proto, type;
   unsigned session;
   if (c->state == FE_SHARE_CL_IDLE ||
       dg_check(buf, len, &proto, &type, &session, &b, &bl) != 0)
   {
      c->rx_bad++;
      return;
   }
   if (type == T_REFUSE)
   {
      /* Fixed layout in every generation.  Only meaningful while we are
       * still asking to be let in. */
      if (bl < 2 || c->state != FE_SHARE_CL_HELLO)
      {
         c->rx_bad++;
         return;
      }
      c->refused_proto = b[1];
      if (bl >= 2 + FE_SHARE_BUILD_CAP)
      {
         memcpy(c->host.build, b + 2, FE_SHARE_BUILD_CAP);
         c->host.build[FE_SHARE_BUILD_CAP - 1] = '\0';
      }
      cl_fail(c, b[0] == REFUSE_BUSY ? FE_SHARE_E_BUSY : FE_SHARE_E_REFUSED);
      return;
   }
   if (proto != FE_SHARE_PROTO)
   {
      if (c->state == FE_SHARE_CL_HELLO)
      {
         c->refused_proto = proto;
         cl_fail(c, FE_SHARE_E_REFUSED);
      }
      else
         c->rx_bad++;
      return;
   }
   if (type == T_OFFER)
   {
      fe_share_ident id;
      uint8_t d[96], *p;
      if (c->state != FE_SHARE_CL_HELLO || bl != IDENT_WIRE || !session ||
          get_ident(b, &id) != c->nonce)
      { c->rx_bad++; return; }
      c->offer_session = session;
      c->host = id;
      p = dg_begin(d, T_CONFIRM, session);
      p = put_ident(p, &c->me, c->nonce);
      cl_send(c, id.mac, d, p, now_ms);
      c->rx_ok++;
      return;
   }
   if (type == T_WELCOME)
   {
      fe_share_ident id;
      if (c->state != FE_SHARE_CL_HELLO || bl != IDENT_WIRE + 14 ||
          get_ident(b, &id) != c->nonce || !session ||
          session != c->offer_session || memcmp(id.mac, c->host.mac, 6))
      {
         c->rx_bad++;
         return;
      }
      b += IDENT_WIRE;
      c->host = id;
      c->session = session;
      c->n_entries = get16(b);
      c->n_saves = get16(b + 2);
      c->n_states = get16(b + 4);
      c->manifest_size = get32(b + 6);
      c->manifest_crc = get32(b + 10);
      if (c->n_entries > FE_SHARE_MAX_ENTRIES ||
          c->manifest_size != fe_share_manifest_size(c->n_entries))
      {
         cl_fail(c, FE_SHARE_E_BAD);
         c->session = 0;
         return;
      }
      c->state = FE_SHARE_CL_READY;
      c->t_last_rx = now_ms;
      c->rx_ok++;
      return;
   }
   if (!fe_share_client_linked(c) || session != c->session)
   {
      c->rx_bad++;
      return;
   }
   switch (type)
   {
   case T_PONG:
      if (bl != 0)
         break;
      goto good;
   case T_BYE:
      if (bl != 0)
         break;
      cl_fail(c, FE_SHARE_E_CLOSED);
      return;
   case T_INFO:
   {
      fe_share_obj o;
      int st;
      if (bl != 15)
         break;
      o = get_obj(b);
      st = b[6];
      if ((c->state != FE_SHARE_CL_STAT && c->state != FE_SHARE_CL_DATA) ||
          !obj_eq(o, c->obj) || get16(b + 4) != c->req)
         goto stale;
      if (st == ST_NOT_OPEN)
      {
         /* The sharer no longer has it open: start this fetch again. */
         memset(c->have, 0, sizeof(c->have));
         c->got = 0;
         c->win_mask = 0;
         c->state = FE_SHARE_CL_STAT;
         cl_stat(c, now_ms);
         goto good;
      }
      if (st != ST_OK)
      {
         cl_fail(c, st == ST_TOO_BIG ? FE_SHARE_E_TOO_BIG :
                    st == ST_GONE ? FE_SHARE_E_GONE : FE_SHARE_E_BAD);
         goto good;
      }
      if (c->state != FE_SHARE_CL_STAT)
         goto stale;                       /* a repeated INFO */
      c->size = get32(b + 7);
      c->crc = get32(b + 11);
      if (c->size > FE_SHARE_MAX_OBJECT ||
          (c->sink.cap && c->size > c->sink.cap))
      {
         cl_fail(c, FE_SHARE_E_TOO_BIG);
         goto good;
      }
      if (c->expect && c->size != c->expect)
      {
         /* not the file the manifest listed (empty, or another size) */
         cl_fail(c, FE_SHARE_E_CHANGED);
         goto good;
      }
      if (c->exp_state &&
          !fe_share_state_size_ok(c->exp_state - 1, c->size))
      {
         /* no state of this console has that size: do not fetch it */
         cl_fail(c, FE_SHARE_E_FORMAT);
         goto good;
      }
      c->nchunks = chunks_of(c->size);
      memset(c->have, 0, sizeof(c->have));
      c->got = 0;
      c->state = FE_SHARE_CL_DATA;
      if (!c->nchunks)
         cl_verify(c);
      else
         cl_get(c, now_ms, 1);
      goto good;
   }
   case T_DATA:
   {
      fe_share_obj o;
      uint32_t chunk, n, want;
      if (bl < 12)
         break;
      o = get_obj(b);
      chunk = get32(b + 6);
      n = get16(b + 10);
      if (bl != 12 + n)
         break;
      if (c->state != FE_SHARE_CL_DATA || !obj_eq(o, c->obj) ||
          get16(b + 4) != c->req)
         goto stale;
      if (chunk >= c->nchunks)
         break;
      want = chunk + 1 == c->nchunks
                ? c->size - chunk * FE_SHARE_CHUNK : FE_SHARE_CHUNK;
      if (n != want)
         break;
      c->t_last_data = now_ms;
      if (cl_has(c, chunk))
      {
         c->rx_dup++;
         goto good;
      }
      if (c->sink.write(c->sink.user, chunk * FE_SHARE_CHUNK, b + 12, n) != 0)
      {
         cl_fail(c, FE_SHARE_E_WRITE);
         goto good;
      }
      c->have[chunk >> 3] |= (uint8_t)(1u << (chunk & 7));
      c->got++;
      if (chunk >= c->win_first && chunk - c->win_first < 32u)
         c->win_mask &= ~(1u << (chunk - c->win_first));
      if (c->got == c->nchunks)
         cl_verify(c);
      else if (!c->win_mask)
         cl_get(c, now_ms, 1);             /* window complete: the next */
      goto good;
   }
   default:
      break;
   }
   c->rx_bad++;
   return;
stale:
   c->rx_dup++;
   c->t_last_rx = now_ms;       /* stale, but the sharer is alive */
   return;
good:
   c->rx_ok++;
   c->t_last_rx = now_ms;
}

void fe_share_client_step(fe_share_client *c, uint32_t now_ms)
{
   if (c->state == FE_SHARE_CL_IDLE)
      return;
   if (c->state == FE_SHARE_CL_HELLO)
   {
      if ((uint32_t)(now_ms - c->t_start) > FE_SHARE_HELLO_GIVEUP)
         cl_fail(c, FE_SHARE_E_LOST);
      else if ((uint32_t)(now_ms - c->t_last_tx) >= FE_SHARE_HELLO_EVERY)
         cl_hello(c, now_ms);
      return;
   }
   if (!fe_share_client_linked(c))
      return;
   if ((uint32_t)(now_ms - c->t_last_rx) > FE_SHARE_LOST_AFTER)
   {
      cl_fail(c, FE_SHARE_E_LOST);
      return;
   }
   if (c->state == FE_SHARE_CL_STAT)
   {
      if ((uint32_t)(now_ms - c->t_last_tx) >= FE_SHARE_RETRY_AFTER)
      {
         cl_stat(c, now_ms);
         c->retries++;
      }
   }
   else if (c->state == FE_SHARE_CL_DATA)
   {
      if ((uint32_t)(now_ms - c->t_last_data) >= FE_SHARE_RETRY_AFTER)
      {
         cl_get(c, now_ms, 0);             /* what is still missing */
         c->retries++;
      }
   }
   else if ((uint32_t)(now_ms - c->t_last_tx) >= FE_SHARE_PING_EVERY)
   {
      uint8_t d[16], *p = dg_begin(d, T_PING, c->session);
      cl_send(c, c->host.mac, d, p, now_ms);
   }
}

void fe_share_client_bye(fe_share_client *c)
{
   if (fe_share_client_linked(c))
   {
      uint8_t d[16], *p = dg_begin(d, T_BYE, c->session);
      c->io.send(c->io.user, c->host.mac, d, dg_end(d, p));
   }
   c->state = FE_SHARE_CL_IDLE;
   c->session = 0;
}

/* ====================== commit ============================================ */

/* Mend what an interrupted fe_share_commit leaves (power lost between its
 * two renames, or a roll-back that failed): `final` gone while the file it
 * replaced still sits beside it as final.bak (a save) or final.old (a
 * state).  Without this the game booted with NO save, the player saved, and
 * the next pull deleted the only copy of the original (review finding 5).
 * Also removes the leftovers nothing else looks for: a stray temp file
 * (final + tmp_suffix) and a final.old beside a final that exists.  Call
 * when no transfer is in progress: at game load and when a join scans the
 * player's files.  Returns 1 = `final` was restored, 0 = nothing to mend. */
int fe_share_recover(const fe_share_fs *fs, const char *final,
                     const char *tmp_suffix, char *scratch, size_t cap)
{
   /* .old first: it is the newer of the two -- an interrupted save's own
    * previous file (fe_host's write-temp/move-aside/move-in), or this
    * module's non-kept commit -- while a .bak is the backup a share pull
    * keeps from before it, possibly long ago (fe_host save_recover_bak
    * resolves them in the same order). */
   static const char *const suf[2] = { ".old", ".bak" };
   int i, restored = 0;
   if (!fs || !final || !scratch)
      return 0;
   if (tmp_suffix &&
       snprintf(scratch, cap, "%s%s", final, tmp_suffix) < (int)cap &&
       fs->exists(fs->user, scratch))
      fs->remove(fs->user, scratch);
   if (!fs->exists(fs->user, final))
   {
      for (i = 0; i < 2 && !restored; i++)
         if (snprintf(scratch, cap, "%s%s", final, suf[i]) < (int)cap &&
             fs->exists(fs->user, scratch) &&
             fs->rename(fs->user, scratch, final) == 0)
            restored = 1;
   }
   if (fs->exists(fs->user, final) &&
       snprintf(scratch, cap, "%s.old", final) < (int)cap &&
       fs->exists(fs->user, scratch))
      fs->remove(fs->user, scratch);      /* .bak is the one kept backup */
   return restored;
}

int fe_share_commit(const fe_share_fs *fs, const char *final,
                    const char *tmp, int keep_bak, char *scratch, size_t cap)
{
   int moved = 0;
   if (!fs || !final || !tmp || !scratch ||
       snprintf(scratch, cap, "%s%s", final, keep_bak ? ".bak" : ".old")
          >= (int)cap)
   {
      if (fs && tmp)
         fs->remove(fs->user, tmp);
      return -1;
   }
   if (!fs->exists(fs->user, tmp))
      return -1;
   if (fs->exists(fs->user, final))
   {
      /* ONE backup: whatever was there before goes. */
      fs->remove(fs->user, scratch);
      if (fs->rename(fs->user, final, scratch) != 0)
      {
         fs->remove(fs->user, tmp);
         return -1;
      }
      moved = 1;
   }
   if (fs->rename(fs->user, tmp, final) != 0)
   {
      int back = 0;
      if (moved)
         back = fs->rename(fs->user, scratch, final); /* put the old one back */
      fs->remove(fs->user, tmp);
      /* -2: the old file could not be put back either: it is in `scratch`
       * (final.bak / final.old) and `final` does not exist.  The caller
       * must not say "not changed"; fe_share_recover mends it. */
      return (moved && back != 0) ? -2 : -1;
   }
   if (moved && !keep_bak)
      fs->remove(fs->user, scratch);
   return 0;
}
