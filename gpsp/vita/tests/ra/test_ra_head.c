/* ra_http.c: the request's head is built with every piece counted (a long
 * host, path or agent never overruns).  Run under ASan:
 *   gcc -fsanitize=address,undefined -I vita -I frontend-common
 *       vita/tests/ra/test_ra_head.c vita/ra_http.c
 *       vita/third_party/rcheevos/src/rhash/md5.c -I vita/third_party/rcheevos/src -lpthread */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ra_http.c's thread plumbing is not under test here */
void plat_thread_sleep_us(unsigned us) { (void)us; }
int plat_net_start(void) { return 0; }
int plat_net_join(void) { return 0; }

char *ra_http_build_request(const char *path, const char *host,
                            const char *agent, const char *type, int post,
                            size_t plen);

int main(void)
{
   char agent[6000], host[3000], path[9000];
   char *h;
   memset(agent, 'a', sizeof(agent) - 1); agent[sizeof(agent) - 1] = 0;
   memset(host, 'h', sizeof(host) - 1); host[sizeof(host) - 1] = 0;
   memset(path, 'p', sizeof(path) - 1); path[0] = '/'; path[sizeof(path) - 1] = 0;
   h = ra_http_build_request("/x", "ra.example", "VitAdhoc/2.0", NULL, 0, 0);
   assert(h && !strncmp(h, "GET /x HTTP/1.1\r\nHost: ra.example\r\n", 35));
   free(h);
   h = ra_http_build_request("/x", "ra.example", "VitAdhoc/2.0", NULL, 1, 42);
   assert(h && strstr(h, "Content-Length: 42\r\n") &&
          strstr(h, "application/x-www-form-urlencoded"));
   free(h);
   /* the heads that overran the old fixed 1024 + strlen(path) */
   h = ra_http_build_request(path, host, agent, "text/plain; charset=utf-8-and-a-very-long-name", 1, 4294967295u);
   assert(h && strlen(h) > 1024 + strlen(path) && strstr(h, "\r\n\r\n") &&
          strlen(h) == strlen("POST  HTTP/1.1\r\nHost: \r\nUser-Agent: \r\nContent-Type: text/plain; charset=utf-8-and-a-very-long-name\r\nContent-Length: 4294967295\r\nConnection: close\r\n\r\n") + strlen(path) + strlen(host) + strlen(agent));
   free(h);
   puts("ra head: ok");
   return 0;
}
