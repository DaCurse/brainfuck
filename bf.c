#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mason_arena.h"

#define TAPE_SIZE 30000
#define DA_INITIAL_CAPACITY 256

#define da_append_many(da, src, n)                                             \
    do {                                                                       \
        auto _da = (da);                                                       \
        auto _src = (src);                                                     \
        size_t _n = (n);                                                       \
        size_t item_size = sizeof(*_da->items);                                \
        if (_da->capacity - _da->count < _n) {                                 \
            size_t new_capacity =                                              \
                _da->capacity == 0 ? DA_INITIAL_CAPACITY : _da->capacity * 2;  \
            while (new_capacity < _da->count + _n) {                           \
                new_capacity *= 2;                                             \
            }                                                                  \
            _da->items = mason_arena_realloc(arena,                            \
                                             _da->items,                       \
                                             _da->capacity * item_size,        \
                                             new_capacity * item_size);        \
            assert(_da->items != NULL);                                        \
            _da->capacity = new_capacity;                                      \
        }                                                                      \
        memcpy(_da->items + _da->count, _src, _n * item_size);                 \
        _da->count += _n;                                                      \
    } while (0)

#define da_append(da, src) da_append_many(da, src, 1)

#ifdef PROFILE
#define PROFILE_START(name)                                                    \
    struct timespec _timer_##name;                                             \
    clock_gettime(CLOCK_MONOTONIC, &_timer_##name)
#define PROFILE_END(name)                                                      \
    fprintf(stderr,                                                            \
            "\nPROFILE: " #name " took %.3fms\n",                              \
            timer_elapsed(&_timer_##name))

static inline double timer_elapsed(struct timespec *t)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double ms = (double)(now.tv_sec - t->tv_sec) * 1e3;
    ms += (double)(now.tv_nsec - t->tv_nsec) / 1e6;
    return ms;
}
#else
#define PROFILE_START(_name) (void)0
#define PROFILE_END(_name) (void)(0)
#endif

static MASON_Arena *arena;

typedef enum {
    TOKEN_END,
    TOKEN_RARROW,
    TOKEN_LARROW,
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_DOT,
    TOKEN_COMMA,
    TOKEN_OBRACKET,
    TOKEN_CBRACKET,
} TokenKind;

typedef enum {
    BF_CHANGE_PTR,
    BF_CHANGE_DATA,
    BF_OUTPUT,
    BF_INPUT,
    BF_LOOP
} InstructionKind;

typedef struct Instruction Instruction;

typedef struct {
    bool valid;
    Instruction **items;
    size_t count;
    size_t capacity;
} Instructions;

struct Instruction {
    InstructionKind kind;
    union {
        ptrdiff_t ptr_diff;
        int32_t data_diff;
        size_t output_count;
        Instructions loop;
    } as;
};

typedef struct {
    char *items;
    size_t count;
    size_t capacity;
} Buffer;

typedef struct {
    size_t pos;
    size_t bol;
    size_t row;
} Cursor;

typedef struct {
    const char *source;
    size_t count;

    Cursor cur;
    TokenKind token;
} Lexer;

typedef enum {
    OP_INVALID,

    // Default bf opcodes
    OP_PTR,
    OP_DATA,
    OP_OUT,
    OP_IN,
    OP_JZ,
    OP_JNZ,

    // Optimizations
    /* Set current cell to 0 */
    OP_CLR,
    /* Add value from current cell to a cell at offset `arg` and set current
       cell to 0*/
    OP_MOVEADD,
    /* Move cells right or left in steps of `arg` until you find a cell that
       contains 0 */
    OP_SCAN,

    OP_MAX
} OpcodeKind;

typedef struct {
    OpcodeKind kind;
    int64_t arg;
} Opcode;

typedef struct {
    Opcode *items;
    size_t count;
    size_t capacity;
} Program;

#ifdef BF_BIGNUM16
typedef uint16_t Cell;
#elif defined(BF_BIGNUM32)
typedef uint32_t Cell;
#else
typedef uint8_t Cell;
#endif

typedef struct {
    Cell *tape;
    size_t tape_size;
    ptrdiff_t data_ptr;
} Brainfuck;

Instruction *change_ptr(ptrdiff_t diff)
{
    Instruction *inst = mason_arena_alloc(arena, sizeof(*inst));
    inst->kind = BF_CHANGE_PTR;
    inst->as.ptr_diff = diff;
    return inst;
}

Instruction *change_data(int32_t diff)
{
    Instruction *inst = mason_arena_alloc(arena, sizeof(*inst));
    inst->kind = BF_CHANGE_DATA;
    inst->as.data_diff = diff;
    return inst;
}

Instruction *output(size_t count)
{
    Instruction *inst = mason_arena_alloc(arena, sizeof(*inst));
    inst->kind = BF_OUTPUT;
    inst->as.output_count = count;
    return inst;
}

Instruction *input()
{
    Instruction *inst = mason_arena_alloc(arena, sizeof(*inst));
    inst->kind = BF_INPUT;
    return inst;
}

Instruction *loop(Instructions body)
{
    Instruction *inst = mason_arena_alloc(arena, sizeof(*inst));
    inst->kind = BF_LOOP;
    inst->as.loop = body;
    return inst;
}

void display_token(TokenKind kind)
{
    switch (kind) {
    case TOKEN_END: printf("TOKEN_END\n"); break;
    case TOKEN_RARROW: printf("TOKEN_RARROW\n"); break;
    case TOKEN_LARROW: printf("TOKEN_LARROW\n"); break;
    case TOKEN_PLUS: printf("TOKEN_PLUS\n"); break;
    case TOKEN_MINUS: printf("TOKEN_MINUS\n"); break;
    case TOKEN_DOT: printf("TOKEN_DOT\n"); break;
    case TOKEN_COMMA: printf("TOKEN_COMMA\n"); break;
    case TOKEN_OBRACKET: printf("TOKEN_OBRACKET\n"); break;
    case TOKEN_CBRACKET: printf("TOKEN_CBRACKET\n"); break;
    default: assert(0 && "UNREACHABLE: TokenKind");
    }
}

void display_instruction_source(Instruction *inst)
{
    switch (inst->kind) {
    case BF_CHANGE_PTR: {
        char inst_repr = inst->as.ptr_diff > 0 ? '>' : '<';
        ptrdiff_t abs_diff =
            inst->as.ptr_diff < 0 ? -inst->as.ptr_diff : inst->as.ptr_diff;
        for (ptrdiff_t i = 0; i < abs_diff; i++) {
            putchar(inst_repr);
        }
    } break;
    case BF_CHANGE_DATA: {
        char inst_repr = inst->as.data_diff > 0 ? '+' : '-';
        int32_t abs_diff =
            inst->as.data_diff < 0 ? -inst->as.data_diff : inst->as.data_diff;
        for (int32_t i = 0; i < abs_diff; i++) {
            putchar(inst_repr);
        }
    } break;
    case BF_OUTPUT:
        for (size_t i = 0; i < inst->as.output_count; i++) {
            putchar('.');
        }
        break;
    case BF_INPUT: putchar(','); break;
    case BF_LOOP:
        putchar('[');
        for (size_t i = 0; i < inst->as.loop.count; i++) {
            display_instruction_source(inst->as.loop.items[i]);
        }
        putchar(']');
        break;
    default: assert(0 && "UNREACHABLE: InstructionKind");
    }
}

void display_instructions_source(Instructions insts)
{
    for (size_t i = 0; i < insts.count; i++) {
        display_instruction_source(insts.items[i]);
    }
}

void display_instruction_tree(Instruction *inst, size_t depth)
{
    for (size_t i = 0; i < depth; i++) {
        printf("    ");
    }

    switch (inst->kind) {
    case BF_CHANGE_PTR:
        printf("BF_CHANGE_PTR: %+td\n", inst->as.ptr_diff);
        break;
    case BF_CHANGE_DATA:
        printf("BF_CHANGE_DATA: %+d\n", inst->as.data_diff);
        break;
    case BF_OUTPUT: printf("BF_OUTPUT: %zu\n", inst->as.output_count); break;
    case BF_INPUT: printf("BF_INPUT\n"); break;
    case BF_LOOP:
        printf("BF_LOOP:\n");
        for (size_t i = 0; i < inst->as.loop.count; i++) {
            display_instruction_tree(inst->as.loop.items[i], depth + 1);
        }
        break;
    default: assert(0 && "UNREACHABLE: InstructionKind");
    }
}

void display_instructions_tree(Instructions insts)
{
    for (size_t i = 0; i < insts.count; i++) {
        display_instruction_tree(insts.items[i], 1);
    }
}

void display_opcode(Opcode op)
{
    switch (op.kind) {
    case OP_DATA: printf("DATA %+" PRId64 "\n", op.arg); break;
    case OP_PTR: printf("PTR %+" PRId64 "\n", op.arg); break;
    case OP_OUT: printf("OUT %" PRId64 "\n", op.arg); break;
    case OP_IN: printf("IN\n"); break;
    case OP_JZ: printf("JZ %" PRId64 "\n", op.arg); break;
    case OP_JNZ: printf("JNZ %" PRId64 "\n", op.arg); break;
    case OP_CLR: printf("CLR\n"); break;
    case OP_MOVEADD: printf("MOVEADD %+" PRId64 "\n", op.arg); break;
    case OP_SCAN: printf("SCAN %+" PRId64 "\n", op.arg); break;
    default: assert(0 && "UNREACHABLE: OpcodeKind");
    }
}

void display_program(Program p)
{
    for (size_t i = 0; i < p.count; i++) {
        printf("    %zu ", i);
        display_opcode(p.items[i]);
    }
}

char lexer_current_char(Lexer *l)
{
    if (l->cur.pos >= l->count) return '\0';
    return l->source[l->cur.pos];
}

char lexer_next_char(Lexer *l)
{
    if (l->cur.pos >= l->count) return 0;

    char c = l->source[l->cur.pos++];
    if (c == '\n') {
        l->cur.row += 1;
        l->cur.bol = l->cur.pos;
    }

    return c;
}

bool lexer_next(Lexer *l)
{
    while (true) {
        while (isspace(lexer_current_char(l))) {
            lexer_next_char(l);
        }

        char c = lexer_next_char(l);

        if (c == '\0') {
            l->token = TOKEN_END;
            return false;
        }

        switch (c) {
        case '>': l->token = TOKEN_RARROW; return true;
        case '<': l->token = TOKEN_LARROW; return true;
        case '+': l->token = TOKEN_PLUS; return true;
        case '-': l->token = TOKEN_MINUS; return true;
        case '.': l->token = TOKEN_DOT; return true;
        case ',': l->token = TOKEN_COMMA; return true;
        case '[': l->token = TOKEN_OBRACKET; return true;
        case ']': l->token = TOKEN_CBRACKET; return true;
        // Ignore unknown chars
        default: break;
        }
    }
}

bool lexer_peek(Lexer *l, TokenKind *out)
{
    Lexer saved = *l;
    bool ok = lexer_next(l);
    if (ok) {
        *out = l->token;
    }
    *l = saved;
    return ok;
}

size_t lexer_forward_count(Lexer *l, TokenKind expected)
{
    size_t count = 0;
    TokenKind peeked;
    while (lexer_peek(l, &peeked) && peeked == expected) {
        lexer_next(l);
        count++;
    }
    return count;
}

Instructions invalid_instructions() { return (Instructions){.valid = false}; }
Instructions empty_instructions() { return (Instructions){.valid = true}; }

Instructions parse_instructions(Lexer *l, TokenKind stop_token)
{
    Instructions insts = empty_instructions();

    while (true) {
        if (!lexer_next(l)) {
            insts.valid = stop_token == TOKEN_END;
            return insts;
        }

        if (l->token == stop_token) {
            return insts;
        }

        Instruction *inst;
        switch (l->token) {
        case TOKEN_RARROW: {
            inst = change_ptr(lexer_forward_count(l, TOKEN_RARROW) + 1);
        } break;
        case TOKEN_LARROW: {
            inst = change_ptr(-(lexer_forward_count(l, TOKEN_LARROW) + 1));
        } break;
        case TOKEN_PLUS: {
            inst = change_data(lexer_forward_count(l, TOKEN_PLUS) + 1);
        } break;
        case TOKEN_MINUS: {
            inst = change_data(-(lexer_forward_count(l, TOKEN_MINUS) + 1));
        } break;
        case TOKEN_DOT:
            inst = output(lexer_forward_count(l, TOKEN_DOT) + 1);
            break;
        case TOKEN_COMMA: {
            inst = input();
        } break;
        case TOKEN_OBRACKET: {
            Cursor bracket_cur = l->cur;
            Instructions body = parse_instructions(l, TOKEN_CBRACKET);
            if (l->token != TOKEN_CBRACKET) {
                fprintf(stderr,
                        "ERROR: Unmatched '[' at line %zu, column %zu: "
                        "expected ']' but got ",
                        bracket_cur.row + 1,
                        bracket_cur.pos - bracket_cur.bol);
                display_token(l->token);
                return invalid_instructions();
            }
            inst = loop(body);
        } break;
        case TOKEN_CBRACKET: {
            fprintf(stderr,
                    "ERROR: Unmatched ']' at line %zu, column %zu\n",
                    l->cur.row + 1,
                    l->cur.pos - l->cur.bol);
            return invalid_instructions();
        }
        default:
            fprintf(stderr,
                    "ERROR: Unexpected token at line %zu, column %zu: ",
                    l->cur.row + 1,
                    l->cur.pos - l->cur.bol);
            display_token(l->token);
            return invalid_instructions();
        }

        da_append(&insts, &inst);
    }
}

Opcode opcode(OpcodeKind kind, int64_t arg)
{
    return (Opcode){.kind = kind, .arg = arg};
}

void compile_instructions_into(Program *p, Instructions *insts);

Opcode detect_clear(Instructions *body)
{
    // Pattern: [-] or [+]
    if (body->count == 1 && body->items[0]->kind == BF_CHANGE_DATA &&
        (body->items[0]->as.data_diff == -1 ||
         body->items[0]->as.data_diff == 1)) {
        return opcode(OP_CLR, 0);
    }

    return opcode(OP_INVALID, 0);
}

Opcode detect_moveadd(Instructions *body)
{
    // Matches any pattern like so:
    // [->+<] or [>+<-] - With any balanced number of > and < or < and > for
    // negative offset.

    // Always 4 instructions because we compress repeated instructions
    if (body->count != 4) {
        return opcode(OP_INVALID, 0);
    }

    // ptr tracks our current position relative to the origin cell.
    // it must return to 0 by the end, meaning the ptr moves are balanced.
    ptrdiff_t ptr = 0;
    ptrdiff_t offset = 0;
    bool saw_decrement = false;
    bool saw_increment = false;

    for (size_t i = 0; i < body->count; i++) {
        Instruction *inst = body->items[i];

        switch (inst->kind) {
        case BF_CHANGE_PTR: {
            ptr += inst->as.ptr_diff;
        } break;
        case BF_CHANGE_DATA: {
            if (ptr == 0) {
                // we're on the origin cell, must be the loop counter decrement
                if (inst->as.data_diff != -1) return opcode(OP_INVALID, 0);
                if (saw_decrement) return opcode(OP_INVALID, 0);
                saw_decrement = true;
            } else {
                // we're on the target cell, must be a single increment
                if (inst->as.data_diff != 1) return opcode(OP_INVALID, 0);
                if (saw_increment) return opcode(OP_INVALID, 0);
                saw_increment = true;
                // record the target cell's offset
                offset = ptr;
            }
        } break;
        default: return opcode(OP_INVALID, 0);
        }
    }

    if (!saw_decrement || !saw_increment || ptr != 0) {
        return opcode(OP_INVALID, 0);
    }

    return opcode(OP_MOVEADD, offset);
}

Opcode detect_scan(Instructions *body)
{
    // Detects patterns like [>] or [<] with any amount of < or > inside
    if (body->count == 1 && body->items[0]->kind == BF_CHANGE_PTR) {
        return opcode(OP_SCAN, body->items[0]->as.ptr_diff);
    }

    return opcode(OP_INVALID, 0);
}

typedef Opcode (*OptimizationDetector)(Instructions *body);

void compile_loop(Program *p, Instructions *body)
{
    static const OptimizationDetector detectors[] = {
        detect_clear,
        detect_moveadd,
        detect_scan,
        NULL,
    };

    for (size_t i = 0; detectors[i] != NULL; i++) {
        Opcode op = detectors[i](body);
        if (op.kind != OP_INVALID) {
            da_append(p, &op);
            return;
        }
    }

    // Simple comment loop optimization (don't emit loop if it's the first
    // instruction of the program)
    if (p->count > 0) {
        size_t jz_pos = p->count;
        Opcode op_loop_start = opcode(OP_JZ, 0);
        da_append(p, &op_loop_start);

        compile_instructions_into(p, body);

        size_t jnz_pos = p->count;
        Opcode op_loop_end = opcode(OP_JNZ, (int64_t)(jz_pos + 1));
        da_append(p, &op_loop_end);

        // Set JZ address after recursively expanding the loop
        p->items[jz_pos].arg = (int64_t)(jnz_pos + 1);
    }
}

void compile_instructions_into(Program *p, Instructions *insts)
{
    for (size_t i = 0; i < insts->count; i++) {
        Instruction *inst = insts->items[i];

        switch (inst->kind) {
        case BF_CHANGE_PTR: {
            Opcode op = opcode(OP_PTR, inst->as.ptr_diff);
            da_append(p, &op);
        } break;
        case BF_CHANGE_DATA: {
            Opcode op = opcode(OP_DATA, inst->as.data_diff);
            da_append(p, &op);
        } break;
        case BF_OUTPUT: {
            Opcode op = opcode(OP_OUT, inst->as.output_count);
            da_append(p, &op);
        } break;
        case BF_INPUT: {
            Opcode op = opcode(OP_IN, 0);
            da_append(p, &op);
        } break;
        case BF_LOOP: {
            compile_loop(p, &inst->as.loop);
        } break;
        default: assert(0 && "UNREACHABLE: InstructionKind");
        }
    }
}

Program compile_program(Instructions insts)
{
    Program p = {0};
    compile_instructions_into(&p, &insts);
    return p;
}

#ifdef PROFILE
static size_t opcode_counts[OP_MAX] = {0};
static const char *opcode_names[OP_MAX] = {
    "OP_INVALID",
    "OP_PTR",
    "OP_DATA",
    "OP_OUT",
    "OP_IN",
    "OP_JZ",
    "OP_JNZ",
    "OP_CLR",
    "OP_MOVEADD",
    "OP_SCAN",
};
#endif

static inline void move_ptr(Brainfuck *bf, ptrdiff_t diff)
{
    bf->data_ptr += diff;

    if (bf->data_ptr < 0) {
        bf->data_ptr += bf->tape_size;
    } else if (bf->data_ptr >= (ptrdiff_t)bf->tape_size) {
        bf->data_ptr -= bf->tape_size;
    }
}

void run_program(Brainfuck *bf, Program p)
{
    size_t ip = 0;
    while (ip < p.count) {
        Opcode *op = &p.items[ip];

#ifdef PROFILE
        opcode_counts[op->kind]++;
#endif

        switch (op->kind) {
        case OP_PTR: {
            move_ptr(bf, (ptrdiff_t)op->arg);
            ip++;
        } break;
        case OP_DATA: {
            bf->tape[bf->data_ptr] += op->arg;
            ip++;
        } break;
        case OP_OUT: {
            for (size_t i = 0; i < (size_t)op->arg; i++) {
                putchar(bf->tape[bf->data_ptr]);
            }
            ip++;
        } break;
        case OP_IN: {
            int c = getchar();
            bf->tape[bf->data_ptr] = (c == EOF) ? 0 : (Cell)c;
            ip++;
        } break;
        case OP_JZ: {
            if (bf->tape[bf->data_ptr] == 0) {
                ip = (size_t)op->arg;
            } else {
                ip++;
            }
        } break;
        case OP_JNZ: {
            if (bf->tape[bf->data_ptr] != 0) {
                ip = (size_t)op->arg;
            } else {
                ip++;
            }
        } break;
        case OP_CLR: {
            bf->tape[bf->data_ptr] = 0;
            ip++;
        } break;
        case OP_MOVEADD: {
            bf->tape[bf->data_ptr + (ptrdiff_t)op->arg] +=
                bf->tape[bf->data_ptr];
            bf->tape[bf->data_ptr] = 0;
            ip++;
        } break;
        case OP_SCAN: {
            while (bf->tape[bf->data_ptr] != 0) {
                move_ptr(bf, (ptrdiff_t)op->arg);
            }
            ip++;
        } break;
        case OP_INVALID:
            assert(0 && "ERROR: Program contains invalid opcode");
            break;
        default: assert(0 && "UNREACHABLE: OpcodeKind");
        }
    }
}

int main(int argc, char **argv)
{
    char *program = argv[0];
    if (--argc < 1) {
        fprintf(stderr, "USAGE: %s <filename>\n", program);
        return 1;
    }

    char *source_file = argv[1];
    FILE *f = fopen(source_file, "r");
    if (!f) {
        fprintf(stderr,
                "ERROR: Failed to open %s: %s\n",
                source_file,
                strerror(errno));
        return 1;
    }

    arena = mason_arena_create(1024 * 1024);
    assert(arena != NULL);

    Buffer buf = {0};
    char chunk[4096];
    size_t read;
    while ((read = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        da_append_many(&buf, chunk, read);
    }
    fclose(f);

    if (buf.count == 0) {
        fprintf(stderr, "ERROR: Empty input file\n");
        mason_arena_destroy(arena);
        return 1;
    }

    Lexer l = {
        .source = buf.items,
        .count = buf.count,
    };
    PROFILE_START(parse_instructions);
    Instructions insts = parse_instructions(&l, TOKEN_END);
    PROFILE_END(parse_instructions);

    if (!insts.valid) {
        fprintf(stderr, "ERROR: Invalid program\n");
        mason_arena_destroy(arena);
        return 1;
    }

    if (insts.count == 0) {
        fprintf(stderr, "NOTE: Empty program\n");
    }

#ifdef TRACE_PROGRAM
    printf("Instructions tree:\n");
    display_instructions_tree(p);
    printf("\nSource reconstructed from instructions:\n");
    display_instructions_source(p);
    printf("\n");
#endif

    PROFILE_START(compile_program);
    Program p = compile_program(insts);
    PROFILE_END(compile_program);

#ifdef TRACE_PROGRAM
    printf("\nOpcodes:\n");
    display_program(p);
    printf("\n\nOutput:\n");
#endif

    Brainfuck bf = {
        .tape = mason_arena_calloc(arena, TAPE_SIZE, sizeof(*bf.tape)),
        .tape_size = TAPE_SIZE,
    };
    PROFILE_START(run_program);
    run_program(&bf, p);
    PROFILE_END(run_program);

#ifdef PROFILE
    fprintf(stderr, "\nOpcode counts:\n");
    size_t total = 0;
    for (size_t i = 0; i < OP_MAX; i++) {
        if (i == OP_INVALID) continue;
        fprintf(stderr, "    %s: %zu\n", opcode_names[i], opcode_counts[i]);
        total += opcode_counts[i];
    }
    fprintf(stderr, "Total: %zu\n", total);
#endif

    mason_arena_destroy(arena);
    return 0;
}
