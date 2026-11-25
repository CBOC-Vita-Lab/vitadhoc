/* GBAdhoc: libgambatte's log calls go nowhere (the libretro log interface is not part of this build). */
#ifndef GBADHOC_GAMBATTE_LOG_H
#define GBADHOC_GAMBATTE_LOG_H
enum { RETRO_LOG_DEBUG = 0, RETRO_LOG_INFO, RETRO_LOG_WARN, RETRO_LOG_ERROR };
#define gambatte_log(...) ((void)0)
#endif
