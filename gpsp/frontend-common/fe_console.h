/* Copyright (c) the gpsp contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef FE_CONSOLE_H
#define FE_CONSOLE_H

/* Selected hardware family for ROM browsing and core dispatch. Keep the GBA
 * value zero so zero-initialized frontend configs preserve current behavior. */
typedef enum fe_console
{
  FE_CONSOLE_GBA = 0,
  FE_CONSOLE_GB,
  FE_CONSOLE_GBC,
  FE_CONSOLE_SNES,   /* run by the separate program snes.bin (snes/), never by a core here */
  FE_CONSOLE_NES,    /* FCEUmm (nes/core) inside this program, through nescore.h */
  FE_CONSOLE_WSWAN,  /* WonderSwan and Color: Beetle (wswan/core) inside, wscore.h */
  /* 2.0: the four Sega consoles, run by the separate program md.bin (md/): never by a core here.
   * Console ids of the launch file: md, sms, gg, sg, scd (playerkit/FORMATS.md). */
  FE_CONSOLE_MD,     /* Sega Genesis / Mega Drive: .md .gen .smd .bin .68k */
  FE_CONSOLE_SMS,    /* Sega Master System: .sms */
  FE_CONSOLE_GG,     /* Sega Game Gear: .gg */
  FE_CONSOLE_SG,     /* Sega SG-1000: .sg .sc */
  FE_CONSOLE_SCD,    /* Sega CD / Mega CD (launch id scd, same md.bin): .cue .chd .iso, with the player's own BIOS */
  FE_CONSOLE_COUNT
} fe_console_t;

#endif
