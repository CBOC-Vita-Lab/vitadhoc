/* G9: a line longer than the line buffer is cut AND marked ('~'), counted in
 * fe_evt_cuts(), and reported in the evt_finish line.  Host test. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../frontend-common/fe_evt.c"
int main(int argc, char **argv)
{
   const char *p = argc > 1 ? argv[1] : "/tmp/t_evt_cut.log";
   static char big[1200], buf[8192];
   FILE *f; size_t n; char *l;
   memset(big, 'k', sizeof(big) - 1);
   assert(fe_evt_init(p, 0) == 0);
   fe_evt("short ok");
   fe_evt("%s", big);
   assert(fe_evt_cuts() == 1);
   fe_evt_close();
   f = fopen(p, "r"); assert(f);
   n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
   l = strstr(buf, "kkkk"); assert(l);
   { char *e = strchr(l, '\n'); assert(e && e[-1] == '~'); }
   assert(strstr(buf, "short ok\n"));
   printf("  evt cut: marked, counted (%u)\n", fe_evt_cuts());
   return 0;
}
