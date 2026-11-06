/* SNES as a fourth console (VitAdhoc 1.3): which files each console lists.
 *   cc -DVITA -I psp -I frontend-common -I vita tools/test_snes_console.c -o /tmp/t && /tmp/t */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui_psp.h"

int main(void)
{
   assert(FE_CONSOLE_SNES == 3 && FE_CONSOLE_COUNT == 4);
   assert(ui_rom_matches_console("Game.sfc", FE_CONSOLE_SNES));
   assert(ui_rom_matches_console("Game.SMC", FE_CONSOLE_SNES));
   assert(ui_rom_matches_console("a.b.Sfc", FE_CONSOLE_SNES));
   assert(!ui_rom_matches_console("Game.sfc", FE_CONSOLE_GBA));
   assert(!ui_rom_matches_console("Game.gba", FE_CONSOLE_SNES));
   assert(!ui_rom_matches_console("Game.sfx", FE_CONSOLE_SNES));
   assert(!ui_rom_matches_console("Game.smd", FE_CONSOLE_SNES));
   assert(!ui_rom_matches_console("sfc", FE_CONSOLE_SNES));
   assert(!ui_rom_matches_console(".sc", FE_CONSOLE_SNES));
   assert(!ui_rom_matches_console("", FE_CONSOLE_SNES));
   assert(ui_rom_matches_console("Game.gbc", FE_CONSOLE_GBC));
   assert(ui_rom_matches_console("Game.gb", FE_CONSOLE_GB));
   assert(ui_rom_matches_console("Game.GBA", FE_CONSOLE_GBA));
   puts("snes console matching ok");
   return 0;
}
