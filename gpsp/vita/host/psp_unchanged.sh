#!/bin/sh
# psp_unchanged.sh -- prove the -DVITA edits left the PSP build alone.
#
#   sh psp_unchanged.sh [base-commit]        (default: the upstream tag's commit)
#
# psp/ui_psp.c, osd_psp.c, config_psp.c and ctl_map.c carry the Vita port as
# #ifdef VITA.  With VITA undefined they must be the PSP's files, unchanged.
# There is no PSP toolchain on the development machine, so this compiles the
# base commit's files and the working tree's with the host compiler (-m32,
# -O2) against a small stub of the PSP SDK declarations they use, in four
# flag sets the PSP Makefile builds with, and compares the object code with
# its relocations and the data/string sections.  They must be identical.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
GPSP=$(cd "$HERE/../.." && pwd)
BASE=${1:-1964d8c}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/stub" "$T/base"

cat > "$T/stub/pspkernel.h" <<'STUB'
#ifndef STUB_PSPKERNEL_H
#define STUB_PSPKERNEL_H
#include <stddef.h>
#include <stdint.h>
typedef int SceUID;
typedef unsigned SceSize;
typedef struct { unsigned short year, month, day, hour, minute, second;
                 unsigned int microsecond; } ScePspDateTime;
typedef struct { unsigned st_mode; unsigned st_attr; long long st_size;
                 ScePspDateTime sce_st_ctime, sce_st_atime, sce_st_mtime;
                 unsigned st_private[6]; } SceIoStat;
typedef struct { SceIoStat d_stat; char d_name[256]; void *d_private;
                 int dummy; } SceIoDirent;
typedef struct { unsigned TimeStamp; unsigned Buttons;
                 unsigned char Lx, Ly, Rsrv[6]; } SceCtrlData;
#define PSP_O_RDONLY 1
#define PSP_O_WRONLY 2
#define PSP_O_CREAT 0x200
#define PSP_O_TRUNC 0x400
#define PSP_SEEK_SET 0
#define PSP_SEEK_END 2
enum { PSP_CTRL_SELECT = 1, PSP_CTRL_START = 8, PSP_CTRL_UP = 0x10,
       PSP_CTRL_RIGHT = 0x20, PSP_CTRL_DOWN = 0x40, PSP_CTRL_LEFT = 0x80,
       PSP_CTRL_LTRIGGER = 0x100, PSP_CTRL_RTRIGGER = 0x200,
       PSP_CTRL_TRIANGLE = 0x1000, PSP_CTRL_CIRCLE = 0x2000,
       PSP_CTRL_CROSS = 0x4000, PSP_CTRL_SQUARE = 0x8000 };
SceUID sceIoOpen(const char *, int, int);
int sceIoClose(SceUID);
int sceIoRead(SceUID, void *, SceSize);
int sceIoWrite(SceUID, const void *, SceSize);
long long sceIoLseek(SceUID, long long, int);
int sceIoLseek32(SceUID, int, int);
int sceIoRemove(const char *);
int sceIoMkdir(const char *, int);
int sceIoGetstat(const char *, SceIoStat *);
SceUID sceIoDopen(const char *);
int sceIoDread(SceUID, SceIoDirent *);
int sceIoDclose(SceUID);
unsigned sceKernelGetSystemTimeLow(void);
void sceKernelDcacheWritebackRange(const void *, unsigned);
int sceCtrlPeekBufferPositive(SceCtrlData *, int);
int sceDisplayWaitVblankStart(void);
#endif
STUB
for h in pspctrl.h pspdisplay.h pspiofilemgr.h; do
   echo '#include "pspkernel.h"' > "$T/stub/$h"
done

FILES="ui_psp.c osd_psp.c config_psp.c ctl_map.c"
for f in $FILES ui_psp.h osd_psp.h config_psp.h ctl_map.h video_psp.h; do
   git -C "$GPSP" show "$BASE:gpsp/psp/$f" > "$T/base/$f"
done

INC="-I$T/stub -I$GPSP/frontend-common -I$GPSP/netdrv -I$GPSP"
fail=0
for def in "" "-DGPSP_PLAYABLE" \
           "-DGBA_LINK -DGPSP_PLAYABLE -DGPSP_NO_TELEMETRY" \
           "-DGPSP_PERF_RIG -DGPSP_MEM_BUDGET -DGPSP_CATCH_SELFTEST"; do
   for f in $FILES; do
      ${CC:-gcc} -m32 -O2 -w -c $def -I"$T/base" $INC -I"$GPSP/psp" \
         -o "$T/a.o" "$T/base/$f"
      ${CC:-gcc} -m32 -O2 -w -c $def $INC -I"$GPSP/psp" \
         -o "$T/b.o" "$GPSP/psp/$f"
      a=$(objdump -d -r --no-show-raw-insn "$T/a.o" | tail -n +3 | md5sum)
      b=$(objdump -d -r --no-show-raw-insn "$T/b.o" | tail -n +3 | md5sum)
      c=$(objdump -s -j .rodata -j .rodata.str1.1 -j .rodata.str1.4 \
             -j .data "$T/a.o" 2>/dev/null | tail -n +4 | md5sum)
      d=$(objdump -s -j .rodata -j .rodata.str1.1 -j .rodata.str1.4 \
             -j .data "$T/b.o" 2>/dev/null | tail -n +4 | md5sum)
      if [ "$a" = "$b" ] && [ "$c" = "$d" ]; then
         echo "identical  $f  [$def]"
      else
         echo "DIFFERENT  $f  [$def]"
         fail=1
      fi
   done
done
[ $fail = 0 ] && echo "PSP configuration: object code and data unchanged" \
              || echo "PSP configuration CHANGED"
exit $fail
