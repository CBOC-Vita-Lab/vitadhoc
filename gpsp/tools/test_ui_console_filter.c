#include <assert.h>
#include <string.h>
#include "../psp/ui_psp.h"
#include "../psp/room_label.h"

/* room_label.h: display only -- the group id is never rewritten. */
static void test_room_label(void)
{
   char b[ROOM_LABEL_CAP];
   char tiny[5];
   assert(strcmp(room_label("GPSP07", b, sizeof(b)), "Room 7") == 0);
   assert(strcmp(room_label("GPSP00", b, sizeof(b)), "Room 0") == 0);
   assert(strcmp(room_label("GPSP10", b, sizeof(b)), "Room 10") == 0);
   assert(strcmp(room_label("GPSP99", b, sizeof(b)), "Room 99") == 0);
   /* Anything else is shown as it is. */
   assert(strcmp(room_label("GPSP7", b, sizeof(b)), "GPSP7") == 0);
   assert(strcmp(room_label("GPSP070", b, sizeof(b)), "GPSP070") == 0);
   assert(strcmp(room_label("GPSPx7", b, sizeof(b)), "GPSPx7") == 0);
   assert(strcmp(room_label("ULUS1234", b, sizeof(b)), "ULUS1234") == 0);
   assert(strcmp(room_label("", b, sizeof(b)), "") == 0);
   assert(strcmp(room_label(NULL, b, sizeof(b)), "") == 0);
   /* Truncates, never overruns. */
   assert(strcmp(room_label("GPSP42", tiny, sizeof(tiny)), "Room") == 0);
}

int main(void)
{
   assert(ui_rom_matches_console("Pokemon Red.gb", FE_CONSOLE_GB));
   assert(ui_rom_matches_console("Pokemon Red.GB", FE_CONSOLE_GB));
   assert(ui_rom_matches_console("Pokemon Blue.gB", FE_CONSOLE_GB));
   assert(!ui_rom_matches_console("Pokemon Crystal.gbc", FE_CONSOLE_GB));
   assert(ui_rom_matches_console("Pokemon Crystal.gbc", FE_CONSOLE_GBC));
   assert(ui_rom_matches_console("Pokemon Crystal.GbC", FE_CONSOLE_GBC));
   assert(ui_rom_matches_console("Pokemon Emerald.gba", FE_CONSOLE_GBA));
   assert(ui_rom_matches_console("Pokemon Emerald.GbA", FE_CONSOLE_GBA));
   assert(!ui_rom_matches_console("Pokemon Emerald.GBA", FE_CONSOLE_GB));
   assert(!ui_rom_matches_console("folder.gb/rom.gbc", FE_CONSOLE_GB));
   assert(!ui_rom_matches_console("extensionless", FE_CONSOLE_GBA));
   assert(!ui_rom_matches_console(NULL, FE_CONSOLE_GBA));
   assert(!ui_rom_matches_console("game.gb", (fe_console_t)99));
   assert(ui_rom_stem_length("Pokemon Red.gb") == 11);
   assert(ui_rom_stem_length("Pokemon Crystal.gbc") == 15);
   assert(ui_rom_stem_length("dir.with.dot/Pokemon Emerald.gba") == 28);
   test_room_label();
   return 0;
}
