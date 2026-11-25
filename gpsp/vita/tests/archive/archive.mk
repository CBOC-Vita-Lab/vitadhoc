# The .7z reader (sz_rom.c over the LZMA SDK) on its own: make -C gpsp/vita/tests/archive -f archive.mk sz_cli
# test_archive.py builds the archives with 7z and drives it.
V = ../..
L = $(V)/third_party/lzma
SZ_FLAGS = -O1 -g -Wall -Wextra -std=gnu11 $(SAN) -I$(V) -I$(V)/../frontend-common -I$(L)
LZ = $(addprefix $(L)/,7zAlloc.c 7zArcIn.c 7zBuf.c 7zCrc.c 7zCrcOpt.c 7zDec.c 7zStream.c \
       Bcj2.c Bra.c Bra86.c BraIA64.c CpuArch.c Delta.c Lzma2Dec.c LzmaDec.c)
sz_cli: sz_cli.c $(V)/sz_rom.c $(V)/sz_rom.h $(V)/zip_rom.c $(V)/zip_rom.h $(LZ)
	$(CC) $(SZ_FLAGS) -o $@ sz_cli.c $(V)/sz_rom.c $(V)/zip_rom.c $(LZ)
clean:
	rm -f sz_cli
.PHONY: clean
