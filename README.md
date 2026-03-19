# Brainfuck

Brainfuck interpreter in C

## Build

```bash
# default build
make
# enable trace output
make TRACE=1
# enable "Bignum" cells 
make BIGNUM=16 # or BIGNUM=32 for 32-bit cells
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
