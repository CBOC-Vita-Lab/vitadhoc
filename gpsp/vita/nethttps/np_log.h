// np_log.h -- one line per event of the HTTPS client (never per packet or per frame).
//
// Vendored from Galaxy, which also wrote the lines to its own log file.  Here the app decides: np_https_set_log() (np_https.h)
// hands every line (without its newline) to a callback, and online_http.cpp passes them on to fe_evt from the frame thread
// (fe_evt has ONE producer; this code runs on the client's own worker threads).  The Vita discards stderr, so on the console
// the callback is the only log; the twin and the tests also get stderr.
#ifndef NP_LOG_H
#define NP_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern "C" void (*np_https_log_sink)(const char* line);   // np_https.cpp; null = none

// `format` ends with "\n" like the old fprintf lines. `tag` is the bracketed prefix, without brackets.
static inline void NpLogf(const char* tag, const char* format, ...) __attribute__((format(printf, 2, 3)));
static inline void NpLogf(const char* tag, const char* format, ...) {
    char line[384];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
#if !defined(NP_TARGET_VITA)
    fprintf(stderr, "[%s] %s", tag, line);
#endif
    void (*sink)(const char*) = np_https_log_sink;
    if (sink) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char out[400];
        snprintf(out, sizeof out, "[%s] %s", tag, line);
        sink(out);
    }
}

#endif  // NP_LOG_H
