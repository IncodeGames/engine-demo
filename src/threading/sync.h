#pragma once
#include "shared/types.h"
#include <intrin.h>
#include <emmintrin.h>

static FORCE_INLINE u32 atomic_fetch_add_u32(volatile u32 *v, u32 addend)
{
    return (u32)_InterlockedExchangeAdd((volatile long *)v, (long)addend);
}

static FORCE_INLINE u64 atomic_fetch_add_u64(volatile u64 *v, u64 addend)
{
    return (u64)_InterlockedExchangeAdd64((volatile __int64 *)v, (__int64)addend);
}

static FORCE_INLINE u32 atomic_increment_u32(volatile u32 *v)
{
    return (u32)_InterlockedIncrement((volatile long *)v);
}

static FORCE_INLINE u32 atomic_decrement_u32(volatile u32 *v)
{
    return (u32)_InterlockedDecrement((volatile long *)v);
}

static FORCE_INLINE void atomic_store_u32(volatile u32 *v, u32 x)
{
    _InterlockedExchange((volatile long *)v, (long)x);
}

static FORCE_INLINE u32 atomic_load_u32(volatile u32 *v) { return *v; }

static FORCE_INLINE void cpu_pause(void) { _mm_pause(); }

typedef struct SpinLock
{
    volatile long locked;
} SpinLock;

static FORCE_INLINE void spin_lock(SpinLock *s)
{
    for (;;)
    {
        if (_InterlockedExchange(&s->locked, 1) == 0)
            return;
        while (s->locked)
            cpu_pause();
    }
}

static FORCE_INLINE void spin_unlock(SpinLock *s) { _InterlockedExchange(&s->locked, 0); }

typedef struct WorkCounter
{
    volatile u32 next;
    u32 count;
} WorkCounter;

static FORCE_INLINE void work_counter_init(WorkCounter *wc, u32 count)
{
    wc->next = 0;
    wc->count = count;
}

static FORCE_INLINE b32 work_counter_next(WorkCounter *wc, u32 *out_index)
{
    u32 i = atomic_fetch_add_u32(&wc->next, 1);
    if (i >= wc->count)
        return false;
    *out_index = i;
    return true;
}

static FORCE_INLINE void work_split(u32 count, u32 thread_index, u32 thread_count,
                                    u32 *out_begin, u32 *out_end)
{
    u32 base = count / thread_count;
    u32 rem = count % thread_count;
    u32 begin = thread_index * base + Min(thread_index, rem);
    u32 extra = (thread_index < rem) ? 1u : 0u;
    *out_begin = begin;
    *out_end = begin + base + extra;
}