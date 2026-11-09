#include "engine/engine.h"
#include <string.h>

void systems_init(SystemRegistry *reg, Arena *persistent, u32 thread_count)
{
    memset(reg, 0, sizeof(*reg));
    reg->thread_stats = PushArray(persistent, SystemThreadStats, thread_count);
    reg->thread_count = thread_count;
}

void systems_clear_transient(SystemRegistry *reg)
{
    u32 w = 0;
    for (u32 i = 0; i < reg->count; ++i)
        if (reg->systems[i].desc.persist_across_reload)
            reg->systems[w++] = reg->systems[i];
    reg->count = w;
    reg->active_count = 0; // nothing is runnable until the order index is rebuilt
    reg->order_dirty = true;
}

ENGINE_API SystemId system_register(EngineContext *e, const SystemDesc *desc)
{
    SystemRegistry *reg = &e->systems;
    ASSERT(reg->count < MAX_SYSTEMS);

    SystemId id = reg->count++; // Appends past active_count.
    System *s = &reg->systems[id];
    memset(s, 0, sizeof(*s));
    s->desc = *desc;
    s->registration_seq = reg->next_seq++;
    for (u32 i = 0; i < desc->query.include_count; ++i)
    {
        mask_set(&s->include_mask, desc->query.include[i]);
    }

    for (u32 i = 0; i < desc->query.exclude_count; ++i)
    {
        mask_set(&s->exclude_mask, desc->query.exclude[i]);
    }

    reg->order_dirty = true;
    return id;
}

static void rebuild_order_index(SystemRegistry *reg)
{
    for (u32 i = 0; i < reg->count; ++i)
    {
        reg->order_index[i] = (u16)i;
    }

    for (u32 i = 1; i < reg->count; ++i)
    {
        u16 v = reg->order_index[i];
        System *sv = &reg->systems[v];
        u32 j = i;
        while (j > 0)
        {
            System *sp = &reg->systems[reg->order_index[j - 1]];
            b32 greater = (sp->desc.order > sv->desc.order) ||
                          (sp->desc.order == sv->desc.order && sp->registration_seq > sv->registration_seq);
            if (!greater)
            {
                break;
            }

            reg->order_index[j] = reg->order_index[j - 1];
            --j;
        }
        reg->order_index[j] = v;
    }
    reg->active_count = reg->count;
    reg->order_dirty = false;
}

static FORCE_INLINE System *system_at(SystemRegistry *reg, u32 i)
{
    return &reg->systems[reg->order_index[i]];
}

static void query_for(EngineContext *e, System *sys, EcsQueryResult *out)
{
    ecs_query(e->world, sys->desc.query.include, sys->desc.query.include_count,
              sys->desc.query.exclude, sys->desc.query.exclude_count, out);
}

// ----------------------------- thread 0, serial ----------------------------
static void systems_begin_frame(EngineContext *e)
{
    SystemRegistry *reg = &e->systems;
    arena_reset(&e->systems_arena);
    u32 w = 0;
    for (u32 i = 0; i < reg->count; ++i)
    {
        if (reg->systems[i].has_run_once) // ignore systems that only run once on first frame
        {
            reg->order_dirty = true;
            continue;
        }
        if (w != i)
        {
            reg->systems[w] = reg->systems[i];
        }
        w++;
    }
    reg->count = w;
    if (reg->order_dirty)
        rebuild_order_index(reg);
    for (u32 t = 0; t < reg->thread_count; ++t)
    {
        reg->thread_stats[t].rows = 0;
        reg->thread_stats[t].chunks = 0;
    }
}

static void system_run_main_thread(EngineContext *e, System *sys)
{
    u64 t0 = e->platform->time_ticks();
    EcsQueryResult q;
    query_for(e, sys, &q);
    u32 rows = 0;
    for (u32 ai = 0; ai < q.count; ++ai)
    {
        ArchetypeId a = q.archetypes[ai];
        SystemIter it = {e->world, a, 0, ecs_archetype_count(e->world, a), 0, 1};
        if (it.end > it.begin)
        {
            sys->desc.fn(e, &it, sys->desc.user);
            rows += it.end - it.begin;
        }
    }
    sys->total_rows = rows;
    sys->last_ticks = e->platform->time_ticks() - t0;
    e->systems.thread_stats[0].rows += rows;
    if (sys->desc.run_once)
        sys->has_run_once = true;
}

// Resolve the query and reset the claim cursors
static void system_prepare(EngineContext *e, System *sys)
{
    EcsQueryResult q;
    query_for(e, sys, &q);
    sys->work = q.count ? PushArray(&e->systems_arena, SystemWork, q.count) : 0;
    sys->work_count = q.count;
    sys->total_rows = 0;
    for (u32 i = 0; i < q.count; ++i)
    {
        sys->work[i].archetype = q.archetypes[i];
        sys->work[i].count = ecs_archetype_count(e->world, q.archetypes[i]);
        sys->work[i].next = 0;
        sys->total_rows += sys->work[i].count;
    }
    atomic_store_u32(&sys->rows_done, 0);
    if (sys->desc.run_once)
        sys->has_run_once = true;
}

// ------------------------------- all threads -------------------------------
static void system_run_parallel(EngineContext *e, System *sys, u32 ti, u32 tc)
{
    SystemThreadStats *stats = &e->systems.thread_stats[ti];
    u64 t0 = (ti == 0) ? e->platform->time_ticks() : 0;
    u32 done = 0;

    if (sys->desc.schedule == SYSTEM_PARALLEL_STATIC)
    {
        for (u32 wi = 0; wi < sys->work_count; ++wi)
        {
            SystemWork *w = &sys->work[wi];
            u32 begin, end;
            work_split(w->count, ti, tc, &begin, &end);
            if (end > begin)
            {
                SystemIter it = {e->world, w->archetype, begin, end, ti, tc};
                sys->desc.fn(e, &it, sys->desc.user);
                done += end - begin;
                stats->chunks++;
            }
        }
    }
    else
    {
        u32 chunk = sys->desc.chunk_rows ? sys->desc.chunk_rows : SYSTEM_DEFAULT_CHUNK_ROWS;
        for (u32 wi = 0; wi < sys->work_count; ++wi)
        {
            SystemWork *w = &sys->work[wi];
            while (w->next < w->count) // plain read first: exhausted ranges cost no atomic
            {
                u32 begin = atomic_fetch_add_u32(&w->next, chunk);
                if (begin >= w->count)
                    break;
                u32 end = Min(begin + chunk, w->count);
                SystemIter it = {e->world, w->archetype, begin, end, ti, tc};
                sys->desc.fn(e, &it, sys->desc.user);
                done += end - begin;
                stats->chunks++;
            }
        }
    }

    stats->rows += done;
    if (done)
    {
        atomic_fetch_add_u32(&sys->rows_done, done);
    }

    // Out of chunks
    while (atomic_load_u32(&sys->rows_done) != sys->total_rows)
    {
        cpu_pause();
    }

    if (ti == 0)
        sys->last_ticks = e->platform->time_ticks() - t0;
}

// A segment is [main-thread systems...][parallel systems...], ends before next main-thread system.
static void segment_bounds(SystemRegistry *reg, u32 begin, u32 *par_begin, u32 *par_end)
{
    u32 i = begin;
    while (i < reg->active_count && system_at(reg, i)->desc.schedule == SYSTEM_MAIN_THREAD)
        ++i;
    *par_begin = i;
    while (i < reg->active_count && system_at(reg, i)->desc.schedule != SYSTEM_MAIN_THREAD)
        ++i;
    *par_end = i;
}

void systems_run_all(EngineContext *e, u32 ti, u32 tc)
{
    PlatformAPI *p = e->platform;
    SystemRegistry *reg = &e->systems;
    u32 si = 0;
    for (;;)
    {
        u32 par_begin = 0, par_end = 0;
        if (ti == 0)
        {
            if (si == 0)
            {
                systems_begin_frame(e);
            }

            segment_bounds(reg, si, &par_begin, &par_end);
            for (u32 i = si; i < par_begin; ++i)
            {
                system_run_main_thread(e, system_at(reg, i));
            }

            for (u32 i = par_begin; i < par_end; ++i)
            {
                system_prepare(e, system_at(reg, i)); // sees structural changes made just above
            }
        }
        p->barrier_wait(e->frame_barrier); // publishes thread 0's work; the only barrier per segment
        if (ti != 0)
        {
            segment_bounds(reg, si, &par_begin, &par_end); // registry is stable after the barrier
        }

        for (u32 i = par_begin; i < par_end; ++i)
        {
            system_run_parallel(e, system_at(reg, i), ti, tc);
        }

        si = par_end;
        if (si >= reg->active_count)
        {
            break;
        }
    }
}