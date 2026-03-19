# Brainfuck

Brainfuck interpreter in C.

## Build

```bash
# debug build
make          
# trace build (prints ast)
make bf_trace  -> ./bf_trace
# optimized build
make bf_opt   
```

## Run

```bash
./bf examples/helloworld.b
./bf_trace examples/rot13.b
./bf_opt examples/bitwidth.b
```
