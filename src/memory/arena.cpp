#include "memory/arena.h"
#include "shared/assert.h"
#include "threading/sync.h"
#include <string.h>

static_assert(sizeof(usize) == 8, "arena_push_atomic assumes a 64-bit usize");

void arena_init(Arena *a, void *base, usize size, const char *tag)
{
    a->base = (u8 *)base;
    a->size = size;
    a->used = 0;
    a->tag = tag;
}

void arena_reset(Arena *a) { a->used = 0; }

ENGINE_API void *arena_push(Arena *a, usize size, usize align)
{
    if (align == 0)
        align = 1;
    usize cur = (usize)a->base + a->used;
    usize aligned = AlignUp(cur, align);
    usize pad = aligned - (usize)a->base;
    ASSERT(pad + size <= a->size && "arena out of memory");
    a->used = pad + size;
    return (void *)aligned;
}

ENGINE_API void *arena_push_zero(Arena *a, usize size, usize align)
{
    void *ptr = arena_push(a, size, align);
    memset(ptr, 0, size);
    return ptr;
}

// Callers must ensure no concurrent non-atomic access (e.g. arena_reset) to
// this arena; a frame barrier separates the two in the engine's render path.
void *arena_push_atomic(Arena *a, usize size)
{
    size = AlignUp(size, 16);
    usize off = (usize)atomic_fetch_add_u64((volatile u64 *)&a->used, (u64)size);
    ASSERT(off + size <= a->size && "arena out of memory (atomic)");
    return a->base + off;
}

ENGINE_API ArenaScope arena_scope_begin(Arena *a)
{
    ArenaScope s;
    s.arena = a;
    s.saved_used = a->used;
    return s;
}
ENGINE_API void arena_scope_end(ArenaScope s) { s.arena->used = s.saved_used; }