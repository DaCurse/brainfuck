#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mason_arena.h"

#define TAPE_SIZE 30000
#define MAX_INSTRUCTION_COUNT 4096

static MASON_Arena *arena;

typedef enum {
    TOKEN_INVALID,
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
    Instruction **instructions;
    size_t count;
} Program;

struct Instruction {
    InstructionKind kind;
    union {
        ptrdiff_t ptr_diff;
        int8_t data_diff;
        size_t output_count;
        Program loop;
    } as;
};

typedef struct {
    size_t pos;
    size_t bol;
    size_t row;
} Cursor;

typedef struct {
    const char *source;
    size_t length;

    Cursor cur;

    TokenKind token;
} Lexer;

typedef struct {
    int8_t *tape;
    size_t tape_size;
    size_t data_ptr;
    size_t bf_ptr;
} Brainfuck;

Instruction *change_ptr(ptrdiff_t diff)
{
    Instruction *inst = mason_arena_alloc(arena, sizeof(*inst));
    inst->kind = BF_CHANGE_PTR;
    inst->as.ptr_diff = diff;
    return inst;
}

Instruction *change_data(int8_t diff)
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

    Instruction **instructions =
        mason_arena_alloc(arena, count * sizeof(*instructions));
    for (size_t i = 0; i < count; i++) {
        instructions[i] = va_arg(args, Instruction *);
    }
    va_end(args);

    Instruction *inst = mason_arena_alloc(arena, sizeof(*inst));
    inst->kind = BF_LOOP;
    inst->as.loop.instructions = instructions;
    inst->as.loop.count = count;
    return inst;
}

void display_token(TokenKind kind)
{
    switch (kind) {
    case TOKEN_INVALID:
        printf("TOKEN_INVALID\n");
        break;
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
        for (size_t i = 0; i < llabs(inst->as.ptr_diff); i++) {
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
            display_instruction(inst->as.loop.instructions[i]);
        }
        putchar(']');
        break;
    default:
        assert(0 && "UNREACHABLE: InstructionKind");
    }
}

void display_program(Program p)
{
    for (size_t i = 0; i < p.count; i++) {
        display_instruction(p.instructions[i]);
    }
}

void display_instruction_tree(Instruction *inst, size_t depth)
{
    for (size_t i = 0; i < depth; i++) {
        printf("\t");
    }

    switch (inst->kind) {
    case BF_CHANGE_PTR:
        printf("BF_CHANGE_PTR: %+lld\n", inst->as.ptr_diff);
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
            display_instruction_tree(inst->as.loop.instructions[i], depth + 1);
        }
        break;
    default:
        assert(0 && "UNREACHABLE: InstructionKind");
    }
}

void display_program_tree(Program p)
{
    for (size_t i = 0; i < p.count; i++) {
        display_instruction_tree(p.instructions[i], 0);
    }
}

char lexer_current_char(Lexer *l)
{
    if (l->cur.pos >= l->length)
        return '\0';

    return l->source[l->cur.pos];
}

char lexer_next_char(Lexer *l)
{
    if (l->cur.pos >= l->length)
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
        break;
    case '<':
        l->token = TOKEN_LARROW;
        break;
    case '+':
        l->token = TOKEN_PLUS;
        break;
    case '-':
        l->token = TOKEN_MINUS;
        break;
    case '.':
        l->token = TOKEN_DOT;
        break;
    case ',':
        l->token = TOKEN_COMMA;
        break;
    case '[':
        l->token = TOKEN_OBRACKET;
        break;
    case ']':
        l->token = TOKEN_CBRACKET;
        break;
    default:
        l->token = TOKEN_INVALID;
        return false;
    }

    return true;
}

bool lexer_expect(Lexer *l, TokenKind expected)
{
    if (!lexer_next(l))
        return false;
    return l->token == expected;
}

size_t lexer_forward_count(Lexer *l, TokenKind expected)
{
    size_t count = 0;

    Cursor saved = l->cur;
    while (lexer_expect(l, expected)) {
        count++;
        saved = l->cur;
    }
    l->cur = saved;

    return count;
}

Program invalid_program() { return (Program){NULL, 0}; }

Program parse_program(Lexer *l, TokenKind stop_token)
{
    Program p = {0};
    p.instructions = mason_arena_calloc(arena,
                                        MAX_INSTRUCTION_COUNT,
                                        sizeof(*p.instructions));

    while (lexer_next(l)) {
        if (l->token == stop_token)
            return p;

        assert(p.count < MAX_INSTRUCTION_COUNT);

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
            Program body = parse_program(l, TOKEN_CBRACKET);
            if (body.instructions == NULL) {
                fprintf(stderr, "ERROR: Failed to parse loop");
                return invalid_program();
            }
            inst = loop_from_program(body);
            break;
        default:
            // TODO: print location
            fprintf(stderr, "ERROR: Unexepcted token: ");
            display_token(l->token);
            return invalid_program();
        }

        p.instructions[p.count++] = inst;
    }

    if (l->token == stop_token)
        return p;

    return invalid_program();
}

void run_program(Brainfuck *bf, Program program)
{

    while (bf->bf_ptr < program.count) {
        Instruction *inst = program.instructions[bf->bf_ptr];

        switch (inst->kind) {
        case BF_CHANGE_PTR:
            bf->data_ptr += inst->as.ptr_diff;
            assert(bf->data_ptr < TAPE_SIZE);
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
            bf->tape[bf->data_ptr] = getchar();
            break;
        case BF_LOOP:
            while (bf->tape[bf->data_ptr] != 0) {
                size_t saved = bf->bf_ptr;
                bf->bf_ptr = 0;
                run_program(bf, inst->as.loop);
                bf->bf_ptr = saved;
            }
            break;
        default:
            assert(0 && "UNREACHABLE: InstructionKind");
        }

        bf->bf_ptr++;
    }
}

int main(void)
{
    arena = mason_arena_create(1024 * 1024);
    assert(arena != NULL);

    // Brainfuck quine by Erik Bosman
    char *program_source =
        "->++>+++>+>+>++>>+>+>+++>>+>+>++>+++>+++>+>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
        ">>>>>+>+>++>>>+++>>>>>+++>+>>>>>>>>>>>>>>>>>>>>>>+++>>>>>>>++>+++>+++>"
        "+>>+++>+++>+>+++>+>+++>+>++>+++>>>+>+>+>+>++>+++>+>+>>+++>>>>>>>+>+>>>"
        "+>+>++>+++>+++>+>>+++>+++>+>+++>+>++>+++>++>>+>+>++>+++>+>+>>+++>>>+++"
        ">+>>>++>+++>+++>+>>+++>>>+++>+>+++>+>>+++>>+++>>+[[>>+[>]+>+[<]<-]>>[>"
        "]<+<+++[<]<<+]>>>[>]+++[++++++++++>++[-<++++++++++++++++>]<.<-<]";

    Lexer l = {
        .source = program_source,
        .length = strlen(program_source),
    };
    Program p = parse_program(&l, TOKEN_END);
    if (p.instructions == NULL) {
        fprintf(stderr, "ERROR: Invalid program");
        return 1;
    }

    printf("Program tree:\n");
    display_program_tree(p);
    printf("\nProgram reconstructed from tree:\n");
    display_program(p);
    printf("\n\nOutput:\n");

    Brainfuck bf = {
        .tape = mason_arena_calloc(arena, TAPE_SIZE, sizeof(*bf.tape)),
        .tape_size = TAPE_SIZE,
    };
    run_program(&bf, p);

    mason_arena_destroy(arena);
    return 0;
}
