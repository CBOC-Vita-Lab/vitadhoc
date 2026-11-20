/* diskspace.h -- how much room a path's own device has, and an honest line
 * for a write that failed.
 *
 * "Save not written - is the memory card full?" used to be said for every
 * failed write, so a read-only folder or a vanished card said "full" on a
 * card with 150 GB free.  Now the device the file is on is asked (ux0:,
 * uma0:, imc0:, ur0: ... each its own), in 64-bit bytes, and the line says
 * what is true: full (with the room left), or not full (with the room left)
 * and the write failed for another reason, or "could not check".
 *
 * Header only.  On the Vita the query is sceIoDevctl(0x3001) then
 * sceAppMgrGetDevInfo; elsewhere (the tests) ds_set_query() supplies it.
 *
 * Licensed MIT OR GPL-2.0-or-later (VitAdhoc's own). */
#ifndef DISKSPACE_H
#define DISKSPACE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* below this a save, a state or a picture may not fit: "full" */
#define DS_LOW_BYTES (8ull * 1024ull * 1024ull)

typedef int (*ds_query_fn)(const char *dev, uint64_t *max_bytes, uint64_t *free_bytes);

#ifdef __vita__
#include <psp2/appmgr.h>
#include <psp2/io/devctl.h>
static int ds_native_query(const char *dev, uint64_t *max_bytes, uint64_t *free_bytes)
{
   SceIoDevInfo info;
   memset(&info, 0, sizeof(info));
   if (sceIoDevctl(dev, 0x3001, NULL, 0, &info, sizeof(info)) >= 0 &&
       info.max_size > 0)
   {
      *max_bytes = (uint64_t)info.max_size;           /* SceOff: 64-bit */
      *free_bytes = (uint64_t)info.free_size;
      return 1;
   }
   if (sceAppMgrGetDevInfo(dev, max_bytes, free_bytes) >= 0 && *max_bytes > 0)
      return 1;
   return 0;
}
static ds_query_fn ds_query_hook = ds_native_query;
#else
static ds_query_fn ds_query_hook;
#endif

static inline void ds_set_query(ds_query_fn fn) { ds_query_hook = fn; }

/* "ux0:data/x" -> "ux0:" ; a path with no "dev:" is on ux0:.  0 if it does
 * not fit. */
static inline int ds_device_of(const char *path, char *dev, size_t cap)
{
   const char *colon = path ? strchr(path, ':') : NULL;
   const char *slash = path ? strchr(path, '/') : NULL;
   size_t n;
   if (!dev || cap < 8)
      return 0;
   if (!colon || colon == path || colon - path > 6 || (slash && slash < colon))
   {
      snprintf(dev, cap, "ux0:");
      return 1;
   }
   n = (size_t)(colon - path) + 1;
   if (n + 1 > cap)
      return 0;
   memcpy(dev, path, n);
   dev[n] = '\0';
   return 1;
}

/* Free bytes of the device `path` is on; 1 if known. */
static inline int ds_free_for(const char *path, uint64_t *free_bytes, uint64_t *max_bytes)
{
   char dev[16];
   uint64_t mx = 0, fr = 0;
   if (!ds_query_hook || !ds_device_of(path, dev, sizeof(dev)))
      return 0;
   if (!ds_query_hook(dev, &mx, &fr) || fr > mx)
      return 0;
   if (free_bytes) *free_bytes = fr;
   if (max_bytes) *max_bytes = mx;
   return 1;
}

/* "150.3 GB", "420 MB", "3 KB": 64-bit throughout. */
static inline void ds_size_text(uint64_t b, char *out, size_t cap)
{
   if (b >= (1ull << 30))
      snprintf(out, cap, "%llu.%llu GB", (unsigned long long)(b >> 30),
               (unsigned long long)(((b & ((1ull << 30) - 1)) * 10) >> 30));
   else if (b >= (1ull << 20))
      snprintf(out, cap, "%llu MB", (unsigned long long)(b >> 20));
   else
      snprintf(out, cap, "%llu KB", (unsigned long long)(b >> 10));
}

/* The line for a write to `path` that failed.  `what` is the thing ("Save",
 * "Cover picture"); the result is for a toast. */
static inline const char *ds_failed_line(const char *what, const char *path,
                                         char *out, size_t cap)
{
   uint64_t fr = 0, mx = 0;
   char sz[24], dev[16];
   if (!ds_free_for(path, &fr, &mx))
      snprintf(out, cap, "%s not written - the card could not be checked", what);
   else
   {
      ds_size_text(fr, sz, sizeof(sz));
      (void)ds_device_of(path, dev, sizeof(dev));
      if (fr < DS_LOW_BYTES)
         snprintf(out, cap, "%s not written - %s is full (%s free)", what, dev, sz);
      else
         snprintf(out, cap, "%s not written - %s has %s free; is the folder read-only?",
                  what, dev, sz);
   }
   return out;
}

#endif
