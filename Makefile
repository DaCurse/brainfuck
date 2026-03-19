CC = cc
CFLAGS = -ggdb -Wall -Wextra
OPT_CFLAGS = -O3 -DNDEBUG -Wall -Wextra

bf: bf.c mason_arena.c
	$(CC) $(CFLAGS) -o bf bf.c mason_arena.c

bf_trace: bf.c mason_arena.c
	$(CC) $(CFLAGS) -DTRACE_PROGRAM -o bf_trace bf.c mason_arena.c

bf_opt: bf.c mason_arena.c
	$(CC) $(OPT_CFLAGS) -o bf_opt bf.c mason_arena.c

.PHONY: clean

clean:
	rm -v bf.exe bf bf_trace.exe bf_trace bf_opt.exe bf_opt 2>/dev/null || true
