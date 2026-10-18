#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../frontend-common/fe_util.c"

int main(void)
{
  char path[] = "/tmp/gbadhoc-ini-XXXXXX";
  char value[4401];
  char actual[64] = {0};
  int fd = mkstemp(path);
  FILE *file;
  assert(fd >= 0);
  file = fdopen(fd, "w");
  assert(file);
  assert(fputs("theme = original\nother = keep\n", file) >= 0);
  assert(fclose(file) == 0);

  memset(value, 'x', sizeof(value) - 1);
  value[sizeof(value) - 1] = '\0';
  assert(fe_ini_set(path, "theme", value) == -1);

  file = fopen(path, "r");
  assert(file);
  assert(fgets(actual, sizeof(actual), file));
  assert(strcmp(actual, "theme = original\n") == 0);
  assert(fgets(actual, sizeof(actual), file));
  assert(fclose(file) == 0);
  assert(strcmp(actual, "other = keep\n") == 0);
  puts("PASS oversized INI update is refused without truncating the existing file");

  /* fe_ini_never: that path reads as absent without being opened, and no
   * other path is affected. */
  {
    unsigned n0, n1, m;
    char v[32];
    assert(fe_ini_get(path, "other", v, sizeof(v)) && strcmp(v, "keep") == 0);
    fe_ini_stats(&n0, &m);
    fe_ini_never(path);
    assert(!fe_ini_get(path, "other", v, sizeof(v)));
    assert(fe_ini_get_int(path, "other", 42) == 42);
    fe_ini_stats(&n1, &m);
    assert(n1 == n0);
    assert(!fe_ini_get("/nonexistent/x.ini", "other", v, sizeof(v)));
    fe_ini_never(NULL);
    assert(fe_ini_get(path, "other", v, sizeof(v)) && strcmp(v, "keep") == 0);
    puts("PASS fe_ini_never skips exactly one path");
  }
  assert(unlink(path) == 0);
  return 0;
}
