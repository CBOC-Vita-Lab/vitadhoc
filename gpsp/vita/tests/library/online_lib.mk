# online_lib.c on its own: a folder tree, duplicate codes, the pure decision.
#   make -C gpsp/vita/tests/library -f online_lib.mk test
V = ../..
ONLINE_LIB_FLAGS = -O1 -g -Wall -Wextra -Wno-format-truncation -std=gnu11 -DVITA -DGBA_LINK \
  -I$(V) -I$(V)/../psp -I$(V)/../frontend-common -I$(V)/../netdrv -I$(V)/..
test: online_lib_test
	./online_lib_test
online_lib_test: online_lib_test.c $(V)/online_lib.c $(V)/online_lib.h $(V)/zip_rom.c
	$(CC) $(ONLINE_LIB_FLAGS) -Wl,--wrap=fopen -o $@ online_lib_test.c \
	  $(V)/online_lib.c $(V)/zip_rom.c
clean:
	rm -f online_lib_test
.PHONY: test clean
