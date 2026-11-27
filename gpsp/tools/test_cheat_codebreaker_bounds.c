/* Regression coverage for malformed variable-length CodeBreaker payloads. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../common.h"
#include "../cheats.h"
#include "../gba_memory.h"

/* Bytes the cheat machine wrote.  (cheat_engine.c writes a super code's
 * payload as halfwords; counting bytes keeps "eight payload bytes" eight.) */
static unsigned writes;
u16 io_registers[512];
/* cheats.c runs the decoded program (cheat_engine.c, linked for real) against
 * this instance's memory.  A write to work RAM looks at the SMC tag planes
 * beside the two RAMs, so those exist (all zero: no translated code), and
 * the cartridge-patch machinery is given an empty cartridge. */
u8 ewram[1024 * 256 * 2];
u8 iwram[1024 * 32 * 2];
u8 *memory_map_read[8 * 1024];
u32 gamepak_size;
bool gamepak_mini_materialized;
int dynarec_enable;
bool gamepak_must_swap(void) { return false; }
void gamepak_map_page(u32 phys, u8 *ptr) { (void)phys; (void)ptr; }
void flush_translation_cache_ram(void) {}
void flush_dynarec_caches(void) {}
u32 read_memory8(u32 address) { (void)address; return 0; }
u32 read_memory16(u32 address) { (void)address; return 0; }
u32 read_memory32(u32 address) { (void)address; return 0; }
cpu_alert_type write_memory8(u32 address, u8 value) { (void)address; (void)value; writes += 1; return 0; }
cpu_alert_type write_memory16(u32 address, u16 value) { (void)address; (void)value; writes += 2; return 0; }
cpu_alert_type write_memory32(u32 address, u32 value) { (void)address; (void)value; writes += 4; return 0; }

/* The addresses are in work RAM (0x02xxxxxx): the cheat machine only writes
 * where a cheat device can, and tells a plain CodeBreaker list from an
 * encrypted one partly by whether its addresses are RAM.  (The earlier core
 * had no such rule, and these tests used arbitrary low addresses.) */
int main(void)
{
   char oversized[8193];   /* cheats.c refuses an entry of 8192 characters or more */
   /* value=4 means eight payload bytes, requiring two six-byte records.
    * Supplying one must be rejected without activating or executing it. */
   cheat_clear();
   assert(cheat_parse(0, "52001000 0004\n12345678 9abc") == CheatErrorNotSupported);
   process_cheats();
   assert(writes == 0);

   /* A complete two-record payload remains accepted and writes eight payload
    * bytes, guarding the valid boundary adjacent to the malformed case. */
   cheat_clear();
   assert(cheat_parse(0,
      "52001000 0004\n12345678 9abc\ndef01234 5678") == CheatNoError);
   process_cheats();
   assert(writes == 8);

   /* Opcode 4 is also variable-length and must not leave a missing parameter
    * record behind as an executable opcode. */
   cheat_clear();
   assert(cheat_parse(1, "42002000 000a") == CheatErrorNotSupported);
   process_cheats();
   assert(writes == 8);

   /* A complete slide record still writes the requested number of values. */
   cheat_clear();
   assert(cheat_parse(1, "42002000 000a\n00020003 0004") == CheatNoError);
   process_cheats();
   assert(writes == 14);

   /* An oversized replacement clears the old active cheat as well. */
   memset(oversized, '0', sizeof(oversized) - 1);
   oversized[sizeof(oversized) - 1] = '\0';
   assert(cheat_parse(1, oversized) == CheatErrorTooBig);
   process_cheats();
   assert(writes == 14);

   /* A conditional that skips "the next code" skips a whole super code.  Its
    * payload record here reads as opcode 8 (16-bit write); skipping only the
    * header used to execute it.  The trailing 8-bit write still runs. */
   cheat_clear();
   writes = 0;
   assert(cheat_parse(2, "A2001000 0000\n52001000 0003\n83000000 1234\n"
                         "32000100 0001") == CheatNoError);
   process_cheats();
   assert(writes == 1);

   /* Payload bytes are data: a record whose top nibble is 9 does not make
    * the cheat "encrypted" (it was rejected as such before). */
   cheat_clear();
   writes = 0;
   assert(cheat_parse(2, "52001000 0003\n9ABCDEF0 1234") == CheatNoError);
   process_cheats();
   assert(writes == 6);
   puts("PASS CodeBreaker opcodes 4/5: truncated payloads rejected, "
        "payload records never run as opcodes");
   return 0;
}
