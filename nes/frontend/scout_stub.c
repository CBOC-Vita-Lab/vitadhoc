/* The scout (a second, renamed copy of the core + host.c, see wide.c and INTEGRATION.md "Wide view: the scout") links
 * host.c, which calls these wide view hooks. Inside the scout they do nothing: the scout never has a wide view of its own.
 * Copyright CBOC-Vita. MIT licence. */
#include <stddef.h>
#include <stdint.h>
void nesw_on_load(void) {}
void nesw_on_unload(void) {}
void nesw_on_discontinuity(void) {}
void nesw_on_shutdown(void) {}
void nesw_on_rom(const uint8_t *data, size_t n, const char *name) { (void) data; (void) n; (void) name; }
