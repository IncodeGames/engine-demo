#pragma once
#include "shared/types.h"
#include "memory/arena.h"
#include "threading/sync.h"

// A greedy shared-pool job system. Idle threads pull any available job, so load
// balances dynamically across the worker pool. Jobs may submit further jobs.
// Waiting on a counter is cooperative: a waiting thread runs other pool jobs
// until the counter reaches zero.

typedef struct JobCounter
{
    volatile u32 value;
} JobCounter;

// begin/end describe a half-open item range (for parallel-for dispatches).
// thread_index is the worker currently executing the job (not the submitter).
typedef void (*JobFn)(void *user, u32 begin, u32 end, u32 thread_index);

typedef struct Job
{
    JobFn fn;
    void *user;
    u32 begin;
    u32 end;
    JobCounter *counter;
} Job;

typedef struct JobQueue
{
    Job *jobs;
    u32 capacity;
    u32 head;
    u32 tail;
    u32 count;
    SpinLock lock;
} JobQueue;

typedef struct JobSystem
{
    JobQueue queue;
} JobSystem;

JobSystem *jobs_create(Arena *persistent, u32 queue_capacity);

void jobs_counter_reset(JobCounter *counter);

// Submit one job against 'counter'. If the pool is full the caller runs a
// pending job inline to make room, then retries.
void jobs_submit(JobSystem *js, JobFn fn, void *user, u32 begin, u32 end,
                 JobCounter *counter, u32 calling_thread_index);

// Parallel-for: splits [0, count) into ceil(count / items_per_job) jobs.
void jobs_dispatch(JobSystem *js, JobFn fn, void *user, u32 count, u32 items_per_job,
                   JobCounter *counter, u32 calling_thread_index);

// Runs one job from the pool if available. Returns true if a job was executed.
b32 jobs_help_once(JobSystem *js, u32 thread_index);

// Runs pool jobs until *counter reaches zero.
void jobs_wait(JobSystem *js, JobCounter *counter, u32 thread_index);