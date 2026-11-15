// online_http.cpp -- the room client of online play (2.0); see online_http.h.
//
// W0 scaffold: every call says "not available" (no slot / no answer) so the
// tree builds and nothing touches the network.  Package H writes the real
// client on top of nethttps/ (the project's own Mbed TLS client).
#include <string.h>

#include "online_http.h"

extern "C" {

void online_http_config(const char *base_url, const char *app_version) { (void)base_url; (void)app_version; }
int  online_http_create(const online_desc *room) { (void)room; return -1; }
int  online_http_lookup(const char *code) { (void)code; return -1; }
int  online_http_join(const char *code, const online_desc *me) { (void)code; (void)me; return -1; }
int  online_http_poll(const char *code, const char *token, const char *new_pub)
{
   (void)code; (void)token; (void)new_pub;
   return -1;
}
int  online_http_relay(const char *code, const char *token) { (void)code; (void)token; return -1; }
int  online_http_endpoint(const char *code, const char *token, const char *relay_addr)
{
   (void)code; (void)token; (void)relay_addr;
   return -1;
}
int  online_http_close(const char *code, const char *token, const char *reason)
{
   (void)code; (void)token; (void)reason;
   return -1;
}
int  online_http_resolve(const char *host) { (void)host; return -1; }
int  online_http_busy(int op) { (void)op; return 0; }
int  online_http_take(online_reply *out)
{
   (void)out;
   return 0;
}
void online_http_drop(void) { }
void online_http_shutdown(void) { }

int online_json_get(const char *body, const char *path, char *out, size_t cap)
{
   (void)body; (void)path;
   if (out && cap)
      out[0] = 0;
   return 0;
}

int online_json_int(const char *body, const char *path, int def)
{
   (void)body; (void)path;
   return def;
}

size_t online_json_desc(char *out, size_t cap, const online_desc *d, const char *extra_members)
{
   (void)d; (void)extra_members;
   if (out && cap)
      out[0] = 0;
   return 0;
}

}   // extern "C"
