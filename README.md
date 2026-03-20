# Brainfuck

Brainfuck interpreter in C

Converts BF into a more compact instruction AST, then compiles the AST to a linear bytecode before interpreting it.

Has some optimization opcodes:

* `OP_CLR` - Set current cell to 0.
* `OP_MOVEADD` - Add value from current cell to a cell at offset `arg` and set current
    cell to 0.
* `OP_SCAN`- Move cells right or left in steps of `arg` until you find a cell that contains 0.

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
./bf_opt examples/mandel.b
# Examples ending in ! can be run through dbfi, a brainfuck in brainfuck interpreter
cat examples/helloworld_compact.b | ./bf_opt examples/dbfi.b
cat examples/twinkle.b | ./bf_opt examples/dbfi.b
cat examples/quine.b | ./bf_opt examples/dbfi.b
```
