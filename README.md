# Brainfuck

Brainfuck interpreter in C.

## Build

```bash
# default build
make
# enable trace output
make TRACE=1
# enable big-number mode (16 bit cells)
make BIGNUM=1
# compile with -O3
make bf_opt
```

## Run

```bash
./bf examples/helloworld.b
./bf examples/rot13.b
./bf_opt examples/bitwidth.b
cat examples/helloworld_compact.b | ./bf_opt examples/brainfuck.b
```
