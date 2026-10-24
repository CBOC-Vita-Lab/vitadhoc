/* wide.c -- the wide picture: more of the game than the console's 240 pixels.
 *
 * A Vita's screen is 16:9; a GBA picture scaled to its height leaves a
 * sixth of it black.  video.cc can render extra pixels left and right of
 * the 240 (see "THE WIDE PICTURE" there); this file decides WHAT may be
 * drawn there, because that depends on the game:
 *
 *   - a rotated/scaled background ("Mode 7": the racers' tracks) is a
 *     formula, it simply goes on past the edge.  No profile needed: the
 *     default policy in video.cc extends modes 1 and 2.
 *
 *   - a tile background is a 256 pixel ring the game redraws at the edges
 *     as it scrolls; past the screen's edge there is only the ring's other
 *     side.  To show more, somebody has to know what the map holds out
 *     there.  A PROFILE is that knowledge for one game: it reads the game's
 *     own map from its memory and hands the renderer the tiles the game
 *     would have drawn.
 *
 * The first profile is Pokemon Emerald (USA/Europe): the overworld, and the
 * battle screen.  The addresses and structures are from the
 * pret/pokeemerald decompilation (symbols branch, src/fieldmap.c,
 * src/field_camera.c); the same engine runs Ruby, Sapphire, FireRed and
 * LeafGreen, which are in the table from their own decompilations but
 * could not be run here.  A profile only READS the game's memory.  The cartridge, the save and everything the game
 * computes are untouched, so link play, cheats and states work as before,
 * and the 240 pixels in the middle are bit for bit what they were.
 *
 * How it stays exact while the game runs ahead of the picture: the picture
 * on screen was set up by the game's vertical-blank handler, and the game
 * then computes the NEXT frame while this one is being drawn.  So at the
 * start of vertical blank -- the game has finished a frame's logic and its
 * handler has not run yet -- the profile takes a snapshot of the map around
 * the camera (em_latch).  The margins of the following picture are drawn
 * from that snapshot and from the scroll registers of the line itself.
 * Before the first line the snapshot is checked against the 240 pixels the
 * game did draw (em_verify): if the model does not explain what is on
 * screen, the margins of that frame stay black rather than show a guess.
 */
#include "common.h"
#include "wide.h"

#define WIDE_MARGIN_PX 21         /* (240 + 42) x 160 = 16:9.02 */

static int  wide_want;            /* the front end's switch: WIDE_* */
static int  wide_profile;         /* WIDE_PROFILE_* of the loaded game */
static int  wide_profiles_on = 1; /* (tests: 0 = treat every game as unknown) */
static u16 *wide_buf;

/* What the tests and the log ask for. */
static wide_stats_t wide_st;

/* A host twin with GBADHOC_WIDE_DEBUG=1 says on stderr which tiles a
 * profile got wrong. */
#ifndef WIDE_DEBUG
#ifdef VITA
#define WIDE_DEBUG 0
#else
#define WIDE_DEBUG 1
#endif
#endif
#if WIDE_DEBUG
static int wide_dbg;
static unsigned wide_dbg_frame;
static int wide_dbg_dump;         /* GBADHOC_WIDE_DUMP=n: the automatic mode's
                                   * margin tiles of its n-th frame */
static int wide_dbg_mosaic = -1;  /* GBADHOC_WIDE_MOSAIC=0x33: the mosaic
                                   * register is held at this value (a game
                                   * that only blurs on its way into a
                                   * battle is checked while it walks) */
#endif

/* The game's memory, read without side effects.  RAM and the cartridge
 * pages that are in memory are read where they lie (a snapshot is some
 * thousands of reads a frame); a cartridge page that is not goes through
 * the core's own read, which fetches it. */
static inline const u8 *rd_page(u32 a)
{
   return a < 0x10000000u ? memory_map_read[a >> 15] : NULL;
}
static inline u32 rd8(u32 a)
{
   const u8 *page = rd_page(a);
   return page ? page[a & 0x7FFF] : (read_memory8(a) & 0xFF);
}
static inline u32 rd16(u32 a)
{
   const u8 *page = rd_page(a);
   return page ? (u32)readaddress16(page, a & 0x7FFE)
               : (read_memory16(a) & 0xFFFF);
}
static inline u32 rd32(u32 a)
{
   const u8 *page = rd_page(a);
   return page ? (u32)readaddress32(page, a & 0x7FFC) : read_memory32(a);
}

/* ======================================================================== */
/* Pokemon Emerald (BPEE, revision 0)                                        */
/* ======================================================================== */

/* The five games share one engine (the overworld of Ruby and Sapphire,
 * carried into FireRed, LeafGreen and Emerald), and the pret
 * decompilations name the same things in each: addresses below are from
 * each project's symbols branch.  Two things differ in FireRed/LeafGreen
 * (G3_FRLG): a metatile's attributes are 32 bits with the layer type in
 * bits 29-30 and sit after the tileset's callback, there are 640 primary
 * metatiles, and a map's border is borderWidth x borderHeight blocks.
 *
 * Only Emerald could be RUN here (the owner's cartridge); its three
 * routines are fingerprinted.  The others are matched by cartridge code and
 * revision, untested -- which is safe only because of the check before
 * line 0: a profile whose reading of the map does not match the picture
 * shows nothing. */
enum { G3_RSE = 0, G3_FRLG };

typedef struct g3_game
{
   u32 code;                      /* the cartridge's game code, as a word  */
   u8  rev, kind;
   const char *name;
   u32 gMain;                     /* +4: callback2                          */
   u32 cb2_overworld, cb2_basic;  /* (basic: while a battle's opening wipe
                                   * runs over the map)                     */
   u32 cb2_battle;
   u32 cam;                       /* sFieldCameraOffset: xPix yPix xTile yTile */
   u32 sb1ptr, sb1;               /* gSaveBlock1Ptr, or gSaveBlock1 itself:
                                   * pos.x pos.y (s16) at its start         */
   u32 backup;                    /* gBackupMapLayout / VMap: w h map*      */
   u32 header;                    /* gMapHeader: +0 layout* +0xC connections* */
   u32 groups;                    /* gMapGroups                             */
} g3_game;

#define G3_CODE(a, b, c, d) ((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))

static const g3_game g3_games[] = {
   { G3_CODE('B','P','E','E'), 0, G3_RSE, "Pokemon Emerald",
     0x030022c0u, 0x08085e5du, 0x08085e51u, 0x08038421u, 0x03000e20u,
     0x03005d8cu, 0, 0x03005dc0u, 0x02037318u, 0x08486578u },
   { G3_CODE('A','X','V','E'), 0, G3_RSE, "Pokemon Ruby",
     0x03001770u, 0x080543a5u, 0x08054399u, 0x0800f809u, 0x03000590u,
     0, 0x02025734u, 0x03004870u, 0x0202e828u, 0x08308588u },
   { G3_CODE('A','X','V','E'), 1, G3_RSE, "Pokemon Ruby (1.1)",
     0x03001770u, 0x080543c5u, 0x080543b9u, 0x0800f809u, 0x03000590u,
     0, 0x02025734u, 0x03004870u, 0x0202e828u, 0x083085a0u },
   { G3_CODE('A','X','V','E'), 2, G3_RSE, "Pokemon Ruby (1.2)",
     0x03001770u, 0x080543c5u, 0x080543b9u, 0x0800f809u, 0x03000590u,
     0, 0x02025734u, 0x03004870u, 0x0202e828u, 0x083085a0u },
   { G3_CODE('A','X','P','E'), 0, G3_RSE, "Pokemon Sapphire",
     0x03001770u, 0x080543a9u, 0x0805439du, 0x0800f809u, 0x03000590u,
     0, 0x02025734u, 0x03004870u, 0x0202e828u, 0x08308518u },
   { G3_CODE('A','X','P','E'), 1, G3_RSE, "Pokemon Sapphire (1.1)",
     0x03001770u, 0x080543c9u, 0x080543bdu, 0x0800f809u, 0x03000590u,
     0, 0x02025734u, 0x03004870u, 0x0202e828u, 0x08308530u },
   { G3_CODE('A','X','P','E'), 2, G3_RSE, "Pokemon Sapphire (1.2)",
     0x03001770u, 0x080543c9u, 0x080543bdu, 0x0800f809u, 0x03000590u,
     0, 0x02025734u, 0x03004870u, 0x0202e828u, 0x08308530u },
   { G3_CODE('B','P','R','E'), 0, G3_FRLG, "Pokemon FireRed",
     0x030030f0u, 0x080565b5u, 0x080565a9u, 0x08011101u, 0x03000e90u,
     0x03005008u, 0, 0x03005040u, 0x02036dfcu, 0x083526a8u },
   { G3_CODE('B','P','R','E'), 1, G3_FRLG, "Pokemon FireRed (1.1)",
     0x030030f0u, 0x080565c9u, 0x080565bdu, 0x08011115u, 0x03000e90u,
     0x03005008u, 0, 0x03005040u, 0x02036dfcu, 0x08352718u },
   { G3_CODE('B','P','G','E'), 0, G3_FRLG, "Pokemon LeafGreen",
     0x030030f0u, 0x080565b5u, 0x080565a9u, 0x08011101u, 0x03000e90u,
     0x03005008u, 0, 0x03005040u, 0x02036dfcu, 0x08352688u },
   { G3_CODE('B','P','G','E'), 1, G3_FRLG, "Pokemon LeafGreen (1.1)",
     0x030030f0u, 0x080565c9u, 0x080565bdu, 0x08011115u, 0x03000e90u,
     0x03005008u, 0, 0x03005040u, 0x02036dfcu, 0x083526f8u },
};
static const g3_game *g3;         /* the loaded game's, or NULL */
static int au_line(u32 vcount, int side, s32 xo, wide_line_t *out);
static int em_line(u32 vcount, int side, s32 xo, wide_line_t *out);

#define EM_MAP_OFFSET          7
#define EM_BLOCK_UNDEFINED     0x3FF

/* The snapshot: metatiles EM_CW x EM_CH around the camera, as the three
 * layers' map entries (4 per metatile and layer). */
#define EM_CW 26
#define EM_CH 16
#define EM_CX0 (-5)               /* first column, relative to pos.x */
#define EM_CY0 (-2)

typedef struct em_snap
{
   int ok;
   s32 pos_x, pos_y;              /* camera, in metatiles of the map grid */
   u32 xt, yt;                    /* the ring's tile offsets */
   u16 t[EM_CH][EM_CW][3][4];     /* [row][col][bg 1..3][quarter] */
} em_snap;

/* Two snapshots: the one just taken and the last one a picture bore out.
 * When the game takes longer than a frame (it loads the next map as the
 * player walks across a border) the picture on screen is still the earlier
 * one, and so is the snapshot that explains it. */
static struct
{
   em_snap s[2];
   int latest;                    /* index of the snapshot just taken */
   int good;                      /* ...of the last one borne out, or -1 */
   const em_snap *use;            /* this frame's, NULL: margins black */
   u16 rows[2][4][32];            /* the rows handed to the renderer */
   /* The map layers' rows, by pass (0: a margin; 1: the renderer's check,
    * which draws strips of the picture itself), side and layer, and what
    * each was made from (em_line): first tile, tile row, ring column;
    * rkey_ok: made this frame. */
   u16 mrows[2][2][4][32];
   s32 rkey[2][2][4][3];
   int rkey_ok[2][2][4];
   int battle;                    /* the battle screen is up */
   u32 battle_layers;
   int wipe;                      /* a battle's opening wipe runs over the map */
} em;

/* The map as the game's MapGridGetMetatileIdAt sees it, and past the seven
 * columns of neighbour the game keeps: the neighbour itself. */
typedef struct em_map
{
   s32 bw, bh; u32 bmap;          /* gBackupMapLayout */
   u32 layout, border;
   s32 border_w, border_h;        /* (FireRed/LeafGreen) */
   s32 mw, mh;                    /* the map proper */
   u32 prim_meta, sec_meta, prim_attr, sec_attr;
   u32 conn_n, conn;
} em_map;

static u32 em_border_block(const em_map *m, s32 x, s32 y)
{
   u32 i;
   if (g3->kind == G3_FRLG)
   {
      s32 xp = (x - EM_MAP_OFFSET + 8 * m->border_w) % m->border_w;
      s32 yp = (y - EM_MAP_OFFSET + 8 * m->border_h) % m->border_h;
      if (xp < 0) xp += m->border_w;
      if (yp < 0) yp += m->border_h;
      return rd16(m->border + (u32)(xp + yp * m->border_w) * 2);
   }
   i = (u32)((x + 1) & 1) + (u32)((y + 1) & 1) * 2;
   return rd16(m->border + i * 2);
}

/* A block of a connected map at (x, y) in the map's own coordinates
 * (0,0 = its top left, so outside 0..mw-1 / 0..mh-1 here), or -1. */
static s32 em_neighbour_block(const em_map *m, s32 x, s32 y)
{
   u32 i;
   for (i = 0; i < m->conn_n && i < 8; i++)
   {
      u32 c = m->conn + i * 12;
      u32 dir = rd8(c);
      s32 off = (s32)rd32(c + 4);
      u32 grp = rd8(c + 8), num = rd8(c + 9);
      u32 hdr, lay, cmap;
      s32 cw, ch, cx, cy;

      /* 1 south, 2 north, 3 west, 4 east */
      if (dir == 3 && x >= 0) continue;
      if (dir == 4 && x < m->mw) continue;
      if (dir == 2 && y >= 0) continue;
      if (dir == 1 && y < m->mh) continue;
      if (dir < 1 || dir > 4) continue;

      /* gMapGroups[grp][num] -> header -> layout */
      hdr = rd32(g3->groups + grp * 4);
      if ((hdr >> 24) != 0x08) continue;
      hdr = rd32(hdr + num * 4);
      if ((hdr >> 24) != 0x08) continue;
      lay = rd32(hdr);
      if ((lay >> 24) != 0x08) continue;
      cw = (s32)rd32(lay); ch = (s32)rd32(lay + 4); cmap = rd32(lay + 0xC);
      if (cw <= 0 || ch <= 0 || cw > 512 || ch > 512 || (cmap >> 24) != 0x08)
         continue;

      switch (dir)
      {
      case 3:  cx = x + cw;     cy = y - off; break;
      case 4:  cx = x - m->mw;  cy = y - off; break;
      case 2:  cx = x - off;    cy = y + ch;  break;
      default: cx = x - off;    cy = y - m->mh; break;
      }
      if (cx < 0 || cx >= cw || cy < 0 || cy >= ch)
         continue;
      return (s32)rd16(cmap + (u32)(cx + cw * cy) * 2);
   }
   return -1;
}

static u32 em_metatile_at(const em_map *m, s32 x, s32 y)
{
   u32 block;
   if (x >= 0 && x < m->bw && y >= 0 && y < m->bh)
      block = rd16(m->bmap + (u32)(x + m->bw * y) * 2);
   else
   {
      s32 n = em_neighbour_block(m, x - EM_MAP_OFFSET, y - EM_MAP_OFFSET);
      block = n >= 0 ? (u32)n : em_border_block(m, x, y);
   }
   if ((block & 0x3FF) == EM_BLOCK_UNDEFINED)
      block = em_border_block(m, x, y);
   return block & 0x3FF;
}

/* DrawMetatileAt / DrawMetatile of field_camera.c, into the snapshot. */
static void em_metatile_tiles(const em_map *m, s32 x, s32 y, u16 out[3][4])
{
   const u32 primary = g3->kind == G3_FRLG ? 640 : 512;
   u32 id = em_metatile_at(m, x, y);
   u32 attr, tiles, type, n = id < primary ? id : id - primary;
   u16 t[8];
   int i;

   attr = id < primary ? m->prim_attr : m->sec_attr;
   tiles = (id < primary ? m->prim_meta : m->sec_meta) + n * 16;
   type = g3->kind == G3_FRLG ? (rd32(attr + n * 4) >> 29) & 3
                              : (rd16(attr + n * 2) >> 12) & 0xF;
   for (i = 0; i < 8; i++)
      t[i] = (u16)rd16(tiles + (u32)i * 2);

   /* out[0] = BG1 (top), out[1] = BG2 (middle), out[2] = BG3 (bottom) */
   for (i = 0; i < 4; i++)
   {
      switch (type)
      {
      case 2:                     /* split: bottom layer and top layer */
         out[2][i] = t[i]; out[1][i] = 0;        out[0][i] = t[4 + i]; break;
      case 1:                     /* covered: the two lower layers */
         out[2][i] = t[i]; out[1][i] = t[4 + i]; out[0][i] = 0;        break;
      default:                    /* normal: the two upper layers */
         out[2][i] = 0x3014; out[1][i] = t[i];   out[0][i] = t[4 + i]; break;
      }
   }
}

/* Vertical blank begins: the game's logic for the coming picture is done. */
static void em_latch(void)
{
   em_map m;
   em_snap *sn;
   const u32 attr_at = g3->kind == G3_FRLG ? 0x14 : 0x10;
   u32 sb1, cam = g3->cam, prim, sec, conns, cb2 = rd32(g3->gMain + 4);
   s32 r, c;

   em.latest = em.good == 0 ? 1 : 0;
   sn = &em.s[em.latest];
   sn->ok = 0;
   em.use = NULL;
   em.battle = cb2 == g3->cb2_battle;
   em.wipe = cb2 == g3->cb2_basic;
   if (cb2 != g3->cb2_overworld && cb2 != g3->cb2_basic)
   {
      em.good = -1;
      return;
   }
   sb1 = g3->sb1ptr ? rd32(g3->sb1ptr) : g3->sb1;
   if ((sb1 >> 24) != 0x02)
      return;

   m.bw = (s32)rd32(g3->backup);
   m.bh = (s32)rd32(g3->backup + 4);
   m.bmap = rd32(g3->backup + 8);
   m.layout = rd32(g3->header);
   if (m.bw < 15 || m.bh < 14 || m.bw * m.bh > 10240 ||
       (m.bmap >> 24) != 0x02 || (m.layout >> 24) != 0x08)
      return;
   m.mw = (s32)rd32(m.layout);
   m.mh = (s32)rd32(m.layout + 4);
   m.border = rd32(m.layout + 8);
   prim = rd32(m.layout + 0x10);
   sec = rd32(m.layout + 0x14);
   if ((m.border >> 24) != 0x08 || (prim >> 24) != 0x08 || (sec >> 24) != 0x08 ||
       m.mw + 15 != m.bw || m.mh + 14 != m.bh)
      return;
   m.border_w = m.border_h = 2;
   if (g3->kind == G3_FRLG)
   {
      m.border_w = (s32)rd8(m.layout + 0x18);
      m.border_h = (s32)rd8(m.layout + 0x19);
      if (m.border_w < 1 || m.border_h < 1 || m.border_w > 16 || m.border_h > 16)
         return;
   }
   m.prim_meta = rd32(prim + 0xC); m.prim_attr = rd32(prim + attr_at);
   m.sec_meta = rd32(sec + 0xC);   m.sec_attr = rd32(sec + attr_at);
   if ((m.prim_meta >> 24) != 0x08 || (m.prim_attr >> 24) != 0x08 ||
       (m.sec_meta >> 24) != 0x08 || (m.sec_attr >> 24) != 0x08)
      return;
   conns = rd32(g3->header + 0xC);
   m.conn_n = 0; m.conn = 0;
   if ((conns >> 24) == 0x08)
   {
      m.conn_n = rd32(conns);
      m.conn = rd32(conns + 4);
      if (m.conn_n > 8 || (m.conn >> 24) != 0x08)
         m.conn_n = 0;
   }

   sn->pos_x = (s16)rd16(sb1);
   sn->pos_y = (s16)rd16(sb1 + 2);
   sn->xt = rd8(cam + 2);
   sn->yt = rd8(cam + 3);

   for (r = 0; r < EM_CH; r++)
      for (c = 0; c < EM_CW; c++)
         em_metatile_tiles(&m, sn->pos_x + EM_CX0 + c, sn->pos_y + EM_CY0 + r,
                           sn->t[r][c]);
   sn->ok = 1;
   wide_st.latched++;
}

/* The map entry of layer `bg` (1..3) for the 8x8 tile at world tile
 * (wtx, wty); world pixel 0 is the map grid's corner.  -1: not in the
 * snapshot. */
static inline s32 em_entry(const em_snap *sn, u32 bg, s32 wtx, s32 wty)
{
   s32 c = (wtx >> 1) - (sn->pos_x + EM_CX0);
   s32 r = (wty >> 1) - (sn->pos_y + EM_CY0);
   if (c < 0 || c >= EM_CW || r < 0 || r >= EM_CH)
      return -1;
   return sn->t[r][c][bg - 1][(wtx & 1) + ((wty & 1) << 1)];
}

/* World pixel of screen (0, vcount) for layer `bg`, from the line's own
 * scroll registers.  The ring's origin (tile xt,yt) is metatile pos. */
static inline void em_world(const em_snap *sn, u32 bg, u32 vcount,
                            s32 *wx0, s32 *wy)
{
   u32 hofs = read_ioreg(REG_BGxHOFS(bg)) & 0xFF;
   u32 vofs = read_ioreg(REG_BGxVOFS(bg)) & 0xFF;
   *wx0 = sn->pos_x * 16 + (s8)(u8)(hofs - sn->xt * 8);
   *wy  = sn->pos_y * 16 + (s8)(u8)(vofs - sn->yt * 8) + (s32)vcount;
}

/* The three map layers are 256x256 tile maps, 16 colours a tile. */
static inline int em_bg_plain(u32 bg)
{
   return (read_ioreg(REG_BGxCNT(bg)) & 0xC080) == 0;
}

/* The line a layer is drawn from: the game leaves the three layers' mosaic
 * bit set, and blurs the map with it on the way into a battle. */
static inline u32 em_mosaic_line(u32 bg, u32 vcount)
{
   u32 mos = read_ioreg(REG_MOSAIC);
   if ((read_ioreg(REG_BGxCNT(bg)) & 0x40) && (mos & 0xFF))
      vcount -= vcount % (((mos >> 4) & 0xF) + 1);
   return vcount;
}

/* Does the snapshot explain the picture the game drew?  Every tile of the
 * three layers that is on screen is compared with what the snapshot says is
 * there; returns how many differ (or a large number: not these layers). */
static u32 em_compare(const em_snap *sn, u32 *checked)
{
   u32 bg, bad = 0, n = 0;

   if (!sn->ok || (read_ioreg(REG_DISPCNT) & 7) != 0)
      return 0xFFFF;
   for (bg = 1; bg <= 3; bg++)
   {
      u32 cnt = read_ioreg(REG_BGxCNT(bg));
      const u16 *map = (const u16 *)&vram[((cnt >> 8) & 0x1F) * 2048];
      u32 hofs = read_ioreg(REG_BGxHOFS(bg)) & 0xFF;
      u32 vofs = read_ioreg(REG_BGxVOFS(bg)) & 0xFF;
      s32 wx0, wy0, tx, ty;

      if (!em_bg_plain(bg))
         return 0xFFFF;
      em_world(sn, bg, 0, &wx0, &wy0);
      /* (the 31st column and the 21st row only while a part of them is on
       * screen: the game does not keep the ring's far side current) */
      for (ty = 0; ty < ((vofs & 7) ? 21 : 20); ty++)
         for (tx = 0; tx < ((hofs & 7) ? 31 : 30); tx++)
         {
            u32 px = hofs + (u32)tx * 8, py = vofs + (u32)ty * 8;
            s32 e = em_entry(sn, bg, (wx0 + tx * 8) >> 3, (wy0 + ty * 8) >> 3);
            u16 v = eswap16(map[((py >> 3) & 31) * 32 + ((px >> 3) & 31)]);
            n++;
            if (e < 0 || (u16)e != v)
            {
               bad++;
#if WIDE_DEBUG
               if (wide_dbg && bad <= 3)
                  fprintf(stderr, "wide dbg frame %u bg%u tile %d,%d world %d,%d "
                          "want %04x vram %04x pos %d,%d xt %u yt %u hofs %u vofs %u\n",
                          wide_dbg_frame, bg, (int)tx, (int)ty,
                          (int)((wx0 + tx * 8) >> 3), (int)((wy0 + ty * 8) >> 3),
                          (unsigned)(e & 0xFFFF), v, (int)sn->pos_x, (int)sn->pos_y,
                          sn->xt, sn->yt, hofs, vofs);
#endif
            }
         }
   }
#if WIDE_DEBUG
   if (wide_dbg && bad)
      fprintf(stderr, "wide dbg frame %u mismatched %u of %u\n", wide_dbg_frame, bad, n);
#endif
   *checked = n;
   return bad;
}

/* A door swinging open is drawn past the map (8 tiles, 3 layers); more than
 * that and the layers are showing something else. */
#define EM_TOLERATED 48

/* Before line 0: which snapshot is this picture's? */
static void em_verify(void)
{
   u32 n = 0, bad = em_compare(&em.s[em.latest], &n);

#if WIDE_DEBUG
   wide_dbg_frame++;
#endif
   em.use = NULL;
   if (bad <= EM_TOLERATED)
   {
      em.good = em.latest;
      em.use = &em.s[em.latest];
   }
   else if (em.good >= 0 && em.good != em.latest)
   {
      /* The game was not done with its frame at vertical blank: is the
       * picture still the one before? */
      u32 n2 = 0, bad2 = em_compare(&em.s[em.good], &n2);
      if (bad2 <= EM_TOLERATED)
      {
         em.use = &em.s[em.good];
         wide_st.held++;
         bad = bad2; n = n2;
      }
   }
   if (bad == 0xFFFF)
      bad = 0;                    /* not the map layers at all */
   wide_st.checked += n;
   wide_st.mismatched += bad;
   if (bad > wide_st.worst)
      wide_st.worst = bad;
   if (!em.use)
   {
      wide_st.refused++;
      if (em.good == em.latest)
         em.good = -1;
   }
}

static int em_line(u32 vcount, int side, s32 xo, wide_line_t *out)
{
   u32 bg;

   const em_snap *sn;

   if (em.battle)
   {
      /* The battle screen: the ground is a 256 pixel picture that repeats,
       * and the Pokemon and trainers are sprites that come in from beyond
       * the edges.  The text box and what else the three front layers
       * carry is the console's 240 pixels wide and stays there. */
      if ((read_ioreg(REG_DISPCNT) & 7) != 0)
         return 0;
      /* (the last 48 lines are the text box: nothing of the scene is
       * beside it, and it is drawn across the whole width -- unless the
       * owner wants bars where there is no more to show) */
      if (vcount >= 112)
         return wide_fill == WIDE_FILL_BARS ? WIDE_LINE_NONE : WIDE_LINE_STRETCH;
      out->layers = em.battle_layers;
      return 1;
   }
   if (vcount == 0 && side == 0)
      memset(em.rkey_ok, 0, sizeof(em.rkey_ok));  /* a new frame's rows */
   if (vcount == 0 && side == 0 && xo < 0 && em.s[em.latest].ok)
      em_verify();
   sn = em.use;
   if (!sn || (read_ioreg(REG_DISPCNT) & 7) != 0)
      return 0;

   out->layers = 0x1E;            /* the three map layers and the sprites */
   const int pass = xo >= 0 && xo < 240;
   for (bg = 1; bg <= 3; bg++)
   {
      u16 *row = em.mrows[pass][side][bg];
      s32 *key = em.rkey[pass][side][bg];
      u32 hofs = read_ioreg(REG_BGxHOFS(bg)) & 0xFF;
      u32 h = (u32)((s32)hofs + xo + 512) & 0xFF;   /* the strip's offset */
      s32 wx0, wy, wt0, wty;
      u32 j, n = (WIDE_STRIP_MAX + 7 + 7) / 8;

      if (!em_bg_plain(bg))
         return 0;
      em_world(sn, bg, em_mosaic_line(bg, vcount), &wx0, &wy);
      wt0 = (wx0 + xo - (s32)(h & 7)) >> 3;
      wty = wy >> 3;
      /* The row is the same for the eight lines of a tile row (and the
       * same snapshot): made once, not on every line. */
      if (!em.rkey_ok[pass][side][bg] || key[0] != wt0 || key[1] != wty ||
          key[2] != (s32)(h >> 3))
      {
         for (j = 0; j < n; j++)
         {
            s32 e = em_entry(sn, bg, wt0 + (s32)j, wty);
            row[((h >> 3) + j) & 31] = eswap16((u16)(e < 0 ? 0 : e));
         }
         key[0] = wt0;
         key[1] = wty;
         key[2] = (s32)(h >> 3);
         em.rkey_ok[pass][side][bg] = 1;
      }
      out->row[bg] = row;
   }
   if (em.wipe && (xo < 0 || xo >= 225) &&
       (read_ioreg(REG_BGxCNT(0)) & 0xC0C0) == 0)
   {
      /* (a real margin only: the renderer's check draws strips of the
       * picture itself, where the text layer is its own) */
      /* The wipe that opens a battle paints the text layer black tile by
       * tile, across the picture.  Whatever tile it has at the picture's
       * edge on this line goes on to the screen's. */
      const u16 *map = (const u16 *)&vram[((read_ioreg(REG_BGxCNT(0)) >> 8) & 0x1F) * 2048];
      u32 h = read_ioreg(REG_BGxHOFS(0)) + (side ? 239 : 0);
      u32 v = read_ioreg(REG_BGxVOFS(0)) + vcount;
      u16 e = map[((v >> 3) & 31) * 32 + ((h >> 3) & 31)];
      u16 *row = em.rows[side][0];
      u32 j;
      for (j = 0; j < 32; j++)
         row[j] = e;
      out->row[0] = row;
      out->layers |= 1;
   }
   return 1;
}

static int em_detect(void)
{
   u32 code = rd32(0x080000ACu), rev = rd8(0x080000BCu), i;

   g3 = NULL;
   for (i = 0; i < sizeof(g3_games) / sizeof(g3_games[0]); i++)
      if (g3_games[i].code == code && g3_games[i].rev == rev)
         g3 = &g3_games[i];
   if (!g3)
      return 0;
   /* Emerald: the three routines its addresses stand on are known byte for
    * byte -- a translation or a hack built from other sources has them
    * elsewhere, and gets no profile.  (The others cannot be told apart
    * that way from here; for them the picture is the judge.) */
   if (code == G3_CODE('B','P','E','E') &&
       !(rd32(0x08085e5cu) == 0x4809b510u && rd32(0x08085e60u) == 0x09c079c0u &&
         rd32(0x0808df80u) == 0x4657b5f0u && rd32(0x08096530u) == 0x1c05b570u))
      g3 = NULL;
   return g3 != NULL;
}

const char *wide_profile_name(void)
{
   return wide_line_hook == em_line && g3 ? g3->name
        : wide_line_hook == au_line ? "automatic" : "";
}

/* ======================================================================== */


/* ======================================================================== */
/* Any game: the world as the game has shown it                              */
/* ======================================================================== */
/* A game the app has no profile for.  Nothing here knows what the game's
 * maps are; it only watches what the game DRAWS.
 *
 * A scrolling tile background is a ring: the game writes map entries into
 * it at the edges as the camera moves, and the scroll registers say which
 * part is on screen.  Following the scroll registers from frame to frame
 * (and not letting them wrap) gives every map entry that is ever on screen
 * a place in a WORLD that does not wrap.  Those are remembered -- per
 * background, a cache keyed by world tile -- and a margin is drawn from
 * what is remembered for the world tiles beside the screen.  So:
 *
 *   - what has been on screen is known wherever it lies now: the side the
 *     player came from, and everything in an area already walked;
 *   - the column a game draws just past the edge it is scrolling towards
 *     is taken too (AU_AHEAD), when the direction is not in doubt;
 *   - a place the game has never drawn is not known, and is drawn as
 *     nothing: the layers behind and the backdrop show.  Never a guess.
 *
 * Two more things are settled by what the screen proves:
 *   - a background that scrolls further than its ring has room for,
 *     without the ring being written, is showing its own other side: it
 *     repeats (a sky, a far range of hills), and its margins are simply
 *     more of the ring;
 *   - when what is on screen contradicts what is remembered for the same
 *     places, the game has changed the scene: everything remembered for
 *     that background is dropped.
 *
 * Text boxes and status bars are backgrounds that never scroll: nothing of
 * them is beside the screen, and nothing is drawn there.
 */
#define AU_CW 128                 /* the cache: world tiles, direct mapped */
#define AU_CH 64
#define AU_AHEAD_PX 32            /* how far past an edge a game pre-draws */
#define AU_JUMP_PX  64            /* a scroll step larger than this is a cut */

enum { AU_EMPTY = 0, AU_SEEN, AU_AHEAD, AU_SCOUT };

typedef struct au_cell
{
   u16 entry;
   s16 wx, wy;
   u8  src;
   u8  spare;
} au_cell;

typedef struct au_bg
{
   u8  on;                        /* followed in the frame being drawn      */
   u8  periodic;                  /* its ring repeats: margins are the ring */
   u8  have_last;
   u8  spare;
   u16 cnt;                       /* the BGCNT bits a map is defined by     */
   u16 h0, v0;                    /* scroll at line 0 of this frame         */
   u32 wt, ht;                    /* the ring, in tiles                     */
   s32 cam_x, cam_y;              /* world pixel of screen (0,0) at line 0;
                                   * always the scroll modulo the ring      */
   s32 lo_x, hi_x;                /* its travel since the ring was written  */
   s32 recent_dx;                 /* the last horizontal movement...        */
   u32 still;                     /* ...and the frames since                */
   s32 recent_dy;                 /* the same, vertically                   */
   u32 still_y;
   u8  scrolled;                  /* it has scrolled since it was forgotten */
   u8  follow;                    /* it moves as the camera does: a layer of
                                   * the world, not a panel in front of it   */
   u32 travel;                    /* how much it has moved lately (pixels
                                   * x 64, fading as the picture moves on)   */
   s16 fdx, fdy;                  /* ...and in this frame                    */
   u16 with_n, with_same;         /* frames the camera moved / it likewise   */
   u16 shadow[64 * 64];           /* the ring a frame ago                   */
   struct { u8 tx, ty, age; } pend[48];   /* cells written out of sight     */
   u32 npend;
   au_cell *cache;
} au_bg;

static struct
{
   au_bg bg[4];
   u16 rows[2][4][32];
   int oracle;                    /* (tests) compare with the profile       */
   /* for the scout (below) */
   u32 epoch;                     /* counts scene changes                   */
   u32 frame;
   /* What a layer that scrolls lacks, left / right, in the last whole
    * frame / so far in this one: [0] in the margin itself, [1] in the four
    * tiles further out (where a picture moving sideways is about to be),
    * [2] in the margin's columns of the rows just past the top or bottom
    * edge (where one moving up or down is about to be). */
   u32 unknown[3][2], unknown_now[3][2];
   u32 asked[2], asked_now[2];    /* ...of how many looked at, for [0]      */
   struct { s32 cam, cam_y; u32 frame; int set; } blocked[2];
#if WIDE_DEBUG
   u32 cover[2][2];               /* (tests) visible margin tiles of layers
                                   * that scroll: [side][known] */
#endif
   s32 main_cam;                  /* the camera of the layer that scrolls   */
   s32 main_cam_y;
   u32 main_bg;                   /* ...and which it is                     */
} au;

static inline s32 au_wrap(s32 d, s32 period)
{
   d %= period;
   if (d < 0) d += period;
   return d >= period / 2 ? d - period : d;
}

static inline u16 au_map(const au_bg *b, u32 tx, u32 ty)
{
   const u16 *map = (const u16 *)&vram[((b->cnt >> 8) & 0x1F) * 2048];
   u32 sb = (tx >> 5) + (ty >> 5) * (b->wt >> 5);
   return eswap16(map[sb * 1024 + (ty & 31) * 32 + (tx & 31)]);
}

static inline au_cell *auc_slot(au_cell *cache, s32 wx, s32 wy)
{
   return &cache[(u32)(wy & (AU_CH - 1)) * AU_CW + (u32)(wx & (AU_CW - 1))];
}

static inline const au_cell *auc_find(au_cell *cache, s32 wx, s32 wy)
{
   const au_cell *c = auc_slot(cache, wx, wy);
   return (c->src && c->wx == (s16)wx && c->wy == (s16)wy) ? c : NULL;
}

/* 1 = the cache did not have this entry at this place before. */
static inline int auc_put(au_cell *cache, s32 wx, s32 wy, u16 entry, u8 src)
{
   au_cell *c = auc_slot(cache, wx, wy);
   const int same = c->src && c->wx == (s16)wx && c->wy == (s16)wy;
   /* what was on screen is not replaced by what was only inferred */
   if (src != AU_SEEN && same && c->src == AU_SEEN)
      return 0;
   if (same && c->entry == entry)
   {
      if (src == AU_SEEN) c->src = AU_SEEN;
      return 0;
   }
   c->entry = entry; c->wx = (s16)wx; c->wy = (s16)wy; c->src = src;
   return 1;
}
#define au_find(b, wx, wy) auc_find((b)->cache, wx, wy)
#define au_put(b, wx, wy, e, src) auc_put((b)->cache, wx, wy, e, src)

static void au_forget_why(au_bg *b, const char *why)
{
#if WIDE_DEBUG
   if (wide_dbg)
      fprintf(stderr, "wide dbg auto f%u bg%d forgets: %s (cam %d,%d)\n",
              au.frame, (int)(b - au.bg), why, (int)b->cam_x, (int)b->cam_y);
#else
   (void)why;
#endif
   if (b->cache)
      memset(b->cache, 0, sizeof(au_cell) * AU_CW * AU_CH);
   b->periodic = 0;
   b->npend = 0;
   b->scrolled = 0;
   b->lo_x = b->hi_x = b->cam_x;
   wide_st.cuts++;
   au.epoch++;                    /* a scout under way is of another scene */
}
#define au_forget(b) au_forget_why(b, "?")

static void au_reset(void)
{
   u32 i;
   for (i = 0; i < 4; i++)
   {
      au_bg *b = &au.bg[i];
      if (b->cache)
         memset(b->cache, 0, sizeof(au_cell) * AU_CW * AU_CH);
      b->on = b->have_last = b->periodic = 0;
      b->npend = 0;
      b->scrolled = 0;
      b->follow = 0;
      b->travel = 0;
      b->with_n = b->with_same = 0;
      b->cnt = 0xFFFF;
   }
   au.epoch++;
   memset(au.unknown, 0, sizeof(au.unknown));
   memset(au.unknown_now, 0, sizeof(au.unknown_now));
   au.blocked[0].set = au.blocked[1].set = 0;
}

/* Line 0 of a frame: follow each tile background's scroll, see what the
 * game wrote into its ring, and check the screen against what is
 * remembered. */
static void au_frame(void)
{
   u32 dispcnt = read_ioreg(REG_DISPCNT), mode = dispcnt & 7, i;

   au.frame++;
#if WIDE_DEBUG
   if (wide_dbg && (au.frame % 30) == 0)
   {
      fprintf(stderr, "wide dbg cover f%u left %u/%u right %u/%u cam %d,%d\n", au.frame,
              au.cover[0][1], au.cover[0][0] + au.cover[0][1],
              au.cover[1][1], au.cover[1][0] + au.cover[1][1],
              (int)au.bg[au.main_bg & 3].cam_x, (int)au.bg[au.main_bg & 3].cam_y);
      memset(au.cover, 0, sizeof(au.cover));
   }
#endif
   memcpy(au.unknown, au.unknown_now, sizeof(au.unknown));
   memset(au.unknown_now, 0, sizeof(au.unknown_now));
   au.asked[0] = au.asked_now[0]; au.asked[1] = au.asked_now[1];
   au.asked_now[0] = au.asked_now[1] = 0;

   for (i = 0; i < 4; i++)
   {
      au_bg *b = &au.bg[i];
      u32 cnt, h, v, tx, ty, changed = 0, inplace = 0, hits = 0, bad = 0, k;
      s32 W, H, dx = 0, dy = 0;

      b->on = 0;
      b->fdx = b->fdy = 0;
      if (!((mode == 0 || (mode == 1 && i < 2)) && ((dispcnt >> (8 + i)) & 1)) ||
          (dispcnt & 0x80))
      {
         b->have_last = 0;        /* not shown: its scroll is not followed */
         continue;
      }
      if (!b->cache)
      {
         b->cache = (au_cell *)calloc(AU_CW * AU_CH, sizeof(au_cell));
         if (!b->cache)
            continue;
         b->cnt = 0xFFFF;
      }
      cnt = read_ioreg(REG_BGxCNT(i)) & 0xDF8C;
      h = read_ioreg(REG_BGxHOFS(i)) & 0x1FF;
      v = read_ioreg(REG_BGxVOFS(i)) & 0x1FF;
      if (cnt != b->cnt)
      {
         /* another map, or other tiles: nothing remembered applies */
         b->cnt = (u16)cnt;
         b->wt = (cnt & 0x4000) ? 64 : 32;
         b->ht = (cnt & 0x8000) ? 64 : 32;
         b->have_last = 0;
         b->cam_x = (s32)(h & (b->wt * 8 - 1));
         b->cam_y = (s32)(v & (b->ht * 8 - 1));
         au_forget_why(b, "another map or other tiles");
         memset(b->shadow, 0xFF, sizeof(b->shadow));
      }
      W = (s32)b->wt * 8; H = (s32)b->ht * 8;
      if (b->have_last)
      {
         dx = au_wrap((s32)h - (s32)b->h0, W);
         dy = au_wrap((s32)v - (s32)b->v0, H);
      }
      else
      {
         /* seen again after an absence: the same place, as far as known */
         dx = au_wrap((s32)h - b->cam_x, W);
         dy = au_wrap((s32)v - b->cam_y, H);
      }
      b->cam_x += dx; b->cam_y += dy;
      if (b->cam_x > 200000 || b->cam_x < -200000 ||
          b->cam_y > 200000 || b->cam_y < -200000)
      {
         b->cam_x = (s32)(h & (u32)(W - 1)); b->cam_y = (s32)(v & (u32)(H - 1));
         au_forget_why(b, "far from the origin");
      }
      else if (dx > AU_JUMP_PX || dx < -AU_JUMP_PX || dy > AU_JUMP_PX || dy < -AU_JUMP_PX)
         au_forget_why(b, "a jump, not a scroll");
      if (dx) { b->recent_dx = dx; b->still = 0; b->scrolled = 1; }
      else if (++b->still > 8) b->recent_dx = 0;
      if (dy) { b->recent_dy = dy; b->still_y = 0; b->scrolled = 1; }
      else if (++b->still_y > 8) b->recent_dy = 0;
      if (b->have_last && dx <= AU_JUMP_PX && dx >= -AU_JUMP_PX &&
          dy <= AU_JUMP_PX && dy >= -AU_JUMP_PX)
      {
         b->fdx = (s16)dx; b->fdy = (s16)dy;
      }

      /* ---- what the game wrote into the ring since the last frame */
      for (k = 0; k < b->npend; )
         if (++b->pend[k].age > 3) b->pend[k] = b->pend[--b->npend];
         else k++;
      for (ty = 0; ty < b->ht; ty++)
         for (tx = 0; tx < b->wt; tx++)
         {
            u16 e = au_map(b, tx, ty);
            if (e == b->shadow[ty * 64 + tx])
               continue;
            b->shadow[ty * 64 + tx] = e;
            changed++;
            {
               /* was it on screen a frame ago, and is it still? */
               s32 ox = au_wrap((s32)tx * 8 - b->cam_x, W), oy = au_wrap((s32)ty * 8 - b->cam_y, H);
               s32 px = ox < -7 ? ox + W : ox, py = oy < -7 ? oy + H : oy;
               if (px < 240 && py < 160 && px - dx > -8 && px - dx < 240 &&
                   py - dy > -8 && py - dy < 160)
                  inplace++;
            }
            if (b->npend < 48)
            {
               b->pend[b->npend].tx = (u8)tx; b->pend[b->npend].ty = (u8)ty;
               b->pend[b->npend].age = 0; b->npend++;
            }
         }
      /* A game that scrolls writes what is about to come into view; one
       * that repaints what is already in view has changed the scene (or
       * animates its map: then nothing is remembered, and nothing shown). */
      if (inplace > 40)
         au_forget_why(b, "the picture was repainted where it stood");
      if (changed || dx > 16 || dx < -16)
      {
         /* (a leap is no evidence either way: only a scroll that the eye
          * could follow counts as travel) */
         if (changed)
            b->periodic = 0;
         b->lo_x = b->hi_x = b->cam_x;
      }
      else
      {
         if (b->cam_x < b->lo_x) b->lo_x = b->cam_x;
         if (b->cam_x > b->hi_x) b->hi_x = b->cam_x;
         /* further than the ring has room, and nothing written: the screen
          * has shown the ring's other side.  It repeats. */
         if (b->hi_x - b->lo_x > W - 240)
            b->periodic = 1;
      }

      /* ---- written out of sight, just past the edge the picture is moving
       * towards: the column that is about to scroll in */
      for (k = 0; k < b->npend && b->recent_dx; )
      {
         s32 ox = au_wrap((s32)b->pend[k].tx * 8 - b->cam_x, W);
         s32 oy = au_wrap((s32)b->pend[k].ty * 8 - b->cam_y, H);
         s32 px = ox < 0 ? ox + W : ox;       /* 0 .. W-1 from the left edge */
         s32 py = oy < -7 ? oy + H : oy;
         int take = 0;
         if (py < -7 || py >= 160) { k++; continue; }
         if (px >= 240 && px <= W - 8)
         {
            if (b->recent_dx > 0 && px < 240 + AU_AHEAD_PX) take = 1;
            if (b->recent_dx < 0 && px >= W - 8 - AU_AHEAD_PX) { px -= W; take = 1; }
         }
         if (take)
            au_put(b, (b->cam_x + px) >> 3, (b->cam_y + py) >> 3,
                   au_map(b, b->pend[k].tx, b->pend[k].ty), AU_AHEAD);
         if (take || !(px >= 240 && px <= W - 8))
            b->pend[k] = b->pend[--b->npend];  /* placed, or in sight now */
         else
            k++;
      }

      /* ---- does what is remembered still hold for what is on screen? */
      for (ty = 0; ty < 20; ty++)
         for (tx = 0; tx < 30; tx++)
         {
            s32 wx = (b->cam_x + (s32)tx * 8) >> 3, wy = (b->cam_y + (s32)ty * 8) >> 3;
            const au_cell *c = au_find(b, wx, wy);
            if (!c)
               continue;
            hits++;
            if (c->entry != au_map(b, (u32)wx & (b->wt - 1), (u32)wy & (b->ht - 1)))
               bad++;
         }
      if (bad > 24 && bad * 5 > hits * 3)
         au_forget_why(b, "the screen contradicts what is remembered");

      /* (for the scout: a picture moving up or down is about to show the
       * rows past that edge; what the margins' columns lack there) */
      if (b->follow && b->recent_dy && !b->recent_dx && !b->periodic)
      {
         s32 r0 = b->recent_dy < 0 ? (b->cam_y - 48) >> 3 : (b->cam_y + 160) >> 3;
         s32 lx = (b->cam_x - WIDE_MARGIN_PX) >> 3, rx = (b->cam_x + 240) >> 3, r, c;
         for (r = r0; r < r0 + 6; r++)
            for (c = 0; c < 3; c++)
            {
               if (!au_find(b, lx + c, r)) au.unknown_now[2][0]++;
               if (!au_find(b, rx + c, r)) au.unknown_now[2][1]++;
            }
      }

      b->h0 = (u16)h; b->v0 = (u16)v;
      b->have_last = 1;
      b->on = 1;
   }

   /* "The camera": the layer that has moved most lately (the level, not
    * the sky behind it, and not a panel that slid in) -- "lately" counted in
    * frames in which something moved, so that standing still changes
    * nothing.  The layers that move exactly as it does are the world it
    * shows; one that does not is a panel in front of it -- a status bar, a
    * text box -- with nothing beside the screen, or a backdrop that goes its
    * own way. */
   {
      u32 best = 0, bi = au.main_bg & 3, any = 0;
      const au_bg *m;
      for (i = 0; i < 4; i++)
         any |= (u32)(au.bg[i].on && (au.bg[i].fdx || au.bg[i].fdy));
      for (i = 0; i < 4 && any; i++)
      {
         au_bg *b = &au.bg[i];
         b->travel -= b->travel >> 6;
         if (b->on)
            b->travel += (u32)((b->fdx < 0 ? -b->fdx : b->fdx) +
                               (b->fdy < 0 ? -b->fdy : b->fdy)) << 6;
      }
      for (i = 0; i < 4; i++)
         if (au.bg[i].on && au.bg[i].travel > best) { best = au.bg[i].travel; bi = i; }
      if (best >= (12u << 6))
         au.main_bg = bi;
      m = &au.bg[au.main_bg & 3];
      if (m->on && (m->fdx || m->fdy))
         for (i = 0; i < 4; i++)
         {
            au_bg *b = &au.bg[i];
            if (!b->on)
               continue;
            b->with_n++;
            if (b->fdx == m->fdx && b->fdy == m->fdy)
               b->with_same++;
            if (b->with_n >= 12)
               b->follow = (u32)b->with_same * 4 >= (u32)b->with_n * 3;
            if (b->with_n >= 64) { b->with_n >>= 1; b->with_same >>= 1; }
         }
      au.main_cam = m->cam_x;
      au.main_cam_y = m->cam_y;
   }
}

static int au_line(u32 vcount, int side, s32 xo, wide_line_t *out)
{
   u32 mode = read_ioreg(REG_DISPCNT) & 7, i;
   const int real = (side == 0 && xo < 0);
   int holes = 0;

   if (vcount == 0 && real)
      au_frame();
   if (mode > 2)
      return 0;
   out->layers = 0x1F;
   for (i = 0; i < 4; i++)
   {
      au_bg *b = &au.bg[i];
      u16 *row = au.rows[side][i];
      u32 hl, vl, line = vcount, mos, j, mask = 0, h8;
      s32 W, H, cxl, cyl, wx0, wy;

      if (!b->on)
         continue;                /* affine, or not shown */
      W = (s32)b->wt * 8; H = (s32)b->ht * 8;
      hl = read_ioreg(REG_BGxHOFS(i)) & 0x1FF;
      vl = read_ioreg(REG_BGxVOFS(i)) & 0x1FF;
      cxl = b->cam_x + au_wrap((s32)hl - (s32)b->h0, W);
      cyl = b->cam_y + au_wrap((s32)vl - (s32)b->v0, H);

      /* ---- remember the row of tiles that begins on this line */
      if (real && (vcount == 0 || ((cyl + (s32)vcount) & 7) == 0))
      {
         s32 x, x0 = cxl >> 3, x1 = (cxl + 239) >> 3;
         wy = (cyl + (s32)vcount) >> 3;
         for (x = x0; x <= x1; x++)
            au_put(b, x, wy, au_map(b, (u32)x & (b->wt - 1), (u32)wy & (b->ht - 1)),
                   AU_SEEN);
      }
      if (b->periodic)
         continue;                /* row stays NULL: more of the ring */

      /* ---- the strip's tiles, from what is remembered */
      mos = read_ioreg(REG_MOSAIC);
      if ((read_ioreg(REG_BGxCNT(i)) & 0x40) && (mos & 0xFF))
         line -= line % (((mos >> 4) & 0xF) + 1);
      wy = (cyl + (s32)line) >> 3;
      wx0 = (cxl + xo) >> 3;
      h8 = ((u32)((s32)hl + xo + 512) & 0xFF) >> 3;
      for (j = 0; j < (WIDE_STRIP_MAX + 7 + 7) / 8; j++)
      {
         const au_cell *c = au_find(b, wx0 + (s32)j, wy);
         u32 idx = (h8 + j) & 31;
         if (c)
         {
            row[idx] = eswap16(c->entry);
            mask |= 1u << idx;
         }
         else
            row[idx] = 0;
      }
      out->row[i] = row;
      out->mask[i] = mask;
      /* A layer of the world with a tile of this strip not known: the strip
       * would be a picture with a piece missing.  It is not drawn at all
       * (the caller fills that side of the line as it does a menu's). */
      if (b->follow)
      {
         /* (a strip may begin a few pixels early, for a mosaic: its end
          * is the margin's, whatever its start) */
         const s32 end = xo < 0 ? 0 : xo > 240 - (s32)wide_margin
                                    ? 240 + (s32)wide_margin : xo + (s32)wide_margin;
         const u32 last = (u32)(((cxl + end - 1) >> 3) - wx0);
         for (j = 0; j <= last && j < 8; j++)
            if (!(mask & (1u << ((h8 + j) & 31))))
               holes = 1;
      }
#if WIDE_DEBUG
      if (wide_dbg_dump && au.frame == (u32)wide_dbg_dump && (line & 7) == 0 && xo != 0)
         for (j = 0; j < 4; j++)
         {
            const au_cell *c = auc_slot(b->cache, wx0 + (s32)j, wy);
            fprintf(stderr, "wide dump f%u bg%u side%d line%u tile %d,%d %s slot{src %u key %d,%d e %04x} cam %d,%d\n",
                    au.frame, i, side, line, (int)(wx0 + (s32)j), (int)wy,
                    au_find(b, wx0 + (s32)j, wy) ? "known" : "UNKNOWN",
                    c->src, c->wx, c->wy, c->entry, (int)b->cam_x, (int)b->cam_y);
         }
#endif
      /* (for the scout: what a layer that scrolls lacks on this side, in
       * the margin and four tiles further out -- it is sent before the gap
       * is on screen) */
      if (b->follow && (xo < 0 || xo >= 225) && (line & 15) == 0)
      {
         au.asked_now[side] += 3;
         for (j = 0; j < 3; j++)
            if (!au_find(b, wx0 + (s32)j, wy))
               au.unknown_now[0][side]++;
         for (j = 0; j < 4; j++)
            if (!au_find(b, xo < 0 ? wx0 - 4 + (s32)j : wx0 + 3 + (s32)j, wy))
               au.unknown_now[1][side]++;
      }

#if WIDE_DEBUG
      if (b->follow && xo != 0 && (line & 7) == 0)
         for (j = 0; j < 3; j++)
            au.cover[side][(mask >> ((h8 + j) & 31)) & 1]++;
#endif
      if (real && (line & 7) == 0)
      {
         /* how much of the margins is known (a count every eighth line) */
         for (j = 0; j < 3; j++)
            if (mask & (1u << ((h8 + j) & 31))) wide_st.auto_known++;
            else wide_st.auto_unknown++;
      }
#if WIDE_DEBUG
      if (au.oracle && i >= 1 && (line & 7) == 0)
      {
         /* the profile knows what belongs there (its rows for this strip
          * were made when background 1 came by) */
         static wide_line_t t;
         static int have;
         if (i == 1)
         {
            memset(&t, 0, sizeof(t));
            have = em_line(vcount, side, xo, &t);
         }
         if (have && t.row[i])
            for (j = 0; j < 3; j++)
            {
               u32 idx = (h8 + j) & 31;
               if (!(mask & (1u << idx))) continue;
               if (t.row[i][idx] == row[idx]) wide_st.auto_right++;
               else wide_st.auto_wrong++;
            }
      }
#endif
   }
   return holes ? 0 : 1;
}

/* ======================================================================== */
/* The scout: what the game would draw a moment from now                     */
/* ======================================================================== */
/* The side a player walks towards is the side the game has not drawn yet.
 * But it will: a moment after the player moves there.  So the front end
 * (fe_gbadual_scout) copies the running machine into the app's second
 * machine -- the one that is the partner in a link, idle otherwise -- holds
 * LEFT or RIGHT on it for a second, and lets it run ahead, unseen and
 * unheard.  Whatever that copy shows of the world goes into the first
 * machine's memory of it (as AU_SCOUT: it gives way to what is really seen
 * later).  The copy is then thrown away; nothing it did happened.
 *
 * Both machines are this same code.  The first hands out its trackers'
 * state and its caches (wide_scout_export); the second follows its own
 * scroll registers from there, once a frame at vertical blank
 * (wide_scout_begin .. wide_scout_end), and writes into those caches.  It
 * stops as soon as its future leaves the scene: another map, a jump, a
 * screen that contradicts what the first machine remembers. */
static struct
{
   int active, aborted;
   u32 frames, learned;
   s32 start_cam, moved, moved_y;
   wide_scout_seed seed;          /* its bg[] are followed in place */
} sc;

int wide_scout_cam(void)
{
   return au.main_cam;
}

static int au_side_blocked(int side)
{
   /* where the last look that way found nothing new: not again until the
    * picture has moved, or a few seconds have passed */
   return au.blocked[side].set && au.frame - au.blocked[side].frame < 240 &&
          au.main_cam - au.blocked[side].cam < 8 &&
          au.blocked[side].cam - au.main_cam < 8 &&
          au.main_cam_y - au.blocked[side].cam_y < 8 &&
          au.blocked[side].cam_y - au.main_cam_y < 8;
}

int wide_scout_wanted(void)
{
   const au_bg *m = &au.bg[au.main_bg & 3];
   u32 lack[2];
   int side;
   if (wide_line_hook != au_line || !wide_margin)
      return 0;
   if (m->recent_dx)
   {
      /* moving sideways: only the side it is moving to (the other is where
       * it has just been), and further out than the margin */
      side = m->recent_dx > 0 ? 1 : 0;
      return (au.unknown[0][side] + au.unknown[1][side]) && !au_side_blocked(side)
             ? (side ? 1 : -1) : 0;
   }
   /* standing, or moving up or down: whichever side lacks more -- in the
    * margin, and in the rows it is moving to */
   lack[0] = au.unknown[0][0] + (m->recent_dy ? au.unknown[2][0] : 0);
   lack[1] = au.unknown[0][1] + (m->recent_dy ? au.unknown[2][1] : 0);
   side = lack[1] > lack[0] ? 1 : 0;
   if (!lack[side] || au_side_blocked(side))
      side ^= 1;
   return lack[side] && !au_side_blocked(side) ? (side ? 1 : -1) : 0;
}

/* -1 the picture is moving up, 1 down, 0 neither (or sideways as well). */
int wide_scout_vdir(void)
{
   const au_bg *m = &au.bg[au.main_bg & 3];
   if (wide_line_hook != au_line || m->recent_dx)
      return 0;
   return m->recent_dy > 0 ? 1 : m->recent_dy < 0 ? -1 : 0;
}

int wide_scout_cam_y(void)
{
   return au.main_cam_y;
}

/* What the margin on that side (-1 left, 1 right) lacks on screen now, in
 * hundredths of it. */
unsigned wide_scout_gap(int side)
{
   const int s = side > 0 ? 1 : 0;
   return au.asked[s] ? au.unknown[0][s] * 100u / au.asked[s] : 0;
}

/* -1 the picture is moving left, 1 right, 0 not sideways. */
int wide_scout_hdir(void)
{
   const au_bg *m = &au.bg[au.main_bg & 3];
   return m->recent_dx > 0 ? 1 : m->recent_dx < 0 ? -1 : 0;
}

u32 wide_scout_epoch(void)
{
   return au.epoch;
}

void wide_scout_export(wide_scout_seed *s)
{
   u32 i;
   memset(s, 0, sizeof(*s));
   s->main_bg = au.main_bg;
   for (i = 0; i < 4; i++)
   {
      const au_bg *b = &au.bg[i];
      if (!b->on || !b->cache || b->periodic)
         continue;
      s->bg[i].on = 1;
      s->bg[i].cnt = b->cnt;
      s->bg[i].h = b->h0; s->bg[i].v = b->v0;
      s->bg[i].wt = b->wt; s->bg[i].ht = b->ht;
      s->bg[i].cam_x = b->cam_x; s->bg[i].cam_y = b->cam_y;
      s->bg[i].cache = b->cache;
   }
}

void wide_scout_report(int dir, int moved_px, unsigned learned)
{
   int side = dir > 0 ? 1 : 0;
   (void)moved_px;
   wide_st.scouts++;
   wide_st.scout_learned += learned;
   if (!learned)
   {
      au.blocked[side].set = 1;
      au.blocked[side].cam = au.main_cam;
      au.blocked[side].cam_y = au.main_cam_y;
      au.blocked[side].frame = au.frame;
   }
   else
      au.blocked[side].set = 0;
}

void wide_scout_begin(const wide_scout_seed *s)
{
   u32 i;
   memset(&sc, 0, sizeof(sc));
   sc.seed = *s;
   for (i = 0; i < 4; i++)
   {
      sc.seed.bg[i].start_x = sc.seed.bg[i].cam_x;
      sc.seed.bg[i].start_y = sc.seed.bg[i].cam_y;
   }
   sc.active = 1;
}

void wide_scout_end(void)
{
   sc.active = 0;
}

int wide_scout_moved_y(void)
{
   return sc.moved_y;
}

int wide_scout_status(int *moved_px, unsigned *learned, unsigned *frames)
{
   if (moved_px) *moved_px = sc.moved;
   if (learned)  *learned = sc.learned;
   if (frames)   *frames = sc.frames;
   return sc.aborted;
}

/* The scout's machine, at the start of vertical blank: the frame it just
 * drew (it draws nothing; its registers and its map are what count). */
static void sc_frame(void)
{
   u32 dispcnt = read_ioreg(REG_DISPCNT), mode = dispcnt & 7, i, pass;
   s32 far = 0;

   if (sc.aborted)
      return;
   if ((dispcnt & 0x80) || mode > 1)
   {
      sc.aborted = 1;
      return;
   }
   for (pass = 0; pass < 2 && !sc.aborted; pass++)
      for (i = 0; i < 4 && !sc.aborted; i++)
      {
         wide_scout_bg *b = &sc.seed.bg[i];
         const u16 *map;
         u32 h, v, tx, ty, hits = 0, bad = 0;
         s32 W, H;

         if (!b->on)
            continue;
         W = (s32)b->wt * 8; H = (s32)b->ht * 8;
         map = (const u16 *)&vram[((b->cnt >> 8) & 0x1F) * 2048];
         if (!pass)
         {
            s32 dx, dy;
            if (!((mode == 0 || i < 2) && ((dispcnt >> (8 + i)) & 1)) ||
                (read_ioreg(REG_BGxCNT(i)) & 0xDF8C) != b->cnt)
            {
               sc.aborted = 1;    /* its future is another scene */
               break;
            }
            h = read_ioreg(REG_BGxHOFS(i)) & 0x1FF;
            v = read_ioreg(REG_BGxVOFS(i)) & 0x1FF;
            dx = au_wrap((s32)h - (s32)b->h, W);
            dy = au_wrap((s32)v - (s32)b->v, H);
            if (dx > AU_JUMP_PX || dx < -AU_JUMP_PX || dy > AU_JUMP_PX || dy < -AU_JUMP_PX)
            {
               sc.aborted = 1;
               break;
            }
            b->cam_x += dx; b->cam_y += dy;
            b->h = (u16)h; b->v = (u16)v;
         }
         /* (every tile with a pixel on its screen: 31 x 21 where the scroll
          * is not on a tile's edge) */
         for (ty = 0; ty <= (u32)(((b->cam_y + 159) >> 3) - (b->cam_y >> 3)); ty++)
            for (tx = 0; tx <= (u32)(((b->cam_x + 239) >> 3) - (b->cam_x >> 3)); tx++)
            {
               s32 wx = (b->cam_x >> 3) + (s32)tx, wy = (b->cam_y >> 3) + (s32)ty;
               u32 rx = (u32)wx & (b->wt - 1), ry = (u32)wy & (b->ht - 1);
               u16 e = eswap16(map[((rx >> 5) + (ry >> 5) * (b->wt >> 5)) * 1024 +
                                   (ry & 31) * 32 + (rx & 31)]);
               if (!pass)
               {
                  const au_cell *c = auc_find((au_cell *)b->cache, wx, wy);
                  if (c) { hits++; if (c->entry != e) bad++; }
               }
               else
                  sc.learned += (u32)auc_put((au_cell *)b->cache, wx, wy, e, AU_SCOUT);
            }
         if (!pass && bad > 24 && bad * 5 > hits * 3)
            sc.aborted = 1;       /* ...or the first machine's has changed */
      }
   (void)far;
#if WIDE_DEBUG
   if (wide_dbg && (sc.frames % 20) == 0)
      fprintf(stderr, "wide dbg scout frame %u aborted %d learned %u: bg0 on%d cam %d  bg1 on%d cam %d (h %u)  bg2 on%d cam %d\n",
              sc.frames, sc.aborted, sc.learned,
              sc.seed.bg[0].on, sc.seed.bg[0].cam_x, sc.seed.bg[1].on, sc.seed.bg[1].cam_x, sc.seed.bg[1].h,
              sc.seed.bg[2].on, sc.seed.bg[2].cam_x);
#endif
   i = sc.seed.main_bg & 3;
   sc.moved = sc.seed.bg[i].on ? sc.seed.bg[i].cam_x - sc.seed.bg[i].start_x : 0;
   sc.moved_y = sc.seed.bg[i].on ? sc.seed.bg[i].cam_y - sc.seed.bg[i].start_y : 0;
   sc.frames++;
}

/* ======================================================================== */
/* The test profile: video memory's own rows                                  */
/* ======================================================================== */

#if WIDE_DEBUG
static u16 test_rows[2][4][32];

static int test_line(u32 vcount, int side, s32 xo, wide_line_t *out)
{
   u32 mode = read_ioreg(REG_DISPCNT) & 7, bg;

   (void)xo;
   if (mode > 2)
      return 0;
   out->layers = 0x1F;
   for (bg = 0; bg < (mode == 0 ? 4u : mode == 1 ? 2u : 0u); bg++)
   {
      u32 cnt = read_ioreg(REG_BGxCNT(bg));
      u32 size = (cnt >> 14) & 3;
      u32 mos = (cnt & 0x40) ? ((read_ioreg(REG_MOSAIC) >> 4) & 0xF) + 1 : 1;
      u32 v = ((vcount - vcount % mos) + read_ioreg(REG_BGxVOFS(bg))) % 512;
      const u16 *map = (const u16 *)&vram[((cnt >> 8) & 0x1F) * 2048];

      if (size & 1)
         continue;                /* 512 wide: a row is not 32 entries */
      if ((size & 2) && v >= 256)
         map += 32 * 32;
      memcpy(test_rows[side][bg], map + ((v % 256) / 8) * 32, 32 * sizeof(u16));
      out->row[bg] = test_rows[side][bg];
   }
   return 1;
}
#endif

/* ======================================================================== */

/* A game without a profile, and not asked for more: its margins have
 * nothing in them (video.cc fills them: soft edges, a stretch, or bars). */
static int none_line(u32 vcount, int side, s32 xo, wide_line_t *out)
{
   (void)vcount; (void)side; (void)xo; (void)out;
   return 0;
}

static void wide_apply(void)
{
   int on = wide_want != WIDE_OFF;

   if (on && !wide_buf)
   {
      wide_buf = (u16 *)calloc(1, WIDE_BUFFER_SIZE);
      if (!wide_buf)
         on = 0;
   }
   wide_pixels = wide_buf;
   /* The profile, if the game has one; else what was asked for: nothing
    * (WIDE_KNOWN), what goes on by itself (WIDE_ALL: no hook, video.cc's own
    * rule for rotated/scaled backgrounds), or what the game has shown. */
   wide_line_hook = (on && wide_profiles_on && wide_profile == WIDE_PROFILE_EMERALD) ? em_line
                  : wide_want == WIDE_KNOWN ? none_line
                  : wide_want == WIDE_AUTO ? au_line : NULL;
#if WIDE_DEBUG
   au.oracle = 0;
   if (on && wide_want == WIDE_AUTO && getenv("GBADHOC_WIDE_ORACLE") &&
       wide_profile == WIDE_PROFILE_EMERALD)
   {
      wide_line_hook = au_line;   /* ...checked against the profile */
      au.oracle = 1;
   }
   if (on && wide_want == WIDE_TEST)
      wide_line_hook = test_line;
#endif
   au_reset();
   if ((u32)(on ? WIDE_MARGIN_PX : 0) != wide_margin)
   {
      wide_margin = on ? WIDE_MARGIN_PX : 0;
      reg[OAM_UPDATED] = 1;       /* the sprite lists' x range changed */
      if (wide_buf)
         memset(wide_buf, 0, WIDE_BUFFER_SIZE);
   }
   em.s[0].ok = em.s[1].ok = 0;
   em.use = NULL;
   em.good = -1;
}

void wide_set(int mode)
{
   if (mode < WIDE_OFF || mode > WIDE_TEST)
      mode = WIDE_OFF;
#if !WIDE_DEBUG
   if (mode == WIDE_TEST)
      mode = WIDE_OFF;
#endif
   if (mode == wide_want)
      return;
   wide_want = mode;
   wide_apply();
}

int wide_margin_px(void)
{
   return (int)wide_margin;
}

void wide_set_profiles(int on)
{
   if ((on != 0) != wide_profiles_on)
   {
      wide_profiles_on = on != 0;
      wide_apply();
   }
}

void wide_set_fill(int fill)
{
   wide_fill = fill == WIDE_FILL_BARS ? WIDE_FILL_BARS
             : fill == WIDE_FILL_STRETCH ? WIDE_FILL_STRETCH : WIDE_FILL_SOFT;
}

int wide_profile_id(void)
{
   return wide_line_hook == em_line ? WIDE_PROFILE_EMERALD
        : wide_line_hook == au_line ? WIDE_PROFILE_AUTO
        : wide_line_hook && wide_line_hook != none_line ? WIDE_PROFILE_TEST
        : WIDE_PROFILE_NONE;
}

void wide_game_loaded(void)
{
#if WIDE_DEBUG
   wide_dbg = getenv("GBADHOC_WIDE_DEBUG") != NULL;
   if (getenv("GBADHOC_WIDE_DUMP"))
      wide_dbg_dump = atoi(getenv("GBADHOC_WIDE_DUMP"));
   if (getenv("GBADHOC_WIDE_MOSAIC"))
      wide_dbg_mosaic = (int)strtol(getenv("GBADHOC_WIDE_MOSAIC"), NULL, 0);
#endif
   em.battle_layers = 0x1E;       /* the ground, the two effect layers, sprites */
#if WIDE_DEBUG
   if (getenv("GBADHOC_WIDE_BATTLE"))
      em.battle_layers = (u32)strtol(getenv("GBADHOC_WIDE_BATTLE"), NULL, 0);
#endif
   wide_profile = em_detect() ? WIDE_PROFILE_EMERALD : WIDE_PROFILE_NONE;
   memset(&wide_st, 0, sizeof(wide_st));
   wide_apply();
}

void wide_vblank(void)
{
   if (sc.active)
      sc_frame();
#if WIDE_DEBUG
   if (wide_dbg_mosaic >= 0)
      write_ioreg(REG_MOSAIC, (u16)wide_dbg_mosaic);
#endif
   if (wide_margin && (wide_line_hook == em_line || au.oracle))
      em_latch();
}

/* The picture that comes next was set up before the state was saved, and
 * the snapshot that went with it is not in a state.  One taken now is the
 * same unless the game had already moved its camera on -- which the check
 * before line 0 finds (that frame's margins stay black). */
void wide_state_loaded(void)
{
   em.use = NULL;
   em.good = -1;
   /* (what a game has shown stays remembered: a state from elsewhere is a
    * scene change like any other, and is found the same way) */
   wide_vblank();
}

void wide_stats(wide_stats_t *out)
{
   if (out)
      *out = wide_st;
   memset(&wide_st, 0, sizeof(wide_st));
}

void wide_selftest_set(int on)
{
   wide_selftest = on ? 1 : 0;
}

void wide_selftest_get(unsigned *pixels, unsigned *bad)
{
   if (pixels) *pixels = wide_selftest_px;
   if (bad)    *bad = wide_selftest_bad;
   wide_selftest_px = wide_selftest_bad = 0;
}
