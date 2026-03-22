CC = cc
CFLAGS = -std=c23 -ggdb -Wall -Wextra
OPT_CFLAGS = -O3 -DNDEBUG -Wall -Wextra
BIGNUM ?= 0

DEFINES =
ifeq ($(BIGNUM),16)
DEFINES += -DBF_BIGNUM16
else ifeq ($(BIGNUM),32)
DEFINES += -DBF_BIGNUM32
endif

bf: bf.c mason_arena.c
	$(CC) $(CFLAGS) $(DEFINES) -o bf bf.c mason_arena.c

bf_opt: bf.c mason_arena.c
	$(CC) $(OPT_CFLAGS) $(DEFINES) -o bf_opt bf.c mason_arena.c

.PHONY: clean

clean:
	rm -v bf.exe bf bf_opt.exe bf_opt 2>/dev/null || true
