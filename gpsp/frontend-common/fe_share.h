/* fe_share.h -- "Share game data": save files and save states from one PSP
 * to another over ad-hoc wireless (docs/SAVE-SHARE.md).
 *
 * WHAT THIS MODULE IS.  The platform-independent half: which files on a
 * stick are a game's data (fe_share_classify), how a game is recognised on
 * two consoles (fe_share_game_id), the manifest, the wire protocol with its
 * two state machines (the READ-ONLY server on the sharing console, the
 * client on the joining one), and the write-to-temp / verify / swap commit.
 * It does no I/O of its own: datagrams, files and the clock come through
 * callbacks, so tools/tests/test_fe_share.c runs both ends in one process
 * over a lossy fake radio and a real temp directory.
 *
 * WHAT NEVER CROSSES.  A ROM.  The protocol has no object kind for one and
 * the server has no way to name one: a request carries an entry INDEX into
 * the server's own manifest plus a kind (save / state / its two preview
 * files) and a slot number -- never a path -- and the platform derives the
 * file name from its own table.  Nothing a client sends is ever used as, or
 * as part of, a file name, and the server opens files read-only.
 *
 * THE WIRE (little endian, one datagram = one message, FE_SHARE_DGRAM_MAX
 * bytes at most, on the ad-hoc transport's bulk lane beside nothing else:
 * a share room runs no link session):
 *
 *   magic "GSHR" | proto u8 | type u8 | session u32 | body | crc32
 * HELLO/REFUSE retain the v1 8-byte header with a zero u16 session so mixed
 * generations can route and parse the refusal; other v2 headers are 10 bytes.
 *
 * The CRC covers everything before it.  `session` is 0 in HELLO and is the
 * server's provisional challenge in OFFER/CONFIRM and per-guest number from
 * WELCOME on; HELLO never replaces a guest until its CONFIRM matches. A datagram with the wrong one
 * is dropped.  A datagram whose proto byte is not FE_SHARE_PROTO is
 * answered with REFUSE (whose layout never changes), so two different
 * protocol generations tell their players to update instead of timing out.
 *
 * RECEIVER-DRIVEN.  The client asks for an object's size and checksum
 * (STAT -> INFO; the server reads the whole file once to answer), then for
 * windows of up to 32 chunks (GET: first chunk + bitmap), re-asking for
 * whatever did not arrive.  The server keeps one object open and one window
 * pending; it has no per-transfer state a lost datagram can corrupt.  When
 * every chunk is in, the client reads back what it wrote and compares the
 * CRC32 of the WHOLE payload with INFO's: only then is the fetch DONE.
 */
#ifndef FE_SHARE_H
#define FE_SHARE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FE_SHARE_MAGIC        "GSHR"
#define FE_SHARE_PROTO        2
#define FE_SHARE_CHUNK        1024u
#define FE_SHARE_HDR          10u
#define FE_SHARE_DGRAM_MAX    (FE_SHARE_HDR + 16u + FE_SHARE_CHUNK + 4u)
#define FE_SHARE_SLOTS        5
#define FE_SHARE_CONSOLES     3            /* fe_console_t: GBA, GB, GBC */
#define FE_SHARE_MAX_ENTRIES  1024         /* the browser's own list cap */
/* The largest object either side will touch.  A GBA state is ~416 KiB and
 * a save at most 128 KiB; 2 MiB leaves room and bounds every bitmap. */
#define FE_SHARE_MAX_OBJECT   (2u * 1024u * 1024u)
#define FE_SHARE_MAX_CHUNKS   (FE_SHARE_MAX_OBJECT / FE_SHARE_CHUNK)
#define FE_SHARE_ID_LEN       16
#define FE_SHARE_NAME_CAP     48
#define FE_SHARE_BUILD_CAP    24
#define FE_SHARE_ENTRY_WIRE   120u
#define FE_SHARE_WINDOW       32u

/* THE ONE-LINE POLICY SWITCHES (owner defaults, 2026-10-10).
 * States are offered only between identical builds; 0 = the state format
 * ids alone decide (same format, different build: offered). */
#define FE_SHARE_STATES_NEED_SAME_BUILD 1
/* Content samples in the game identity (see fe_share_game_id); 0 = header
 * and size only (a ROM hack that keeps its base game's header and size
 * would then be taken for the base game). */
#define FE_SHARE_ID_SAMPLES   4

enum { FE_SHARE_OBJ_MANIFEST = 0, FE_SHARE_OBJ_SAVE, FE_SHARE_OBJ_STATE,
       FE_SHARE_OBJ_THUMB, FE_SHARE_OBJ_SHOT, FE_SHARE_OBJ_KINDS };

typedef struct
{
   uint16_t entry;         /* index into the server's manifest */
   uint8_t  kind;          /* FE_SHARE_OBJ_* */
   uint8_t  slot;          /* 1..5 for state / thumb / shot, else 0 */
} fe_share_obj;

/* Slot flags. */
#define FE_SHARE_ST_THUMB 1u
#define FE_SHARE_ST_SHOT  2u

typedef struct
{
   uint8_t  console;                  /* 0 GBA, 1 GB, 2 GBC */
   uint8_t  id[FE_SHARE_ID_LEN];      /* fe_share_game_id */
   uint32_t save_size;                /* 0 = no save shared */
   uint32_t save_time;                /* fe_share_time_pack, 0 = unknown */
   struct
   {
      uint32_t size;                  /* 0 = empty slot */
      uint32_t time;
      uint8_t  flags;                 /* FE_SHARE_ST_* */
   } st[FE_SHARE_SLOTS];
   char     name[FE_SHARE_NAME_CAP];  /* the sharer's name for it; display */
} fe_share_entry;

/* ---- times --------------------------------------------------------------
 * A file's date as its console stamped it, packed for the manifest and the
 * "9 Oct, 21:05" captions: y(12) m(4) d(5) h(5) mi(6).  0 = unknown. */
uint32_t fe_share_time_pack(int year, int month, int day, int hour, int min);
/* "9 Oct, 21:05" (with_time) or "9 Oct"; "" for 0 / a non-date. */
void fe_share_time_text(uint32_t t, int with_time, char *out, size_t n);

uint32_t fe_share_crc32(uint32_t crc, const void *buf, size_t len);

/* ---- which files are game data ------------------------------------------
 * `name` is a file name (or a path relative to roms/).  Returns 1 and fills
 * the outputs when it is a save, a state or a state's preview:
 *
 *   Game.sav        GBA save      key "Game"      (the ROM is key + ".gba")
 *   Game.gb.sav     GB  save      key "Game.gb"   (the ROM's own name)
 *   Game.gbc.sav    GBC save      key "Game.gbc"
 *   Game.st0..st4   GBA state, slots 1..5 (+ ".thumb", ".shot")
 *   Game.gb.st0 ..  GB / GBC states the same way
 *
 * key_len = how many leading bytes of `name` are the key.  Anything else
 * (ROMs, .bak, .tmp, art, text) returns 0. */
int fe_share_classify(const char *name, int *console, int *kind, int *slot,
                      size_t *key_len);
/* Does ROM file `rom` (same directory level as the data file) own key
 * `key[0..key_len)` on `console`?  Case-insensitive, as FAT is. */
int fe_share_key_matches_rom(const char *key, size_t key_len, int console,
                             const char *rom);

/* ---- game identity -------------------------------------------------------
 * 16 bytes that are the same for the same ROM image on any console and any
 * file name: the ROM's size, a CRC32 of its header block (GBA 0xA0..0xBF:
 * title, game code, maker, version, complement; GB/GBC 0x134..0x14F: title,
 * CGB flag, cartridge type, sizes, version, both checksums), a CRC32 of
 * FE_SHARE_ID_SAMPLES 256-byte samples spread through the image (so a hack
 * built on a game is not that game), and the four code/title characters
 * for logs.  `read` returns the bytes read at `off`.  0 = ok. */
typedef int (*fe_share_read_fn)(void *user, uint32_t off, void *buf,
                                uint32_t len);
int fe_share_game_id(int console, uint32_t rom_size, fe_share_read_fn read,
                     void *user, uint8_t id[FE_SHARE_ID_LEN]);

/* ---- manifest ------------------------------------------------------------
 * 8-byte head ("GSM1", count u16, 0 u16) + count x FE_SHARE_ENTRY_WIRE. */
size_t fe_share_manifest_size(unsigned n);
size_t fe_share_manifest_build(const fe_share_entry *e, unsigned n,
                               uint8_t *out, size_t cap);
/* Validates every field as hostile input (console, sizes, flags on empty
 * slots, the name made printable and NUL-ended).  Returns the entry count,
 * or -1 when the blob is not a manifest; at most `max` are written. */
int fe_share_manifest_parse(const uint8_t *buf, size_t len,
                            fe_share_entry *out, unsigned max);
/* The entry of `console` with identity `id`, or -1. */
int fe_share_match(const fe_share_entry *e, unsigned n, int console,
                   const uint8_t id[FE_SHARE_ID_LEN]);
/* Is `o` something entry table `e` really offers? */
int fe_share_obj_valid(const fe_share_entry *e, unsigned n, fe_share_obj o);

/* ---- who we are -----------------------------------------------------------
 * Sent in HELLO and WELCOME.  `build` names the build (the core's version
 * string); state_fmt[console] names the state format of each core.  States
 * cross only when fe_share_states_ok() says so; saves always do. */
typedef struct
{
   char     build[FE_SHARE_BUILD_CAP];
   uint32_t state_fmt[FE_SHARE_CONSOLES];
   uint8_t  mac[6];
} fe_share_ident;

int fe_share_states_ok(const fe_share_ident *a, const fe_share_ident *b,
                       int console);

/* ---- is this file a state of this build? ----------------------------------
 * fe_share_states_ok compares what the two builds SAY.  This looks at the
 * file that arrived (review finding 4): its size and its first bytes must
 * be those of `console`'s state format, or it is not put in a slot -- a
 * slot holding something the game cannot load reads as "No state to load"
 * at best, and it replaced a state that worked.
 *
 *   GBA: exactly FE_SHARE_GBA_STATE_SIZE bytes (retro_serialize_size is a
 *        constant); the BSON document length, "info-magic" and
 *        "info-version" == `state_fmt` (savestate.c gba_save_state writes
 *        them first, in that order).
 *   GB / GBC: gbcore_state_save's 40-byte header: "GBAT", version 1, the
 *        console byte, and a core length that accounts for the file's size.
 *
 * `head` is the file's first min(size, FE_SHARE_STATE_HEAD) bytes.
 * fe_share_state_size_ok alone can be asked before a byte is fetched. */
#define FE_SHARE_STATE_HEAD      40u
#define FE_SHARE_GBA_STATE_SIZE  (416u * 1024u)   /* GBA_STATE_MEM_SIZE */
#define FE_SHARE_GBA_STATE_MAGIC 0x06BAC0DEu      /* GBA_STATE_MAGIC */
int fe_share_state_size_ok(int console, uint32_t size);
int fe_share_state_check(int console, uint32_t state_fmt,
                         const uint8_t *head, size_t head_len,
                         uint32_t size);

/* ---- errors --------------------------------------------------------------- */
enum
{
   FE_SHARE_OK = 0,
   FE_SHARE_E_LOST,        /* the other PSP stopped answering */
   FE_SHARE_E_REFUSED,     /* protocol generations differ: update both */
   FE_SHARE_E_BUSY,        /* the sharer already has a guest */
   FE_SHARE_E_GONE,        /* the sharer no longer has that file */
   FE_SHARE_E_TOO_BIG,     /* larger than FE_SHARE_MAX_OBJECT */
   FE_SHARE_E_CHANGED,     /* whole-file checksum mismatch after transfer */
   FE_SHARE_E_WRITE,       /* could not write here (stick full?) */
   FE_SHARE_E_BAD,         /* a malformed answer */
   FE_SHARE_E_CLOSED,      /* the sharer stopped sharing */
   FE_SHARE_E_FORMAT       /* not a state this build can load */
};
const char *fe_share_err_name(int e);

/* ====================== the server (the sharing PSP) ====================== */

typedef struct
{
   void *user;
   /* One datagram to `mac`.  Unreliable; the return value is ignored. */
   void (*send)(void *user, const uint8_t mac[6], const void *buf,
                size_t len);
   /* Open entry/kind/slot's file READ-ONLY and report its size; one file
    * is open at a time (close comes first).  The platform maps the triple
    * to a path through its own table.  0 = ok. */
   int  (*open)(void *user, fe_share_obj o, uint32_t *size);
   int  (*read)(void *user, uint32_t off, void *buf, uint32_t len);
   void (*close)(void *user);
} fe_share_server_io;

enum { FE_SHARE_SRV_WAITING = 0, FE_SHARE_SRV_CONNECTED };

typedef struct
{
   fe_share_server_io io;
   fe_share_ident me;
   const fe_share_entry *entries;
   unsigned n;
   const uint8_t *manifest;
   uint32_t manifest_size, manifest_crc;
   unsigned n_saves, n_states;

   int      state;
   uint8_t  peer[6];
   fe_share_ident peer_ident;
   uint32_t session, session_seed, peer_nonce;
   /* One bounded provisional guest. HELLO cannot replace the active guest. */
   fe_share_ident offer_ident;
   uint32_t offer_nonce, offer_session, offer_ms;
   uint32_t last_rx_ms;

   /* the open object */
   int      have_obj;
   fe_share_obj obj;
   uint32_t obj_size, obj_crc;
   /* the pending window */
   uint16_t req;
   int have_req;                 /* request watermark within this session */
   uint32_t win_first, win_mask;
   /* what the waiting screen shows */
   uint32_t sent_chunks;          /* of the open object, counted once each */
   uint32_t hi_chunk;
   /* counters (also the tests' evidence) */
   uint32_t rx_ok, rx_bad, rx_refused, tx_data, objects_served;
} fe_share_server;

/* `manifest` is the built blob (fe_share_manifest_build) and `entries` the
 * table it was built from; both stay the caller's and must outlive `s`. */
void fe_share_server_init(fe_share_server *s, const fe_share_server_io *io,
                          const fe_share_ident *me,
                          const fe_share_entry *entries, unsigned n,
                          const uint8_t *manifest, uint32_t manifest_size,
                          uint32_t session_seed);
void fe_share_server_rx(fe_share_server *s, const void *buf, size_t len,
                        uint32_t now_ms);
/* Send up to `budget` DATA datagrams of the pending window and run the
 * guest timeout.  Call once per frame. */
void fe_share_server_step(fe_share_server *s, uint32_t now_ms,
                          unsigned budget);
/* Stop: tell the guest (BYE) and close the open file. */
void fe_share_server_stop(fe_share_server *s);

/* ====================== the client (the joining PSP) ====================== */

typedef struct
{
   void *user;
   void (*send)(void *user, const uint8_t mac[6], const void *buf,
                size_t len);
} fe_share_client_io;

/* Where a fetch lands.  write() may be called for any chunk in any order,
 * each exactly FE_SHARE_CHUNK bytes except the last; read() is the
 * verification pass over what was written.  A memory buffer or a temp
 * file -- never the final file. */
typedef struct
{
   void *user;
   int (*write)(void *user, uint32_t off, const void *buf, uint32_t len);
   int (*read)(void *user, uint32_t off, void *buf, uint32_t len);
   uint32_t cap;           /* refuse anything larger (0 = MAX_OBJECT) */
} fe_share_sink;

enum { FE_SHARE_CL_IDLE = 0, FE_SHARE_CL_HELLO, FE_SHARE_CL_READY,
       FE_SHARE_CL_STAT, FE_SHARE_CL_DATA, FE_SHARE_CL_DONE,
       FE_SHARE_CL_ERROR };

typedef struct
{
   fe_share_client_io io;
   fe_share_ident me, host;
   int      state, err;
   uint32_t session, nonce, offer_session;
   int hello_started;
   uint32_t t_start, t_last_rx, t_last_tx, t_last_data;
   /* from WELCOME */
   unsigned n_entries, n_saves, n_states;
   uint32_t manifest_size, manifest_crc;
   int      refused_proto;

   /* the fetch */
   fe_share_obj obj;
   fe_share_sink sink;
   uint32_t expect;        /* fe_share_client_expect; 0 = any size */
   int      exp_state;     /* fe_share_client_expect_state: console + 1 */
   uint32_t exp_fmt;
   uint16_t req;
   uint32_t size, crc, nchunks, got;
   uint32_t win_first, win_mask;      /* asked and still missing */
   uint8_t  have[FE_SHARE_MAX_CHUNKS / 8];
   /* counters */
   uint32_t rx_ok, rx_bad, rx_dup, tx_get, retries;
} fe_share_client;

/* Timers (ms). */
#define FE_SHARE_HELLO_EVERY   250u
#define FE_SHARE_HELLO_GIVEUP  12000u
#define FE_SHARE_PING_EVERY    1000u
#define FE_SHARE_LOST_AFTER    6000u
#define FE_SHARE_RETRY_AFTER   250u

void fe_share_client_init(fe_share_client *c, const fe_share_client_io *io,
                          const fe_share_ident *me, uint32_t nonce);
/* Start looking for the room's sharer (broadcast HELLO until WELCOME). */
void fe_share_client_hello(fe_share_client *c, uint32_t now_ms);
/* Start fetching `o` into `sink`.  Needs state READY or DONE/ERROR of a
 * previous fetch with the link still up.  0 = started. */
int  fe_share_client_fetch(fe_share_client *c, fe_share_obj o,
                           const fe_share_sink *sink, uint32_t now_ms);
/* Drop the fetch in flight (late answers to it are ignored). */
/* The size the object being fetched MUST have (the manifest entry's): call
 * right after a successful fe_share_client_fetch.  The sharer's own answer
 * is otherwise believed, and a buggy or hostile one could hand over an empty
 * or a different file that then passes "its" checksum and replaces the
 * player's save.  A mismatch (an empty file included) fails the fetch with
 * FE_SHARE_E_CHANGED before a byte is written.  0 = no expectation. */
void fe_share_client_expect(fe_share_client *c, uint32_t size);
/* The object being fetched is a save STATE of `console` in format
 * `state_fmt` (this build's fe_share_ident.state_fmt[console]): call right
 * after a successful fe_share_client_fetch.  A size no such state has fails
 * with FE_SHARE_E_FORMAT before a byte is written; a head that is not this
 * build's fails it after the checksum, so the fetch never reaches DONE. */
void fe_share_client_expect_state(fe_share_client *c, int console,
                                  uint32_t state_fmt);
void fe_share_client_cancel(fe_share_client *c);
void fe_share_client_rx(fe_share_client *c, const void *buf, size_t len,
                        uint32_t now_ms);
void fe_share_client_step(fe_share_client *c, uint32_t now_ms);
/* Leave: tell the sharer (BYE). */
void fe_share_client_bye(fe_share_client *c);
/* 1 while the sharer is answering (any state past HELLO, not LOST etc.). */
int  fe_share_client_linked(const fe_share_client *c);

/* ====================== commit: temp -> final ============================= */

typedef struct
{
   void *user;
   int (*exists)(void *user, const char *path);
   int (*rename)(void *user, const char *from, const char *to);  /* 0 ok */
   int (*remove)(void *user, const char *path);
} fe_share_fs;

/* Put the verified temp file `tmp` in place as `final`.
 *
 *   keep_bak = 1 (a save): an existing `final` becomes `final`.bak --
 *     exactly ONE backup per game: the previous .bak is replaced -- and
 *     stays there.
 *   keep_bak = 0 (a state or one of its previews): an existing `final` is
 *     moved aside, and removed once the new one is in place.
 *
 * If the temp cannot be moved in, the old file is moved back, so `final`
 * is either the complete new file or exactly what it was.  0 = done.
 * `scratch` holds one path (cap bytes; `final` + 5 must fit). */
/* fe_share.c: mend an interrupted commit (final missing, final.bak/.old
 * present) and remove stray temp/.old files.  1 = final was restored. */
int fe_share_recover(const fe_share_fs *fs, const char *final,
                     const char *tmp_suffix, char *scratch, size_t cap);
/* 0 ok; -1 failed, final untouched; -2 failed AND the old file could not
 * be put back: it is final.bak / final.old (fe_share_recover mends it). */
int fe_share_commit(const fe_share_fs *fs, const char *final,
                    const char *tmp, int keep_bak, char *scratch,
                    size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* FE_SHARE_H */
