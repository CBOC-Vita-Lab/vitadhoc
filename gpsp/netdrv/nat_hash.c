/* nat_hash.c -- see nat_hash.h.  W0 scaffold: package N writes the real
 * MD5 / SHA-1 / HMAC-SHA1 here.  Until then the digests are all zero, which
 * is why nothing calls them yet. */
#include <string.h>

#include "nat_hash.h"

void nat_md5(uint8_t out[16], const void *data, size_t len)
{
   (void)data; (void)len;
   memset(out, 0, 16);
}

void nat_sha1(uint8_t out[20], const void *data, size_t len)
{
   (void)data; (void)len;
   memset(out, 0, 20);
}

void nat_hmac_sha1(uint8_t out[20], const void *key, size_t klen, const void *msg, size_t mlen)
{
   (void)key; (void)klen; (void)msg; (void)mlen;
   memset(out, 0, 20);
}
