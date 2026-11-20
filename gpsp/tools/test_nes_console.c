/* NES as the fifth console (VitAdhoc 1.4): which files each console lists.
 *   cc -DVITA -I psp -I frontend-common -I vita tools/test_nes_console.c -o /tmp/t && /tmp/t */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui_psp.h"

int main(void)
{
   assert(FE_CONSOLE_NES == 4 && FE_CONSOLE_COUNT == 10);
   assert(ui_rom_matches_console("Game.nes", FE_CONSOLE_NES));
   assert(ui_rom_matches_console("Game.NES", FE_CONSOLE_NES));
   assert(ui_rom_matches_console("Disk.fds", FE_CONSOLE_NES));
   assert(ui_rom_matches_console("Odd.unf", FE_CONSOLE_NES));
   assert(ui_rom_matches_console("Odd.UNIF", FE_CONSOLE_NES));
   assert(ui_rom_matches_console("a.b.nes", FE_CONSOLE_NES));
   assert(!ui_rom_matches_console("Game.sfc", FE_CONSOLE_NES));
   assert(!ui_rom_matches_console("Game.nes", FE_CONSOLE_SNES));
   assert(!ui_rom_matches_console("Game.nes", FE_CONSOLE_GBA));
   assert(!ui_rom_matches_console("Game.nesx", FE_CONSOLE_NES));
   assert(!ui_rom_matches_console("nes", FE_CONSOLE_NES));
   assert(!ui_rom_matches_console("", FE_CONSOLE_NES));
   /* every console's hint says START + SELECT (the SNES and Sega players open on it too) */
   assert(!strcmp(ui_menu_chord_text(FE_CONSOLE_SNES), "START + SELECT") &&
          !strcmp(ui_menu_chord_text(FE_CONSOLE_NES), "START + SELECT"));
   puts("nes console matching ok");
   return 0;
}
