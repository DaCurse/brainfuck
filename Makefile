CC = cc
CFLAGS = -ggdb -Wall -Wextra
OPT_CFLAGS = -O3 -DNDEBUG -Wall -Wextra
TRACE ?= 0
BIGNUM ?= 0

DEFINES =
ifeq ($(TRACE),1)
DEFINES += -DTRACE_PROGRAM
endif

ifeq ($(BIGNUM),16)
DEFINES += -DBF_BIGNUM16
else ifeq ($(BIGNUM),32)
DEFINES += -DBF_BIGNUM32
endif

ifeq ($(PROFILE), 1)
DEFINES += -DPROFILE
endif

bf: bf.c mason_arena.c
	$(CC) $(CFLAGS) $(DEFINES) -o bf bf.c mason_arena.c

bf_opt: bf.c mason_arena.c
	$(CC) $(OPT_CFLAGS) $(DEFINES) -o bf_opt bf.c mason_arena.c

.PHONY: clean

clean:
	rm -v bf.exe bf bf_opt.exe bf_opt 2>/dev/null || true
