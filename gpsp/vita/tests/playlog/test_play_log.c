/* play_log.c: a full table makes room by forgetting the game played longest
 * ago (5,000 games played, the newest 2,048 kept).  gcc -I vita
 * -Ifrontend-common vita/tests/playlog/test_play_log.c vita/play_log.c */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "play_log.h"
void fe_evt(const char *f, ...) { (void)f; }
int main(void){
  char n[64]; unsigned i;
  play_log_reset();
  for (i = 0; i < 5000; i++) { snprintf(n, sizeof n, "game-%05u.gba", i); assert(play_log_add(n, 1000 + i, 5, 1) == 0); }
  assert(play_log_size() == 2048);
  assert(!play_log_get("game-00000.gba"));
  assert(play_log_get("game-04999.gba") && play_log_get("game-04999.gba")->last == 5999);
  assert(play_log_get("game-02952.gba"));
  assert(!play_log_get("game-02951.gba"));
  puts("play log eviction: ok"); return 0; }
