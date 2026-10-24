# memprobe: a tiny EBOOT shaped like GBAdhoc (same heap/stack/MEMSIZE) that
# writes memprobe.txt next to itself.  docs/MEMORY-BUDGET.md.
#   docker run --rm -v "$PWD":/build -w /build/tools/memprobe pspdev/pspdev make
# Check the log for "stubs out of order" (broken imports, make still exits 0).
TARGET = memprobe
OBJS   = main.o

CFLAGS   = -O2 -G0 -Wall -Wextra -Wno-unused-parameter
CXXFLAGS = $(CFLAGS)
ASFLAGS  = $(CFLAGS)

# Only libraries psp-gcc's spec and build.mak do NOT already link
# (psp/Makefile, PSP stub-order trap).
LIBS = -lpspkubridge -lpspsystemctrl_user

EXTRA_TARGETS    = EBOOT.PBP
PSP_EBOOT_TITLE  = GBAdhoc MEMPROBE
# MEMSIZE=1, like a playable or harness64 GBAdhoc (a PSP-1000 ignores it).
PSP_LARGE_MEMORY = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
