# online_lib.c on its own: a folder tree, duplicate codes, the pure decision.
#   make -C gpsp/vita/tests/library -f online_lib.mk test
V = ../..
L = $(V)/third_party/lzma
LZ = $(addprefix $(L)/,7zAlloc.c 7zArcIn.c 7zBuf.c 7zCrc.c 7zCrcOpt.c 7zDec.c 7zStream.c \
       Bcj2.c Bra.c Bra86.c BraIA64.c CpuArch.c Delta.c Lzma2Dec.c LzmaDec.c)
ONLINE_LIB_FLAGS = -O1 -g -Wall -Wextra -Wno-format-truncation -std=gnu11 -DVITA -DGBA_LINK \
  -I$(V) -I$(V)/../psp -I$(V)/../frontend-common -I$(V)/../netdrv -I$(V)/.. -I$(L)
test: online_lib_test
	./online_lib_test
online_lib_test: online_lib_test.c $(V)/online_lib.c $(V)/online_lib.h $(V)/zip_rom.c $(V)/sz_rom.c $(LZ)
	$(CC) $(ONLINE_LIB_FLAGS) -Wl,--wrap=fopen -o $@ online_lib_test.c \
	  $(V)/online_lib.c $(V)/zip_rom.c $(V)/sz_rom.c $(LZ)
clean:
	rm -f online_lib_test
.PHONY: test clean
