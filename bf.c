#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mason_arena.h"

#define TAPE_SIZE 30000
#define DA_INITIAL_CAPACITY 256

#define da_append_many(da, src, n)                                             \
    do {                                                                       \
        size_t item_size = sizeof(*(da)->items);                               \
        if ((da)->count + (n) > (da)->capacity) {                              \
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

#ifndef BRAINFUCK_BIGNUM
typedef uint8_t Cell;
#else
typedef uint16_t Cell;
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
    case TOKEN_END:
        printf("TOKEN_END\n");
        break;
    case TOKEN_RARROW:
        printf("TOKEN_RARROW\n");
        break;
    case TOKEN_LARROW:
        printf("TOKEN_LARROW\n");
        break;
    case TOKEN_PLUS:
        printf("TOKEN_PLUS\n");
        break;
    case TOKEN_MINUS:
        printf("TOKEN_MINUS\n");
        break;
    case TOKEN_DOT:
        printf("TOKEN_DOT\n");
        break;
    case TOKEN_COMMA:
        printf("TOKEN_COMMA\n");
        break;
    case TOKEN_OBRACKET:
        printf("TOKEN_OBRACKET\n");
        break;
    case TOKEN_CBRACKET:
        printf("TOKEN_CBRACKET\n");
        break;
    default:
        assert(0 && "UNREACHABLE: TokenKind");
    }
}

void display_instruction(Instruction *inst)
{
    switch (inst->kind) {
    case BF_CHANGE_PTR: {
        char inst_repr = inst->as.ptr_diff > 0 ? '>' : '<';
        ptrdiff_t abs_diff = inst->as.ptr_diff < 0 ? -inst->as.ptr_diff : inst->as.ptr_diff;
        for (size_t i = 0; i < (size_t)abs_diff; i++) {
            putchar(inst_repr);
        }
    } break;
    case BF_CHANGE_DATA: {
        char inst_repr = inst->as.data_diff > 0 ? '+' : '-';
        for (size_t i = 0; i < abs(inst->as.data_diff); i++) {
            putchar(inst_repr);
        }
    } break;
    case BF_OUTPUT:
        for (size_t i = 0; i < inst->as.output_count; i++) {
            putchar('.');
        }
        break;
    case BF_INPUT:
        putchar(',');
        break;
    case BF_LOOP:
        putchar('[');
        for (size_t i = 0; i < inst->as.loop.count; i++) {
            display_instruction(inst->as.loop.items[i]);
        }
        putchar(']');
        break;
    default:
        assert(0 && "UNREACHABLE: InstructionKind");
    }
}

void display_instruction_tree(Instruction *inst, size_t depth)
{
    for (size_t i = 0; i < depth; i++) {
        printf("\t");
    }

    switch (inst->kind) {
    case BF_CHANGE_PTR:
        printf("BF_CHANGE_PTR: %+td\n", inst->as.ptr_diff);
        break;
    case BF_CHANGE_DATA:
        printf("BF_CHANGE_DATA: %+d\n", inst->as.data_diff);
        break;
    case BF_OUTPUT:
        printf("BF_OUTPUT: %zu\n", inst->as.output_count);
        break;
    case BF_INPUT:
        printf("BF_INPUT\n");
        break;
    case BF_LOOP:
        printf("BF_LOOP:\n");
        for (size_t i = 0; i < inst->as.loop.count; i++) {
            display_instruction_tree(inst->as.loop.items[i], depth + 1);
        }
        break;
    default:
        assert(0 && "UNREACHABLE: InstructionKind");
    }
}

void display_program_tree(Program p)
{
    for (size_t i = 0; i < p.count; i++) {
        display_instruction_tree(p.items[i], 0);
    }
}

char lexer_current_char(Lexer *l)
{
    if (l->cur.pos >= l->count)
        return '\0';

    return l->source[l->cur.pos];
}

char lexer_next_char(Lexer *l)
{
    if (l->cur.pos >= l->count)
        return 0;

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
        case '>':
            l->token = TOKEN_RARROW;
            return true;
        case '<':
            l->token = TOKEN_LARROW;
            return true;
        case '+':
            l->token = TOKEN_PLUS;
            return true;
        case '-':
            l->token = TOKEN_MINUS;
            return true;
        case '.':
            l->token = TOKEN_DOT;
            return true;
        case ',':
            l->token = TOKEN_COMMA;
            return true;
        case '[':
            l->token = TOKEN_OBRACKET;
            return true;
        case ']':
            l->token = TOKEN_CBRACKET;
            return true;

        default:
            // Ignore unknown chars
            break;
        }
    }
}

bool lexer_peek(Lexer *l, TokenKind *out) {
    Lexer saved = *l;
    bool ok = lexer_next(l);
    if (ok) *out = l->token;
    *l = saved;
    return ok;
}

size_t lexer_forward_count(Lexer *l, TokenKind expected) {
    size_t count = 0;
    TokenKind peeked;
    while (lexer_peek(l, &peeked) && peeked == expected) {
        lexer_next(l);
        count++;
    }
    return count;
}

Program invalid_program() { return (Program){ .valid = false }; }
Program empty_program()   { return (Program){ .valid = true  }; }

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
        case TOKEN_RARROW:
            inst = change_ptr(lexer_forward_count(l, TOKEN_RARROW) + 1);
            break;
        case TOKEN_LARROW:
            inst = change_ptr(-(lexer_forward_count(l, TOKEN_LARROW) + 1));
            break;
        case TOKEN_PLUS:
            inst = change_data(lexer_forward_count(l, TOKEN_PLUS) + 1);
            break;
        case TOKEN_MINUS:
            inst = change_data(-(lexer_forward_count(l, TOKEN_MINUS) + 1));
            break;
        case TOKEN_DOT:
            inst = output(lexer_forward_count(l, TOKEN_DOT) + 1);
            break;
        case TOKEN_COMMA:
            inst = input();
            break;

        case TOKEN_OBRACKET:
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
            break;

        case TOKEN_CBRACKET:
            fprintf(stderr,
                    "ERROR: Unmatched ']' at line %zu, column %zu\n",
                    l->cur.row + 1,
                    l->cur.pos - l->cur.bol);
            return invalid_program();

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

void run_program(Brainfuck *bf, Program program)
{
    size_t ip = 0;

    while (ip < program.count) {
        Instruction *inst = program.items[ip];

        switch (inst->kind) {
        case BF_CHANGE_PTR:
            bf->data_ptr += inst->as.ptr_diff;
            ptrdiff_t tape_size = (ptrdiff_t)bf->tape_size;
            bf->data_ptr =
                (bf->data_ptr % tape_size + tape_size) % tape_size;
            break;
        case BF_CHANGE_DATA:
            bf->tape[bf->data_ptr] += inst->as.data_diff;
            break;
        case BF_OUTPUT:
            for (size_t i = 0; i < inst->as.output_count; i++) {
                putchar(bf->tape[bf->data_ptr]);
            }
            break;
        case BF_INPUT:
            int c = getchar();
            bf->tape[bf->data_ptr] = (c == EOF) ? 0 : (Cell)c;
            break;
        case BF_LOOP:
            while (bf->tape[bf->data_ptr] != 0) {
                run_program(bf, inst->as.loop);
            }
            break;
        default:
            assert(0 && "UNREACHABLE: InstructionKind");
        }

        ip++;
    }
}

int main(int argc, char **argv)
{
    char *program = argv[0];
    if (--argc < 1) {
        fprintf(stderr, "USAGE: %s <filename>", program);
        return 1;
    }

    char *source_file = argv[1];
    FILE *f = fopen(source_file, "r");
    if (!f) {
        fprintf(stderr,
                "ERROR: Failed to open %s: %s",
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
    Program p = parse_program(&l, TOKEN_END);
    if (!p.valid) {
        fprintf(stderr, "ERROR: Invalid program\n");
        mason_arena_destroy(arena);
        return 1;
    }

    if(p.count == 0) {
        fprintf(stderr, "NOTE: Empty program\n");
    }

#ifdef TRACE_PROGRAM
    printf("Program tree:\n");
    display_program_tree(p);
    printf("\nProgram reconstructed from tree:\n");
    display_program(p);
    printf("\n\nOutput:\n");
#endif

    Brainfuck bf = {
        .tape = mason_arena_calloc(arena, TAPE_SIZE, sizeof(*bf.tape)),
        .tape_size = TAPE_SIZE,
    };
    run_program(&bf, p);

    mason_arena_destroy(arena);
    return 0;
}
