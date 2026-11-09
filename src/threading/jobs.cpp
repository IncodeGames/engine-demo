#include "threading/jobs.h"
#include "shared/assert.h"

static b32 job_queue_push(JobQueue *q, const Job *job)
{
    spin_lock(&q->lock);
    b32 ok = false;
    if (q->count < q->capacity)
    {
        q->jobs[q->tail] = *job;
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
        ok = true;
    }
    spin_unlock(&q->lock);
    return ok;
}

static b32 job_queue_pop(JobQueue *q, Job *out)
{
    spin_lock(&q->lock);
    b32 ok = false;
    if (q->count > 0)
    {
        *out = q->jobs[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        ok = true;
    }
    spin_unlock(&q->lock);
    return ok;
}

static void job_execute(const Job *job, u32 thread_index)
{
    job->fn(job->user, job->begin, job->end, thread_index);
    atomic_decrement_u32(&job->counter->value);
}

JobSystem *jobs_create(Arena *persistent, u32 queue_capacity)
{
    ASSERT(queue_capacity > 0);
    JobSystem *js = PushStruct(persistent, JobSystem);
    js->queue.jobs = PushArray(persistent, Job, queue_capacity);
    js->queue.capacity = queue_capacity;
    return js;
}

void jobs_counter_reset(JobCounter *counter) { atomic_store_u32(&counter->value, 0); }

void jobs_submit(JobSystem *js, JobFn fn, void *user, u32 begin, u32 end,
                 JobCounter *counter, u32 ti)
{
    Job job;
    job.fn = fn;
    job.user = user;
    job.begin = begin;
    job.end = end;
    job.counter = counter;
    // Increment before publishing so a waiter never observes a false zero.
    atomic_increment_u32(&counter->value);
    while (!job_queue_push(&js->queue, &job))
    {
        if (!jobs_help_once(js, ti))
        {
            cpu_pause(); // pool saturated
        }
    }
}

void jobs_dispatch(JobSystem *js, JobFn fn, void *user, u32 count, u32 items_per_job,
                   JobCounter *counter, u32 ti)
{
    if (count == 0)
    {
        return;
    }

    if (items_per_job == 0)
    {
        items_per_job = 1;
    }

    for (u32 begin = 0; begin < count; begin += items_per_job)
    {
        u32 end = Min(begin + items_per_job, count);
        jobs_submit(js, fn, user, begin, end, counter, ti);
    }
}

b32 jobs_help_once(JobSystem *js, u32 thread_index)
{
    Job job;
    if (!job_queue_pop(&js->queue, &job))
        return false;
    job_execute(&job, thread_index);
    return true;
}

void jobs_wait(JobSystem *js, JobCounter *counter, u32 thread_index)
{
    while (atomic_load_u32(&counter->value) != 0)
    {
        if (jobs_help_once(js, thread_index))
        {
            continue;
        }

        // Nothing to pull and another thread is still inside a job so wait.
        cpu_pause();
    }
}