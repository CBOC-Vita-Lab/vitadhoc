/* lan_host.c -- netdrv/transport_lan.c for the Linux twin.
 *
 * The twin's app objects are compiled -DVITA (it is the Vita app); the
 * transport's -DVITA branches are Sony headers and sceNetCtl calls.  This
 * wrapper compiles the same file as the desktop build does: BSD sockets,
 * getifaddrs.  Nothing else changes -- it is the code the console runs. */
#undef VITA
#include "../../netdrv/transport_lan.c"
