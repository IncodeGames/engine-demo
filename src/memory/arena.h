#pragma once
#include "shared/types.h"

typedef struct Arena
{
    u8 *base;
    usize size;
    usize used;
    const char *tag;
} Arena;

typedef struct ArenaScope
{
    Arena *arena;
    usize saved_used;
} ArenaScope;

void arena_init(Arena *a, void *base, usize size, const char *tag);
void arena_reset(Arena *a);

ENGINE_API void *arena_push(Arena *a, usize size, usize align);
ENGINE_API void *arena_push_zero(Arena *a, usize size, usize align);
ENGINE_API ArenaScope arena_scope_begin(Arena *a);
ENGINE_API void arena_scope_end(ArenaScope scope);

// Thread-safe bump allocation. 'size' is rounded up to 16 bytes so each result is
// 16-byte aligned (arenas carved from page-aligned reservations always are).
// Intended for concurrent writers claiming disjoint regions within a frame.
void *arena_push_atomic(Arena *a, usize size);

#define PushStruct(a, T) (T *)arena_push_zero((a), sizeof(T), alignof(T))
#define PushArray(a, T, count) (T *)arena_push_zero((a), sizeof(T) * (usize)(count), alignof(T))
#define PushArrayRaw(a, T, count) (T *)arena_push((a), sizeof(T) * (usize)(count), alignof(T))