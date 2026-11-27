/* The four Sega consoles (VitAdhoc 2.0, md.bin): which files each tab lists.
 *   cc -DVITA -I psp -I frontend-common -I vita tools/test_sega_console.c -o /tmp/t && /tmp/t */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui_psp.h"
#include "ui_vita_md.h"

int main(void)
{
   assert(FE_CONSOLE_WSWAN == 5 && FE_CONSOLE_MD == 6 && FE_CONSOLE_SMS == 7 && FE_CONSOLE_GG == 8 &&
          FE_CONSOLE_SG == 9 && FE_CONSOLE_COUNT == 11);
   /* Genesis: four spellings (.bin is a candidate; the scan asks the bytes) */
   assert(ui_rom_matches_console("Sonic.md", FE_CONSOLE_MD));
   assert(ui_rom_matches_console("Game.cue", FE_CONSOLE_SCD));
   assert(ui_rom_matches_console("Game.CHD", FE_CONSOLE_SCD));
   assert(ui_rom_matches_console("Game.iso", FE_CONSOLE_SCD));
   assert(!ui_rom_matches_console("Game.bin", FE_CONSOLE_SCD));
   assert(!ui_rom_matches_console("Game.cue", FE_CONSOLE_MD));
   assert(ui_sega_letter(FE_CONSOLE_SCD) == 'M');
   assert(ui_rom_matches_console("Sonic.GEN", FE_CONSOLE_MD));
   assert(ui_rom_matches_console("Sonic.smd", FE_CONSOLE_MD));
   assert(ui_rom_matches_console("Sonic (USA).bin", FE_CONSOLE_MD));
   assert(!ui_rom_matches_console("Sonic.bin.bak", FE_CONSOLE_MD));
   assert(!ui_rom_matches_console(".md", FE_CONSOLE_MD));
   assert(!ui_rom_matches_console("md", FE_CONSOLE_MD));
   assert(!ui_rom_matches_console("Sonic.sms", FE_CONSOLE_MD));
   assert(!ui_rom_matches_console("Sonic.bin", FE_CONSOLE_SMS));
   assert(!ui_rom_matches_console("Sonic.md", FE_CONSOLE_SNES));
   assert(!ui_rom_matches_console("Sonic.md", FE_CONSOLE_GBA));
   /* the others: one each */
   assert(ui_rom_matches_console("Alex Kidd.sms", FE_CONSOLE_SMS));
   assert(ui_rom_matches_console("Alex Kidd.SMS", FE_CONSOLE_SMS));
   assert(!ui_rom_matches_console("Alex Kidd.sms", FE_CONSOLE_GG));
   assert(ui_rom_matches_console("Columns.gg", FE_CONSOLE_GG));
   assert(!ui_rom_matches_console("Columns.gg", FE_CONSOLE_SMS));
   assert(!ui_rom_matches_console("Columns.gb", FE_CONSOLE_GG));
   assert(ui_rom_matches_console("Girl's Garden.sg", FE_CONSOLE_SG));
   assert(ui_rom_matches_console("Sonic.68K", FE_CONSOLE_MD));
   assert(!ui_rom_matches_console("Sonic.68k", FE_CONSOLE_SG));
   assert(ui_rom_matches_console("Sega Card.SC", FE_CONSOLE_SG));
   assert(!ui_rom_matches_console("Sega Card.sc", FE_CONSOLE_SMS));
   assert(!ui_rom_matches_console("Girl's Garden.sg", FE_CONSOLE_SMS));
   assert(!ui_rom_matches_console("Game.gba", FE_CONSOLE_GG));
   /* other consoles keep their files */
   assert(!ui_rom_matches_console("Game.sfc", FE_CONSOLE_MD));
   assert(ui_rom_matches_console("Game.sfc", FE_CONSOLE_SNES));
   assert(ui_rom_matches_console("Game.ws", FE_CONSOLE_WSWAN));
   /* ids, letters, who plays what */
   assert(!strcmp(ui_sega_id(FE_CONSOLE_MD), "md") && !strcmp(ui_sega_id(FE_CONSOLE_SMS), "sms") &&
          !strcmp(ui_sega_id(FE_CONSOLE_GG), "gg") && !strcmp(ui_sega_id(FE_CONSOLE_SG), "sg"));
   assert(ui_sega_id(FE_CONSOLE_SNES) == NULL && ui_sega_id(FE_CONSOLE_GBA) == NULL);
   assert(ui_sega_letter(FE_CONSOLE_MD) == 'I' && ui_sega_letter(FE_CONSOLE_SMS) == 'J' &&
          ui_sega_letter(FE_CONSOLE_GG) == 'K' && ui_sega_letter(FE_CONSOLE_SG) == 'L');
   assert(ui_console_is_player(FE_CONSOLE_SNES) && ui_console_is_player(FE_CONSOLE_GG) && !ui_console_is_player(FE_CONSOLE_NES));
   assert(!strcmp(ui_menu_chord_text(FE_CONSOLE_MD), "START + SELECT"));   /* Start + Select opens the menu, as on GBA */
   puts("sega console matching ok");
   return 0;
}
