#pragma once
#include "shared/types.h"
#include "threading/sync.h"
#include "ecs/ecs.h"
struct EngineContext;
#define MAX_SYSTEMS 256
#define MAX_SYSTEM_QUERY_COMPONENTS 16
#define SYSTEM_DEFAULT_CHUNK_ROWS 4096

typedef enum SystemSchedule
{
    SYSTEM_MAIN_THREAD,     // thread 0 only; the only place **structural** ECS changes are allowed
    SYSTEM_PARALLEL_STATIC, // each thread gets one fixed, contiguous slice (same rows every frame)
    SYSTEM_PARALLEL_DYNAMIC // threads claim chunk_rows-sized ranges from a shared cursor until none remain
} SystemSchedule;

typedef struct SystemQuery
{
    ComponentId include[MAX_SYSTEM_QUERY_COMPONENTS];
    u32 include_count;
    ComponentId exclude[MAX_SYSTEM_QUERY_COMPONENTS];
    u32 exclude_count;
} SystemQuery;

typedef struct SystemIter
{
    World *world;
    ArchetypeId archetype;
    u32 begin;
    u32 end;
    u32 thread_index;
    u32 thread_count;
} SystemIter;

typedef void (*SystemFn)(struct EngineContext *engine, const SystemIter *it, void *user);
typedef u32 SystemId;

typedef struct SystemDesc
{
    const char *name;
    i32 order; // ascending. ties respect registration order
    SystemQuery query;
    SystemSchedule schedule;
    SystemFn fn;
    void *user;
    b32 persist_across_reload;
    u32 chunk_rows; // SYSTEM_PARALLEL_DYNAMIC: rows per claim; 0 = SYSTEM_DEFAULT_CHUNK_ROWS
    b32 run_once;
} SystemDesc;

typedef struct SystemWork
{
    alignas(64) volatile u32 next; // claim cursor in rows
    u32 count;
    ArchetypeId archetype;
} SystemWork;

typedef struct SystemThreadStats
{
    alignas(64) u32 rows; // rows this thread processed in the current systems phase
    u32 chunks;
} SystemThreadStats;

typedef struct System
{
    SystemDesc desc;
    u32 registration_seq;
    ComponentMask include_mask;
    ComponentMask exclude_mask;
    b32 has_run_once;
    SystemWork *work;
    u32 work_count;
    u32 total_rows;
    u64 last_ticks;                     // wall time on thread 0, start to completion
    alignas(64) volatile u32 rows_done; // completion: every thread adds what it processed
} System;

typedef struct SystemRegistry
{
    System systems[MAX_SYSTEMS];
    u32 count;
    u32 active_count; // scheduled this frame (snapshot taken when the order index is rebuilt)
    u32 next_seq;
    u16 order_index[MAX_SYSTEMS];
    b32 order_dirty;
    SystemThreadStats *thread_stats; // [thread_count]
    u32 thread_count;
} SystemRegistry;

void systems_init(SystemRegistry *reg, Arena *persistent, u32 thread_count);
void systems_clear_transient(SystemRegistry *reg);
void systems_run_all(struct EngineContext *engine, u32 thread_index, u32 thread_count);
// Thread 0 only. One frame delay for Systems registered mid-frame.
ENGINE_API SystemId system_register(struct EngineContext *engine, const SystemDesc *desc);