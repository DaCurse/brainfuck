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
            _da->items = mason_arena_realloc(g_arena,                          \
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

static inline double timer_elapsed(const struct timespec *t)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double ms = (double)(now.tv_sec - t->tv_sec) * 1e3;
    ms += (double)(now.tv_nsec - t->tv_nsec) / 1e6;
    return ms;
}

#define PROFILE_START(name)                                                    \
    struct timespec _timer_##name;                                             \
    if (g_profile_enabled) {                                                   \
        clock_gettime(CLOCK_MONOTONIC, &_timer_##name);                        \
    }

#define PROFILE_END(name)                                                      \
    if (g_profile_enabled) {                                                   \
        fprintf(g_profile_out,                                                 \
                "PROFILE: " #name " took %.3fms\n",                            \
                timer_elapsed(&_timer_##name));                                \
    }

static bool g_profile_enabled = false;
static FILE *g_profile_out = NULL;
static MASON_Arena *g_arena = NULL;

// Profiling stats
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
static ptrdiff_t data_ptr_min = 0;
static ptrdiff_t data_ptr_max = 0;


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
    INST_MOVE_PTR,
    INST_CHANGE_DATA,
    INST_OUTPUT,
    INST_INPUT,
    INST_LOOP
} InstructionKind;

typedef struct Instruction Instruction;

typedef struct {
    bool valid;
    Instruction *items;
    size_t count;
    size_t capacity;
} Instructions;

struct Instruction {
    InstructionKind kind;
    union {
        ptrdiff_t ptr_diff;
        int64_t data_diff;
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
    OP_PTR,  // PTR diff
    OP_DATA, // DATA diff
    OP_OUT,  // OUT cnt
    OP_IN,   // IN
    OP_JZ,   // JZ addr
    OP_JNZ,  // JNZ addr

    // Optimizations
    OP_CLR,     // CLR
    OP_MOVEADD, // MOVEADD offset, mul
    OP_SCAN,    // SCAN step

    OP_MAX
} OpcodeKind;

typedef struct {
    OpcodeKind kind;
    union {
        int64_t i64;
        int32_t i32[2];
    } arg;
} Opcode;

typedef struct {
    const char *source_file;

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

Instruction move_ptr(ptrdiff_t diff)
{
    return (Instruction){
        .kind = INST_MOVE_PTR,
        .as.ptr_diff = diff,
    };
}

Instruction change_data(int64_t diff)
{
    return (Instruction){
        .kind = INST_CHANGE_DATA,
        .as.data_diff = diff,
    };
}

Instruction output(size_t count)
{
    return (Instruction){
        .kind = INST_OUTPUT,
        .as.output_count = count,
    };
}

Instruction input()
{
    return (Instruction){
        .kind = INST_INPUT,
    };
}

Instruction loop(Instructions body)
{
    return (Instruction){
        .kind = INST_LOOP,
        .as.loop = body,
    };
}

void display_token(TokenKind kind)
{
    switch (kind) {
    case TOKEN_END: printf("TOKEN_END\n"); return;
    case TOKEN_RARROW: printf("TOKEN_RARROW\n"); return;
    case TOKEN_LARROW: printf("TOKEN_LARROW\n"); return;
    case TOKEN_PLUS: printf("TOKEN_PLUS\n"); return;
    case TOKEN_MINUS: printf("TOKEN_MINUS\n"); return;
    case TOKEN_DOT: printf("TOKEN_DOT\n"); return;
    case TOKEN_COMMA: printf("TOKEN_COMMA\n"); return;
    case TOKEN_OBRACKET: printf("TOKEN_OBRACKET\n"); return;
    case TOKEN_CBRACKET: printf("TOKEN_CBRACKET\n"); return;
    }
    assert(0 && "UNREACHABLE: TokenKind");
}

void buffer_appendf(Buffer *buf, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);
    assert(needed >= 0 && "ERROR: Failed to format output string");

    if (needed > 0) {
        size_t n = (size_t)needed;
        char *tmp = mason_arena_alloc(g_arena, n + 1);
        assert(tmp != NULL);
        int written = vsnprintf(tmp, n + 1, fmt, args);
        assert(written == needed && "ERROR: Unexpected formatted size");
        da_append_many(buf, tmp, n);
    }

    va_end(args);
}

bool buffer_write_to_file(Buffer buf, const char *path)
{
    FILE *out = fopen(path, "w");
    if (!out) {
        fprintf(stderr,
                "ERROR: Failed to open %s: %s\n",
                path,
                strerror(errno));
        return false;
    }

    if (buf.count > 0) {
        size_t written = fwrite(buf.items, 1, buf.count, out);
        if (written != buf.count) {
            fclose(out);
            fprintf(stderr, "ERROR: Failed to write all bytes to %s\n", path);
            return false;
        }
    }

    fclose(out);
    return true;
}

void display_instruction_tree(Buffer *out, Instruction *inst, size_t depth)
{
    for (size_t i = 0; i < depth; i++) {
        buffer_appendf(out, "    ");
    }

    switch (inst->kind) {
    case INST_MOVE_PTR: {
        buffer_appendf(out, "INST_MOVE_PTR: %+td\n", inst->as.ptr_diff);
    }
        return;
    case INST_CHANGE_DATA: {
        buffer_appendf(out,
                       "INST_CHANGE_DATA: %+" PRId64 "\n",
                       inst->as.data_diff);
    }
        return;
    case INST_OUTPUT: {
        buffer_appendf(out, "INST_OUTPUT: %zu\n", inst->as.output_count);
    }
        return;
    case INST_INPUT: {
        buffer_appendf(out, "INST_INPUT\n");
    }
        return;
    case INST_LOOP: {
        buffer_appendf(out, "INST_LOOP:\n");
        for (size_t i = 0; i < inst->as.loop.count; i++) {
            display_instruction_tree(out, &inst->as.loop.items[i], depth + 1);
        }
    }
        return;
    }
    assert(0 && "UNREACHABLE: InstructionKind");
}

void display_instructions_tree(Buffer *out, Instructions insts)
{
    for (size_t i = 0; i < insts.count; i++) {
        display_instruction_tree(out, &insts.items[i], 1);
    }
}

void display_opcode(Buffer *out, Opcode op)
{
    switch (op.kind) {
    case OP_DATA:
        buffer_appendf(out, "DATA %+" PRId64 "\n", op.arg.i64);
        return;
    case OP_PTR: buffer_appendf(out, "PTR %+" PRId64 "\n", op.arg.i64); return;
    case OP_OUT: buffer_appendf(out, "OUT %" PRId64 "\n", op.arg.i64); return;
    case OP_IN: buffer_appendf(out, "IN\n"); return;
    case OP_JZ: buffer_appendf(out, "JZ %" PRId64 "\n", op.arg.i64); return;
    case OP_JNZ: buffer_appendf(out, "JNZ %" PRId64 "\n", op.arg.i64); return;
    case OP_CLR: buffer_appendf(out, "CLR\n"); return;
    case OP_MOVEADD:
        buffer_appendf(out, "MOVEADD %+d, %+d\n", op.arg.i32[0], op.arg.i32[1]);
        return;
    case OP_SCAN:
        buffer_appendf(out, "SCAN %+" PRId64 "\n", op.arg.i64);
        return;

    case OP_INVALID:
    case OP_MAX: {
        assert(0 && "ERROR: Invalid opcode");
    }
    }
    assert(0 && "UNREACHABLE: OpcodeKind");
}

void display_program(Buffer *out, Program p)
{
    for (size_t i = 0; i < p.count; i++) {
        buffer_appendf(out, "%08zu ", i);
        display_opcode(out, p.items[i]);
    }
}

Buffer instruction_tree_output(Instructions insts)
{
    Buffer out = {0};
    buffer_appendf(&out, "Instructions tree:\n");
    display_instructions_tree(&out, insts);
    return out;
}

Buffer bytecode_output(Program p)
{
    Buffer out = {0};
    buffer_appendf(&out, "; %s\n", p.source_file);
    display_program(&out, p);
    return out;
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

        switch (l->token) {
        case TOKEN_RARROW: {
            Instruction inst =
                move_ptr(lexer_forward_count(l, TOKEN_RARROW) + 1);
            da_append(&insts, &inst);
        }
            continue;
        case TOKEN_LARROW: {
            Instruction inst =
                move_ptr(-(lexer_forward_count(l, TOKEN_LARROW) + 1));
            da_append(&insts, &inst);
        }
            continue;
        case TOKEN_PLUS: {
            Instruction inst =
                change_data(lexer_forward_count(l, TOKEN_PLUS) + 1);
            da_append(&insts, &inst);
        }
            continue;
        case TOKEN_MINUS: {
            Instruction inst =
                change_data(-(lexer_forward_count(l, TOKEN_MINUS) + 1));
            da_append(&insts, &inst);
        }
            continue;
        case TOKEN_DOT: {
            Instruction inst = output(lexer_forward_count(l, TOKEN_DOT) + 1);
            da_append(&insts, &inst);
        }
            continue;
        case TOKEN_COMMA: {
            Instruction inst = input();
            da_append(&insts, &inst);
        }
            continue;
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
            Instruction inst = loop(body);
            da_append(&insts, &inst);
        }
            continue;
        case TOKEN_CBRACKET: {
            fprintf(stderr,
                    "ERROR: Unmatched ']' at line %zu, column %zu\n",
                    l->cur.row + 1,
                    l->cur.pos - l->cur.bol);
            return invalid_instructions();
        }
        case TOKEN_END: assert(0 && "UNREACHABLE: Parser loop didn't break");
        }
        assert(0 && "UNREACHABLE: TokenKind");
    }
}

Opcode invalid_opcode() { return (Opcode){.kind = OP_INVALID}; }

Opcode opcode(OpcodeKind kind, int64_t arg)
{
    return (Opcode){.kind = kind, .arg.i64 = arg};
}

Opcode opcode2(OpcodeKind kind, int32_t a, int32_t b)
{
    return (Opcode){.kind = kind, .arg.i32 = {a, b}};
}

void compile_instructions_into(Program *p, Instructions *insts);

Opcode detect_clear(Instructions *body)
{
    // Pattern: [-] or [+]
    if (body->count == 1 && body->items[0].kind == INST_CHANGE_DATA &&
        (body->items[0].as.data_diff == -1 ||
         body->items[0].as.data_diff == 1)) {
        return opcode(OP_CLR, 0);
    }

    return invalid_opcode();
}

Opcode detect_moveadd(Instructions *body)
{
    // Matches any pattern like so:
    // [->+<] or [>+<-] - With any balanced number of > and < or < and > for
    // negative offset.

    // Always 4 instructions because we compress repeated instructions
    if (body->count != 4) {
        return invalid_opcode();
    }

    // ptr tracks our current position relative to the origin cell.
    // it must return to 0 by the end, meaning the ptr moves are balanced.
    ptrdiff_t ptr = 0;
    int32_t offset = 0;
    int32_t mul = 0;
    bool saw_decrement = false;
    bool saw_increment = false;

    for (size_t i = 0; i < body->count; i++) {
        Instruction *inst = &body->items[i];

        switch (inst->kind) {
        case INST_MOVE_PTR: {
            ptr += inst->as.ptr_diff;
        } break;
        case INST_CHANGE_DATA: {
            if (ptr == 0) {
                // we're on the origin cell, must be the loop counter decrement
                if (inst->as.data_diff != -1) return invalid_opcode();
                if (saw_decrement) return invalid_opcode();
                saw_decrement = true;
            } else {
                // we're on the target cell, the data diff is the multiplier
                mul = (int32_t)inst->as.data_diff;
                if (saw_increment) return invalid_opcode();
                saw_increment = true;
                // record the target cell's offset
                offset = (int32_t)ptr;
            }
        } break;
        default: return invalid_opcode();
        }
    }

    if (!saw_decrement || !saw_increment || ptr != 0) {
        return invalid_opcode();
    }

    return opcode2(OP_MOVEADD, offset, mul);
}

Opcode detect_scan(Instructions *body)
{
    // Detects patterns like [>] or [<] with any amount of < or > inside
    if (body->count == 1 && body->items[0].kind == INST_MOVE_PTR) {
        return opcode(OP_SCAN, body->items[0].as.ptr_diff);
    }

    return invalid_opcode();
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
        p->items[jz_pos].arg.i64 = (int64_t)(jnz_pos + 1);
    }
}

void compile_instructions_into(Program *p, Instructions *insts)
{
    for (size_t i = 0; i < insts->count; i++) {
        Instruction *inst = &insts->items[i];

        switch (inst->kind) {
        case INST_MOVE_PTR: {
            // Collapse subsequent PTR instructions
            ptrdiff_t diff = inst->as.ptr_diff;
            while (i + 1 < insts->count &&
                   insts->items[i + 1].kind == INST_MOVE_PTR) {
                diff += insts->items[++i].as.ptr_diff;
            }

            if (diff != 0) {
                Opcode op = opcode(OP_PTR, diff);
                da_append(p, &op);
            }
        }
            continue;
        case INST_CHANGE_DATA: {
            // Collapse subsequent DATA instructions
            int64_t diff = inst->as.data_diff;
            while (i + 1 < insts->count &&
                   insts->items[i + 1].kind == INST_CHANGE_DATA) {
                diff += insts->items[++i].as.data_diff;
            }

            if (diff != 0) {
                Opcode op = opcode(OP_DATA, diff);
                da_append(p, &op);
            }
        }
            continue;
        case INST_OUTPUT: {
            Opcode op = opcode(OP_OUT, inst->as.output_count);
            da_append(p, &op);
        }
            continue;
        case INST_INPUT: {
            Opcode op = opcode(OP_IN, 0);
            da_append(p, &op);
        }
            continue;
        case INST_LOOP: {
            compile_loop(p, &inst->as.loop);
        }
            continue;
        }
        assert(0 && "UNREACHABLE: InstructionKind");
    }
}


static inline void bf_move_ptr(Brainfuck *bf, ptrdiff_t diff)
{
    bf->data_ptr += diff;

    if (g_profile_enabled) {
        if (bf->data_ptr < data_ptr_min) data_ptr_min = bf->data_ptr;
        if (bf->data_ptr > data_ptr_max) data_ptr_max = bf->data_ptr;
    }

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

        if (g_profile_enabled) {
            opcode_counts[op->kind]++;
        }

        switch (op->kind) {
        case OP_PTR: {
            bf_move_ptr(bf, (ptrdiff_t)op->arg.i64);
            ip++;
        }
            continue;
        case OP_DATA: {
            bf->tape[bf->data_ptr] += op->arg.i64;
            ip++;
        }
            continue;
        case OP_OUT: {
            for (size_t i = 0; i < (size_t)op->arg.i64; i++) {
                putchar(bf->tape[bf->data_ptr]);
            }
            ip++;
        }
            continue;
        case OP_IN: {
            int c = getchar();
            bf->tape[bf->data_ptr] = (c == EOF) ? 0 : (Cell)c;
            ip++;
        }
            continue;
        case OP_JZ: {
            if (bf->tape[bf->data_ptr] == 0) {
                ip = (size_t)op->arg.i64;
            } else {
                ip++;
            }
        }
            continue;
        case OP_JNZ: {
            if (bf->tape[bf->data_ptr] != 0) {
                ip = (size_t)op->arg.i64;
            } else {
                ip++;
            }
        }
            continue;
        case OP_CLR: {
            bf->tape[bf->data_ptr] = 0;
            ip++;
        }
            continue;
        case OP_MOVEADD: {
            int32_t offset = op->arg.i32[0];
            int32_t mul = op->arg.i32[1];
            bf->tape[bf->data_ptr + offset] += bf->tape[bf->data_ptr] * mul;
            bf->tape[bf->data_ptr] = 0;
            ip++;
        }
            continue;
        case OP_SCAN: {
            while (bf->tape[bf->data_ptr] != 0) {
                bf_move_ptr(bf, (ptrdiff_t)op->arg.i64);
            }
            ip++;
        }
            continue;

        case OP_INVALID:
        case OP_MAX:
            assert(0 && "ERROR: Program contains invalid opcode");
            break;
        }
        assert(0 && "UNREACHABLE: OpcodeKind");
    }
}

typedef struct {
    const char *source_file;
    const char *profile_out_path;
    const char *instruction_tree_out_path;
    const char *bytecode_out_path;
} Flags;

void print_usage(const char *program)
{
    fprintf(stderr,
            "USAGE: %s <filename> [--instruction-out <path>] "
            "[--bytecode-out <path>] [--profile-out <path>]\n",
            program);
}

int main(int argc, char **argv)
{
    Flags flags = {0};
    char *program = argc > 0 ? argv[0] : "bf";
    if (argc < 2) {
        print_usage(program);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && strcmp(argv[i], "--profile-out") == 0) {
            flags.profile_out_path = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--bytecode-out") == 0) {
            flags.bytecode_out_path = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--instruction-out") == 0) {
            flags.instruction_tree_out_path = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "ERROR: Unknown option: %s\n", argv[i]);
            print_usage(program);
            return 1;
        } else if (flags.source_file == NULL) {
            flags.source_file = argv[i];
        } else {
            fprintf(stderr, "ERROR: Multiple input files provided\n");
            print_usage(program);
            return 1;
        }
    }

    if (flags.source_file == NULL) {
        fprintf(stderr, "ERROR: Missing input file\n");
        print_usage(program);
        return 1;
    }

    FILE *profile_out = NULL;
    if (flags.profile_out_path != NULL) {
        profile_out = fopen(flags.profile_out_path, "w");
        if (!profile_out) {
            fprintf(stderr,
                    "ERROR: Failed to open %s: %s\n",
                    flags.profile_out_path,
                    strerror(errno));
            return 1;
        }
        g_profile_enabled = true;
        g_profile_out = profile_out;
    }

    char *source_file = (char *)flags.source_file;
    FILE *f = fopen(source_file, "r");
    if (!f) {
        fprintf(stderr,
                "ERROR: Failed to open %s: %s\n",
                source_file,
                strerror(errno));
        if (profile_out) fclose(profile_out);
        return 1;
    }

    g_arena = mason_arena_create(1024 * 1024);
    assert(g_arena != NULL);

    Buffer buf = {0};
    char chunk[4096];
    size_t read;
    while ((read = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        da_append_many(&buf, chunk, read);
    }
    fclose(f);

    if (buf.count == 0) {
        fprintf(stderr, "ERROR: Empty input file\n");
        if (profile_out) fclose(profile_out);
        mason_arena_destroy(g_arena);
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
        if (profile_out) fclose(profile_out);
        mason_arena_destroy(g_arena);
        return 1;
    }

    if (insts.count == 0) {
        fprintf(stderr, "NOTE: Empty program\n");
    }

    if (flags.instruction_tree_out_path != NULL) {
        Buffer out = instruction_tree_output(insts);
        if (!buffer_write_to_file(out, flags.instruction_tree_out_path)) {
            if (profile_out) fclose(profile_out);
            mason_arena_destroy(g_arena);
            return 1;
        }
    }

    Program p = {
        .source_file = source_file,
    };
    PROFILE_START(compile_program);
    compile_instructions_into(&p, &insts);
    PROFILE_END(compile_program);

    if (flags.bytecode_out_path != NULL) {
        Buffer out = bytecode_output(p);
        if (!buffer_write_to_file(out, flags.bytecode_out_path)) {
            if (profile_out) fclose(profile_out);
            mason_arena_destroy(g_arena);
            return 1;
        }
    }

    Brainfuck bf = {
        .tape = mason_arena_calloc(g_arena, TAPE_SIZE, sizeof(*bf.tape)),
        .tape_size = TAPE_SIZE,
    };
    PROFILE_START(run_program);
    run_program(&bf, p);
    PROFILE_END(run_program);

    if (g_profile_enabled) {
        fprintf(g_profile_out, "\nOpcode counts:\n");
        size_t total = 0;
        for (size_t i = 0; i < OP_MAX; i++) {
            if (i == OP_INVALID) continue;
            fprintf(g_profile_out,
                    "    %s: %zu\n",
                    opcode_names[i],
                    opcode_counts[i]);
            total += opcode_counts[i];
        }
        fprintf(g_profile_out, "Total: %zu\n", total);

        fprintf(g_profile_out, "\nData Pointer Stats:\n");
        fprintf(g_profile_out, "    Min Position = %td\n", data_ptr_min);
        fprintf(g_profile_out,
                "    Max Position = %td\n",
                data_ptr_max % bf.tape_size);
        fclose(profile_out);
    }

    mason_arena_destroy(g_arena);
    return 0;
}
