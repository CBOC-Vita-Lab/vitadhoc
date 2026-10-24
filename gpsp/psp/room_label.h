/* room_label.h -- how a wireless room is NAMED on screen.
 *
 * DISPLAY ONLY.  The room's identity is the ad-hoc group id -- CONFIG.INI
 * `group = GPSP07`, the sceNetAdhocctl group name every GBAdhoc since 1.0
 * scans for and joins -- and that string never changes: an older build must
 * still find a newer one's room.  What a player reads is "Room 7".
 *
 * THE ONE PLACE the on-screen form is decided.  Every screen that names a
 * room (the menu header chip, Settings > Room, the Wireless screen and its
 * scan list, the connecting frame, the toasts, the session overlay chip)
 * goes through room_label(); change ROOM_LABEL_WORD to rename them all.
 *
 * A group that is not one of the kinds below (a hand-edited CONFIG.INI, or
 * some other ad-hoc game's room answering a scan) is shown verbatim, so
 * nothing is ever hidden behind a label that would collide with a real room. */
#ifndef ROOM_LABEL_H
#define ROOM_LABEL_H

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#define ROOM_LABEL_WORD "Room"
#define ROOM_LABEL_CAP  16      /* "Room 99" or an 8-char group id, + NUL */

/* ---- what a room's name SAYS (docs/GBA-LINK-UI.md, "Room advertisement")
 *
 * The scan returns, per room, only the 8-byte adhocctl group name, so the
 * name carries the mode and the game:
 *
 *   GPSPnn     Wireless Adapter room nn: unchanged since 1.0, so "Join room
 *              7" on an older build still finds a newer one's adapter room.
 *   GCnnXXXX   Link Cable room nn, GBA; XXXX = the header code (0xAC..0xAF,
 *              a non-alphanumeric byte becomes '0').
 *   GBnnXXXX   Link Cable room nn, Game Boy / Color; XXXX = the first four
 *              alphanumeric bytes of the cartridge title, else GAME.
 *   GSnnDATA   "Share game data" room nn (docs/SAVE-SHARE.md): a PSP in its
 *              game list serving its saves and states, read-only.  NOT a
 *              link room: nothing speaks the link protocol in it, so a
 *              build from before 4.0 that joins it by its verbatim name
 *              finds nobody to link with and gives up.
 *   other      shown verbatim, joinable, the handshake decides.
 *
 * One room NUMBER (Settings > Room) for every kind: hosting Room 7 by cable
 * makes GC07AMKE, by adapter GPSP07.  Two rooms can share a number if their
 * modes differ; the scan list shows both. */
enum { ROOM_ADAPTER = 0, ROOM_CABLE_GBA, ROOM_CABLE_GB, ROOM_OTHER,
       ROOM_SHARE };
#define ROOM_SHARE_CODE "DATA"

static inline int room_is_digit(char c) { return c >= '0' && c <= '9'; }
static inline int room_is_alnum(char c)
{
   return room_is_digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* `group` -> its kind; *num = the room number (-1 for ROOM_OTHER); code =
 * the 4-character game code of a cable room ("" otherwise), NUL-ended. */
static inline int room_parse(const char *group, int *num, char code[5])
{
   int k = ROOM_OTHER, n = -1, i;
   if (code)
      code[0] = '\0';
   if (!group)
      group = "";
   if (group[0] == 'G' && group[1] == 'P' && group[2] == 'S' &&
       group[3] == 'P' && room_is_digit(group[4]) && room_is_digit(group[5]) &&
       group[6] == '\0')
   {
      k = ROOM_ADAPTER;
      n = (group[4] - '0') * 10 + (group[5] - '0');
   }
   else if (group[0] == 'G' && group[1] == 'S' && room_is_digit(group[2]) &&
            room_is_digit(group[3]) && !strcmp(group + 4, ROOM_SHARE_CODE))
   {
      k = ROOM_SHARE;
      n = (group[2] - '0') * 10 + (group[3] - '0');
   }
   else if (group[0] == 'G' && (group[1] == 'C' || group[1] == 'B') &&
            room_is_digit(group[2]) && room_is_digit(group[3]))
   {
      for (i = 4; i < 8 && room_is_alnum(group[i]); i++)
         ;
      if (i == 8 && group[8] == '\0')
      {
         k = group[1] == 'C' ? ROOM_CABLE_GBA : ROOM_CABLE_GB;
         n = (group[2] - '0') * 10 + (group[3] - '0');
         if (code)
         {
            memcpy(code, group + 4, 4);
            code[4] = '\0';
         }
      }
   }
   if (num)
      *num = n;
   return k;
}

/* A cable room's 4-character code from the GBA header bytes 0xAC..0xAF:
 * a non-alphanumeric byte becomes '0' (an adhocctl name is alphanumeric). */
static inline void room_code_gba(const char *hdr4, char out[5])
{
   int i;
   for (i = 0; i < 4; i++)
      out[i] = hdr4 && room_is_alnum(hdr4[i]) ? hdr4[i] : '0';
   out[4] = '\0';
}

/* A GB room's code: the first four alphanumeric bytes of the cartridge
 * title (0x134.., up to 16 bytes, may stop at a NUL), else GAME. */
static inline void room_code_gb(const char *title, size_t len, char out[5])
{
   size_t i;
   int k = 0;
   for (i = 0; title && i < len && title[i] && k < 4; i++)
      if (room_is_alnum(title[i]))
         out[k++] = title[i];
   if (k < 4)
      memcpy(out, "GAME", 4);
   out[4] = '\0';
}

/* kind + room number (0..99) + code -> the group name (8 bytes + NUL). */
static inline const char *room_make(int kind, int num, const char *code,
                                    char *out, size_t n)
{
   if (!out || !n)
      return "";
   num = num < 0 ? 0 : num % 100;
   if (kind == ROOM_SHARE)
      snprintf(out, n, "GS%02d" ROOM_SHARE_CODE, num);
   else if (kind == ROOM_CABLE_GBA || kind == ROOM_CABLE_GB)
      snprintf(out, n, "G%c%02d%.4s", kind == ROOM_CABLE_GBA ? 'C' : 'B',
               num, code && strlen(code) >= 4 ? code : "GAME");
   else
      snprintf(out, n, "GPSP%02d", num);
   return out;
}

/* `group` -> "Room N" (no leading zero) into buf; returns buf.  Every kind
 * of room is "Room N": the mode and the game are columns of their own. */
static inline const char *room_label(const char *group, char *buf, size_t n)
{
   int num;
   if (!buf || !n)
      return "";
   if (!group)
      group = "";
   if (room_parse(group, &num, NULL) != ROOM_OTHER)
      snprintf(buf, n, ROOM_LABEL_WORD " %d", num);
   else
      /* An adhocctl group name is at most 8 bytes. */
      snprintf(buf, n, "%.8s", group);
   return buf;
}

/* The room number of a group (Settings > Room is GPSPnn), or -1. */
static inline int room_number(const char *group)
{
   int num;
   room_parse(group, &num, NULL);
   return num;
}

#endif /* ROOM_LABEL_H */
