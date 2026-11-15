// np_http.h -- plain http:// with the SHAPE of np_https (np_https.h): the twin and the loopback tests talk to a fake of the room
// service without TLS.  Same request and result structures, same handle / poll / cancel / release rules, own worker thread per request,
// the same time limits and the same answer cap.  Never compiled for the console: it talks TLS or nothing.
#ifndef NP_HTTP_H
#define NP_HTTP_H

#include "np_https.h"

#ifdef __cplusplus
extern "C" {
#endif

// url "http://host[:port]/path?query" (port default 80); keep_alive is ignored (every request asks the server to close).
NpHttpsHandle np_http_start(const NpHttpsRequest* request, NpHttpsError* err);
int np_http_poll(NpHttpsHandle handle, NpHttpsResult* out);   // as np_https_poll
void np_http_cancel(NpHttpsHandle handle);
void np_http_release(NpHttpsHandle handle);
void np_http_shutdown(uint32_t timeout_ms);                   // cancels everything and waits up to timeout_ms for the threads

#ifdef __cplusplus
}
#endif
#endif
