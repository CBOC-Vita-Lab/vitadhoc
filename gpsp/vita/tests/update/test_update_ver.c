/* test_update_ver.c -- the update check's pure parts (update_ver.c): version
 * numbers, their order, and what counts as a release in GitHub's answer.
 *
 *    gcc -Wall -Wextra -Werror -fsanitize=address,undefined -I vita \
 *        vita/tests/update/test_update_ver.c vita/update_ver.c -o t && ./t
 */
#include <stdio.h>
#include <string.h>

#include "update_ver.h"

static int checks, bad;
#define CHECK(c) do { checks++; if (!(c)) { bad++; \
   printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int parse(const char *s, unsigned v[4], char *shown)
{
   return upd_ver_parse(s, v, shown, 24);
}

static void test_parse(void)
{
   unsigned v[4];
   char shown[24];

   CHECK(parse("2.1", v, shown) && v[0] == 2 && v[1] == 1 && !v[2] && !v[3]);
   CHECK(!strcmp(shown, "2.1"));
   CHECK(parse("v2.1.3", v, shown) && v[2] == 3 && !strcmp(shown, "2.1.3"));
   CHECK(parse("V10.20.30.40", v, shown) && v[3] == 40);
   CHECK(parse("VitAdhoc-2.1", v, shown) && v[0] == 2 && !strcmp(shown, "2.1"));
   CHECK(parse("release 3", v, shown) && v[0] == 3 && !strcmp(shown, "3"));
   CHECK(parse("2.0.0", v, shown) && !strcmp(shown, "2.0.0"));
   /* not releases, not versions */
   CHECK(!parse("", v, shown));
   CHECK(!parse("latest", v, shown));
   CHECK(!parse("v", v, shown));
   CHECK(!parse("2.1-rc1", v, shown));
   CHECK(!parse("2.1rc1", v, shown));
   CHECK(!parse("2.1 beta", v, shown));
   CHECK(!parse("2.1.", v, shown));
   CHECK(!parse(".2.1", v, shown));
   CHECK(!parse("2..1", v, shown));
   CHECK(!parse("1.2.3.4.5", v, shown));
   CHECK(!parse("123456.1", v, shown));
   CHECK(!parse("2.1\n", v, shown));
   CHECK(!parse("<script>2.1", v, shown));
   CHECK(!parse("a very long name of a release 2.1", v, shown));
   CHECK(!parse(NULL, v, shown));
   /* shown must fit: a refusal, never a cut */
   CHECK(!upd_ver_parse("2.1", v, shown, 3));
   CHECK(upd_ver_parse("2.1", v, shown, 4));
   CHECK(upd_ver_parse("2.1", v, NULL, 0));
}

static void test_order(void)
{
   CHECK(upd_ver_newer("2.1", "2.0.0"));
   CHECK(upd_ver_newer("v2.0.1", "2.0.0"));
   CHECK(upd_ver_newer("2.0.0.1", "2.0.0"));
   CHECK(upd_ver_newer("3.0", "2.9.9"));
   CHECK(upd_ver_newer("2.10", "2.9"));          /* numbers, not text */
   CHECK(upd_ver_newer("10.0", "9.0"));
   CHECK(!upd_ver_newer("2.0", "2.0.0"));        /* the same */
   CHECK(!upd_ver_newer("v2.0.0", "2.0"));
   CHECK(!upd_ver_newer("1.4", "2.0.0"));        /* older */
   CHECK(!upd_ver_newer("2.0.0", "2.0.1"));
   CHECK(!upd_ver_newer("2.9", "2.10"));
   CHECK(!upd_ver_newer("garbage", "2.0.0"));
   CHECK(!upd_ver_newer("2.1-rc1", "2.0.0"));    /* a pre-release is no update */
   CHECK(!upd_ver_newer("2.1", "dev"));          /* nor is anything against a build with no version */
   CHECK(!upd_ver_newer(NULL, "2.0.0"));
}

static int rel(const char *json, char *shown)
{
   shown[0] = '\0';
   return upd_release_parse(json, strlen(json), shown, 24);
}

static void test_release(void)
{
   char s[24];

   CHECK(rel("{\"tag_name\":\"v2.1\",\"draft\":false,\"prerelease\":false}", s) &&
         !strcmp(s, "2.1"));
   /* a real answer's shape: the key order of the API, nested objects whose
    * keys look like ours, strings with braces and escapes */
   CHECK(rel("{\n \"url\": \"https://api.github.com/repos/x/y/releases/1\",\n"
             " \"author\": {\"login\": \"x\", \"tag_name\": \"v9.9\", \"draft\": true},\n"
             " \"tag_name\": \"v2.1.1\",\n \"name\": \"VitAdhoc {2.1.1} \\\"a\\\"\",\n"
             " \"draft\": false,\n \"prerelease\": false,\n"
             " \"assets\": [{\"name\": \"VitAdhoc.vpk\", \"prerelease\": true,"
             " \"tag_name\": \"v7\"}, []],\n \"body\": \"notes } ] \\\\ \"\n}", s) &&
         !strcmp(s, "2.1.1"));
   /* the same with the order turned round */
   CHECK(rel("{\"prerelease\":false,\"draft\":false,\"tag_name\":\"2.2\"}", s) &&
         !strcmp(s, "2.2"));
   /* what is not an update */
   CHECK(!rel("{\"tag_name\":\"v2.1\",\"draft\":true}", s));
   CHECK(!rel("{\"tag_name\":\"v2.1\",\"prerelease\":true}", s));
   CHECK(!rel("{\"tag_name\":\"v2.1-rc1\"}", s));
   CHECK(!rel("{\"tag_name\":\"latest\"}", s));
   CHECK(!rel("{\"tag_name\":null}", s));
   CHECK(!rel("{\"tag_name\":21}", s));
   CHECK(!rel("{\"tag_name\":\"v2.\\u0031\"}", s));     /* an escape: not a plain tag */
   CHECK(!rel("{\"name\":\"v2.1\"}", s));
   CHECK(!rel("{\"message\":\"Not Found\",\"status\":\"404\"}", s));
   CHECK(!rel("{\"tag_name\":\"v2.1\",\"prerelease\":\"yes\"}", s));
   /* garbage */
   CHECK(!rel("", s));
   CHECK(!rel("not json at all", s));
   CHECK(!rel("<html><body>captive portal</body></html>", s));
   CHECK(!rel("[\"tag_name\",\"v2.1\"]", s));
   CHECK(!rel("{}", s));
   CHECK(!rel("{\"tag_name\":\"v2.1\"", s));               /* cut off */
   CHECK(!rel("{\"tag_name\":\"v2.1\",", s));
   CHECK(!rel("{\"tag_name\" \"v2.1\"}", s));
   CHECK(!rel("{\"tag_name\":\"v2.1\" \"draft\":false}", s));
   CHECK(!rel("{\"a\":[1,2,{\"b\":\"c\"}}", s));           /* unbalanced */
   CHECK(!upd_release_parse(NULL, 5, s, sizeof(s)));
   CHECK(!upd_release_parse("{}", 0, s, sizeof(s)));
   /* a tag longer than any version */
   {
      char big[300];
      memset(big, '1', sizeof(big));
      memcpy(big, "{\"tag_name\":\"", 13);
      strcpy(big + 200, "\"}");
      CHECK(!rel(big, s));
   }
   /* nesting beyond reason does not run away */
   {
      char deep[200];
      size_t i;
      strcpy(deep, "{\"x\":");
      for (i = 0; i < 100; i++)
         strcat(deep, "[");
      CHECK(!rel(deep, s));
   }
   /* only the length given is read (no NUL needed) */
   {
      const char buf[] = "{\"tag_name\":\"v2.1\"}garbage";
      CHECK(upd_release_parse(buf, 19, s, sizeof(s)) && !strcmp(s, "2.1"));
      CHECK(!upd_release_parse(buf, 18, s, sizeof(s)));
   }
}

int main(void)
{
   test_parse();
   test_order();
   test_release();
   printf("test_update_ver: %s (%d checks, %d failed)\n", bad ? "FAILED" : "OK",
          checks, bad);
   return bad ? 1 : 0;
}
