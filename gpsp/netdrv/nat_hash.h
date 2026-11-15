/* nat_hash.h -- MD5, SHA-1 and HMAC-SHA1 for the NAT layer (STUN message
 * integrity, TURN long-term credentials).  Pure: no sockets, no threads, no
 * globals, C99, so the Vita build, the twin and the unit tests share it.
 * Owner: package N.  (W0 scaffold: the bodies are placeholders.) */
#ifndef NAT_HASH_H
#define NAT_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void nat_md5(uint8_t out[16], const void *data, size_t len);
void nat_sha1(uint8_t out[20], const void *data, size_t len);
void nat_hmac_sha1(uint8_t out[20], const void *key, size_t klen, const void *msg, size_t mlen);

#ifdef __cplusplus
}
#endif

#endif /* NAT_HASH_H */
