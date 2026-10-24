#include <sys/stat.h>

int af_stat(const char *path, struct stat *st);
#define stat(path, st) af_stat(path, st)

#include <netdb.h>
int af_getaddrinfo(const char *node, const char *service,
                   const struct addrinfo *hints, struct addrinfo **res);
#define getaddrinfo(n, s, h, r) af_getaddrinfo(n, s, h, r)
