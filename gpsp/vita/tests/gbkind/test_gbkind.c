/* gbkind.h: header-based Game Boy / Game Boy Color detection and the per-file override. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../../gbkind.h"

static int fails;
static void check(int ok, const char *why) { if (!ok) { fprintf(stderr, "FAIL: %s\n", why); fails++; } }

static void make(const char *dir, const char *name, unsigned char flag, size_t size)
{
   char path[512];
   unsigned char *b = (unsigned char *)calloc(1, size ? size : 1);
   FILE *f;
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   f = fopen(path, "wb");
   if (size > 0x143) b[0x143] = flag;
   fwrite(b, 1, size, f);
   fclose(f);
   free(b);
}

static int kind_of(const char *dir, const char *name)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   return gbk_decide(path);
}

int main(void)
{
   char dir[] = "/tmp/vitadhoc-gbkind-XXXXXX", ovr[600];
   check(mkdtemp(dir) != NULL, "temp dir");
   check(gbk_from_flag(0x80) == GBK_GBC && gbk_from_flag(0xC0) == GBK_GBC, "0x80 and 0xC0 are Color");
   check(gbk_from_flag(0x00) == GBK_GB && gbk_from_flag(0x40) == GBK_GB && gbk_from_flag(0x01) == GBK_GB, "other values are a Game Boy");
   make(dir, "Blue Color Hack.gb", 0x80, 0x8000);          /* the reported case: a Color hack named .gb */
   make(dir, "Color Only.gb", 0xC0, 0x8000);
   make(dir, "Plain.gb", 0x00, 0x8000);
   make(dir, "Plain Misnamed.gbc", 0x00, 0x8000);         /* the reverse: a DMG-only game named .gbc */
   make(dir, "Real Color.gbc", 0x80, 0x8000);
   make(dir, "Upper.GB", 0xC0, 0x8000);
   make(dir, "Tiny.gbc", 0x80, 0x100);                    /* too short for a header: the name decides */
   check(kind_of(dir, "Blue Color Hack.gb") == GBK_GBC, "a .gb with a 0x80 header runs and lists as Color");
   check(kind_of(dir, "Color Only.gb") == GBK_GBC, "a .gb with a 0xC0 header is Color");
   check(kind_of(dir, "Plain.gb") == GBK_GB, "a plain .gb stays a Game Boy");
   check(kind_of(dir, "Plain Misnamed.gbc") == GBK_GB, "a DMG-only .gbc is a Game Boy");
   check(kind_of(dir, "Real Color.gbc") == GBK_GBC, "a Color .gbc stays Color");
   check(kind_of(dir, "Upper.GB") == GBK_GBC, "extension case does not matter");
   check(kind_of(dir, "Tiny.gbc") == GBK_GBC && gbk_header_kind("/nonexistent/x.gb") == GBK_NONE, "an unreadable header falls back to the extension");
   check(gbk_decide("/nonexistent/Plain.gb") == GBK_GB, "a missing file falls back to its extension");
   /* the per-game override wins over the header, by file name, any case, any folder */
   snprintf(ovr, sizeof(ovr), "%s/gb_override.txt", dir);
   {
      FILE *f = fopen(ovr, "wb");
      fputs("# my overrides\r\ngbc  Plain.gb\r\n  GB\tReal Color.gbc\nbogus line\ngbc\nGBC Blue color hack.GB\n", f);
      fclose(f);
   }
   check(gbk_overrides_load(ovr) == 3, "three valid override lines, comments and bad lines skipped");
   check(kind_of(dir, "Plain.gb") == GBK_GBC, "override: a plain .gb played as Color");
   check(kind_of(dir, "Real Color.gbc") == GBK_GB, "override: a Color .gbc played as a Game Boy");
   check(gbk_override_for("ux0:data/roms/Sub/Plain.gb") == GBK_GBC, "the override matches the file name in any folder");
   check(kind_of(dir, "Blue Color Hack.gb") == GBK_GBC, "a name matched without regard to case");
   check(kind_of(dir, "Color Only.gb") == GBK_GBC && kind_of(dir, "Plain Misnamed.gbc") == GBK_GB, "files without an override follow the header");
   gbk_overrides_clear();
   check(kind_of(dir, "Plain.gb") == GBK_GB, "no overrides: the header again");
   if (fails) return 1;
   puts("PASS: gbkind (header flag, both directions, unreadable header, override)");
   return 0;
}
