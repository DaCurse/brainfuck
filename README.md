# Brainfuck

Brainfuck interpreter in C

Converts BF into a more compact instruction AST, then compiles the AST to a linear bytecode before interpreting it.

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
# helloworld_compact.b is specifically made to be fed into brainfuck in brainfuck 
cat examples/helloworld_compact.b | ./bf_opt examples/brainfuck.b
```
