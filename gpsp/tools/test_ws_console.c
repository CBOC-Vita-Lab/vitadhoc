/* WonderSwan as the sixth console (VitAdhoc 1.4): which files its tab lists.
 *   cc -DVITA -I psp -I frontend-common -I vita tools/test_ws_console.c -o /tmp/t && /tmp/t */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui_psp.h"

int main(void)
{
   assert(FE_CONSOLE_WSWAN == 5 && FE_CONSOLE_COUNT == 11);
   assert(ui_rom_matches_console("Game.ws", FE_CONSOLE_WSWAN));
   assert(ui_rom_matches_console("Game.WS", FE_CONSOLE_WSWAN));
   assert(ui_rom_matches_console("Colour.wsc", FE_CONSOLE_WSWAN));
   assert(ui_rom_matches_console("Pocket.pc2", FE_CONSOLE_WSWAN));
   assert(!ui_rom_matches_console("Game.ws.bak", FE_CONSOLE_WSWAN));
   assert(!ui_rom_matches_console("Game.nes", FE_CONSOLE_WSWAN));
   assert(!ui_rom_matches_console("Game.ws", FE_CONSOLE_NES));
   assert(!ui_rom_matches_console("Game.ws", FE_CONSOLE_GBA));
   assert(!ui_rom_matches_console("Game.wsx", FE_CONSOLE_WSWAN));
   assert(!ui_rom_matches_console("ws", FE_CONSOLE_WSWAN));
   assert(!strcmp(ui_menu_chord_text(FE_CONSOLE_WSWAN), "START + SELECT"));
   puts("ws console matching ok");
   return 0;
}
