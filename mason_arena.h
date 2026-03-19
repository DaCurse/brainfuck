#ifndef MASON_ARENA
#define MASON_ARENA

#include <stddef.h>
#include <stdint.h>

typedef struct MASON_ArenaBlock {
    struct MASON_ArenaBlock *next;
    size_t capacity;
    size_t offset;
    _Alignas(max_align_t) unsigned char data[];
} MASON_ArenaBlock;

typedef struct {
    MASON_ArenaBlock *head;
    MASON_ArenaBlock *current;
} MASON_Arena;

typedef struct {
    MASON_ArenaBlock *block;
    size_t offset;
} MASON_ArenaMark;

typedef struct {
    size_t block_count;
    size_t total_capacity;
    size_t total_used;
} MASON_ArenaStats;

MASON_Arena *mason_arena_create(size_t initial_capacity);
void *mason_arena_alloc(MASON_Arena *arena, size_t size);
void *mason_arena_calloc(MASON_Arena *arena, size_t count, size_t size);
MASON_ArenaMark mason_arena_mark(MASON_Arena *arena);
MASON_ArenaStats mason_arena_stats(const MASON_Arena *arena);
void mason_arena_rewind(MASON_Arena *arena, MASON_ArenaMark mark);
void mason_arena_reset(MASON_Arena *arena);
void mason_arena_destroy(MASON_Arena *arena);

// New
void *mason_arena_realloc(MASON_Arena *arena,
                          void *ptr,
                          size_t old_size,
                          size_t new_size);

#endif // MASON_ARENA
