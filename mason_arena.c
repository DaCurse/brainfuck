#include "mason_arena.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define _MASON_ARENA_ALIGN (_Alignof(max_align_t))

static inline size_t align_up_pow2(size_t value, size_t align) {
    assert(align != 0 && (align & (align - 1)) == 0);
    return (value + align - 1) & ~(align - 1);
}

MASON_Arena *mason_arena_create(size_t initial_capacity) {
    assert(initial_capacity != 0);
    if (initial_capacity == 0) {
        return NULL;
    }

    MASON_Arena *arena = (MASON_Arena *)malloc(sizeof(*arena));
    if (!arena) {
        return NULL;
    }

    MASON_ArenaBlock *block =
        (MASON_ArenaBlock *)malloc(sizeof(*block) + initial_capacity);

    if (!block) {
        free(arena);
        return NULL;
    }

    block->next = NULL;
    block->capacity = initial_capacity;
    block->offset = 0;

    arena->head = block;
    arena->current = block;
    return arena;
}

void *mason_arena_alloc(MASON_Arena *arena, size_t size) {
    size = align_up_pow2(size, _Alignof(max_align_t));

    MASON_ArenaBlock *current = arena->current;
    if (current->offset + size <= current->capacity) {
        void *ptr = current->data + current->offset;
        current->offset += size;
        return ptr;
    }

    size_t new_cap = current->capacity * 2;
    if (new_cap < size)
        new_cap = size;

    MASON_ArenaBlock *new_block =
        (MASON_ArenaBlock *)malloc(sizeof(*new_block) + new_cap);

    if (!new_block) {
        return NULL;
    }

    new_block->next = NULL;
    new_block->capacity = new_cap;
    new_block->offset = size;

    arena->current->next = new_block;
    arena->current = new_block;

    return new_block->data;
}

void *mason_arena_calloc(MASON_Arena *arena, size_t count, size_t size) {
    if (count == 0 || size == 0) {
        return NULL;
    }

    size_t total_size = count * size;
    if (total_size / count != size) {
        return NULL;
    }

    void *ptr = mason_arena_alloc(arena, total_size);
    if (ptr) {
        memset(ptr, 0, total_size);
    }

    return ptr;
}

MASON_ArenaMark mason_arena_mark(MASON_Arena *arena) {
    assert(arena && arena->current);
    return (MASON_ArenaMark){arena->current, arena->current->offset};
}

MASON_ArenaStats mason_arena_stats(const MASON_Arena *arena) {
    MASON_ArenaStats stats = {0};
    if (!arena) {
        return stats;
    }

    for (const MASON_ArenaBlock *block = arena->head; block; block = block->next) {
        stats.block_count++;
        stats.total_capacity += block->capacity;
        stats.total_used += block->offset;
    }

    return stats;
}

void mason_arena_rewind(MASON_Arena *arena, MASON_ArenaMark mark) {
    assert(arena && arena->head);
    assert(mark.block);
    assert(mark.offset <= mark.block->capacity);

    arena->current = mark.block;
    arena->current->offset = mark.offset;

    for (MASON_ArenaBlock *block = mark.block->next; block; block = block->next) {
        block->offset = 0;
    }
}

void mason_arena_reset(MASON_Arena *arena) {
    assert(arena && arena->head);

    for (MASON_ArenaBlock *block = arena->head; block; block = block->next) {
        block->offset = 0;
    }

    arena->current = arena->head;
}

void mason_arena_destroy(MASON_Arena *arena) {
    assert(arena && arena->head);

    for (MASON_ArenaBlock *block = arena->head; block;) {
        MASON_ArenaBlock *next = block->next;
        free(block);
        block = next;
    }

    free(arena);
}
