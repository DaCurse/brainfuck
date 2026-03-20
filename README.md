# Brainfuck

Semi-optimizing Brainfuck interpreter in C

Converts BF into a more compact instruction AST, then compiles the AST to a linear bytecode before interpreting it.

## Instruction Set

```asm
; Move pointer
PTR     i64         ; step
; Change current cell
DATA    i64         ; delta
; Output character from current cell
OUT     i64         ; repeat count
; Input character into current cell
IN                  ; no operands
; Jump to address if current cell is 0
JZ      i64         ; absolute target
; Jump to address if current cell is not 0
JNZ     i64         ; absolute target

; The instructions below are optimizations of common patterns

; Set current cell to 0
CLR                 ; no operands
; Add value of current cell * multiplier to cell at offset, and set current cell to 0 
MOVEADD i32, i32    ; offset, multiplier
; Move pointer in fixed steps until 0 is encountered
SCAN    i64         ; step
```

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
