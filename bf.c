#include <assert.h>
#include <ctype.h>
#include <errno.h>
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
        size_t item_size = sizeof(*(da)->items);                               \
        if ((da)->capacity - (da)->count < (n)) {                              \
            size_t new_capacity = (da)->capacity == 0 ? DA_INITIAL_CAPACITY    \
                                                      : (da)->capacity * 2;    \
            while (new_capacity < (da)->count + (n)) {                         \
                new_capacity *= 2;                                             \
            }                                                                  \
            (da)->items = mason_arena_realloc(arena,                           \
                                              (da)->items,                     \
                                              (da)->capacity * item_size,      \
                                              new_capacity * item_size);       \
            assert((da)->items != NULL);                                       \
            (da)->capacity = new_capacity;                                     \
        }                                                                      \
        memcpy((da)->items + (da)->count, (src), (n) * item_size);             \
        (da)->count += (n);                                                    \
    } while (0);

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
} Program;

struct Instruction {
    InstructionKind kind;
    union {
        ptrdiff_t ptr_diff;
        int32_t data_diff;
        size_t output_count;
        Program loop;
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
    OP_PTR,
    OP_DATA,
    OP_OUT,
    OP_IN,
    OP_JZ,
    OP_JNZ,
} OpcodeKind;

typedef struct {
    OpcodeKind kind;
    int64_t arg;
} Opcode;

typedef struct {
    Opcode *items;
    size_t count;
    size_t capacity;
} Opcodes;

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

Instruction *loop_from_program(Program body)
{
    Instruction *inst = mason_arena_alloc(arena, sizeof(*inst));
    inst->kind = BF_LOOP;
    inst->as.loop = body;
    return inst;
}

Instruction *loop(size_t count, ...)
{
    va_list args;
    va_start(args, count);

    Program p = {0};
    for (size_t i = 0; i < count; i++) {
        Instruction *inst = va_arg(args, Instruction *);
        da_append_many(&p, &inst, 1);
    }
    va_end(args);

    Instruction *inst = mason_arena_alloc(arena, sizeof(*inst));
    inst->kind = BF_LOOP;
    inst->as.loop = p;
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

void display_instruction(Instruction *inst)
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
            display_instruction(inst->as.loop.items[i]);
        }
        putchar(']');
        break;
    default: assert(0 && "UNREACHABLE: InstructionKind");
    }
}

void display_program(Program p)
{
    for (size_t i = 0; i < p.count; i++) {
        display_instruction(p.items[i]);
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

void display_program_tree(Program p)
{
    for (size_t i = 0; i < p.count; i++) {
        display_instruction_tree(p.items[i], 1);
    }
}

void display_opcode(Opcode op)
{
    switch (op.kind) {
    case OP_DATA:
        printf("DATA %+lld\n", op.arg);
        break;
    case OP_PTR:
        printf("PTR %+lld\n", op.arg);
        break;
    case OP_OUT:
        printf("OUT %lld\n", op.arg);
        break;
    case OP_IN:
        printf("IN\n");
        break;
    case OP_JZ:
        printf("JZ %lld\n", op.arg);
        break;
    case OP_JNZ:
        printf("JNZ %lld\n", op.arg);
        break;
    default:
        assert(0 && "UNREACHABLE: OpcodeKind");
    }
}

void display_opcodes(Opcodes ops)
{
    for (size_t i = 0; i < ops.count; i++) {
        printf("    %08lld ", i);
        display_opcode(ops.items[i]);
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

Program invalid_program() { return (Program){.valid = false}; }
Program empty_program() { return (Program){.valid = true}; }

Program parse_program(Lexer *l, TokenKind stop_token)
{
    Program p = empty_program();

    while (true) {
        if (!lexer_next(l)) {
            p.valid = stop_token == TOKEN_END;
            return p;
        }

        if (l->token == stop_token) {
            return p;
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
            Program body = parse_program(l, TOKEN_CBRACKET);
            if (l->token != TOKEN_CBRACKET) {
                fprintf(stderr,
                        "ERROR: Unmatched '[' at line %zu, column %zu: "
                        "expected ']' but got ",
                        bracket_cur.row + 1,
                        bracket_cur.pos - bracket_cur.bol);
                display_token(l->token);
                return invalid_program();
            }
            inst = loop_from_program(body);
        } break;
        case TOKEN_CBRACKET: {
            fprintf(stderr,
                    "ERROR: Unmatched ']' at line %zu, column %zu\n",
                    l->cur.row + 1,
                    l->cur.pos - l->cur.bol);
            return invalid_program();
        }
        default:
            fprintf(stderr,
                    "ERROR: Unexpected token at line %zu, column %zu: ",
                    l->cur.row + 1,
                    l->cur.pos - l->cur.bol);
            display_token(l->token);
            return invalid_program();
        }

        da_append_many(&p, &inst, 1);
    }
}

Opcode opcode(OpcodeKind kind, int64_t arg)
{
    return (Opcode){.kind = kind, .arg = arg};
}

void compile_program_into(Opcodes *ops, Program p)
{
    for (size_t i = 0; i < p.count; i++) {
        Instruction *inst = p.items[i];

        switch (inst->kind) {
        case BF_CHANGE_PTR: {
            Opcode op = opcode(OP_PTR, inst->as.ptr_diff);
            da_append_many(ops, &op, 1);
        } break;
        case BF_CHANGE_DATA: {
            Opcode op = opcode(OP_DATA, inst->as.data_diff);
            da_append_many(ops, &op, 1);
        } break;
        case BF_OUTPUT: {
            Opcode op = opcode(OP_OUT, inst->as.output_count);
            da_append_many(ops, &op, 1);
        } break;
        case BF_INPUT: {
            Opcode op = opcode(OP_IN, 0);
            da_append_many(ops, &op, 1);
        } break;
        case BF_LOOP: {
            size_t jz_pos = ops->count;
            Opcode op_loop_start = opcode(OP_JZ, 0);
            da_append_many(ops, &op_loop_start, 1);

            compile_program_into(ops, inst->as.loop);

            size_t jnz_pos = ops->count;
            Opcode op_loop_end = opcode(OP_JNZ, (int64_t)(jz_pos + 1));
            da_append_many(ops, &op_loop_end, 1);

            // Set JZ address after recursively expanding the loop
            ops->items[jz_pos].arg = (int64_t)(jnz_pos + 1);
        } break;
        default:
            assert(0 && "UNREACHABLE: InstructionKind");
        }
    }
}

Opcodes compile_program(Program p)
{
    Opcodes ops = {0};
    compile_program_into(&ops, p);
    return ops;
}

void run_program(Brainfuck *bf, Opcodes ops)
{
    size_t ip = 0;

    while (ip < ops.count) {
        Opcode *op = &ops.items[ip];

        switch (op->kind) {
        case OP_PTR: {
            bf->data_ptr += (ptrdiff_t)op->arg;

            if (bf->data_ptr < 0) {
                bf->data_ptr += bf->tape_size;
            } else if (bf->data_ptr >= (ptrdiff_t)bf->tape_size) {
                bf->data_ptr -= bf->tape_size;
            }

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
        default:
            assert(0 && "UNREACHABLE: OpcodeKind");
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

    PROFILE_START(parse_program);
    Program p = parse_program(&l, TOKEN_END);
    PROFILE_END(parse_program);

    if (!p.valid) {
        fprintf(stderr, "ERROR: Invalid program\n");
        mason_arena_destroy(arena);
        return 1;
    }

    if (p.count == 0) {
        fprintf(stderr, "NOTE: Empty program\n");
    }

#ifdef TRACE_PROGRAM
    printf("Program tree:\n");
    display_program_tree(p);
    printf("\nProgram reconstructed from tree:\n");
    display_program(p);
#endif

    PROFILE_START(compile_program);
    Opcodes ops = compile_program(p);
    PROFILE_END(compile_program);

#ifdef TRACE_PROGRAM
    printf("\nOpcodes:\n");
    display_opcodes(ops);
    printf("\n\nOutput:\n");
#endif

    Brainfuck bf = {
        .tape = mason_arena_calloc(arena, TAPE_SIZE, sizeof(*bf.tape)),
        .tape_size = TAPE_SIZE,
    };

    PROFILE_START(run_program);
    run_program(&bf, ops);
    PROFILE_END(run_program);

    mason_arena_destroy(arena);
    return 0;
}
