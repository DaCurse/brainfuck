bf: bf.c mason_arena.c
	cc -ggdb -Wall -Wextra -o bf bf.c mason_arena.c

.PHONY: clean

clean:
	rm -v bf.exe bf 2>/dev/null || true
