#include "engine/engine.h"
#include <string.h>
#include <stdio.h>

#define DEV_BUILD_WATCH_INTERVAL_MS 250

static void dev_build_push_line(EngineContext *e, const char *text, u32 len)
{
    DevBuild *b = &e->dev_build;
    if (len == 0)
        return;
    char *dst = b->lines[b->line_count++ % DEV_BUILD_LOG_LINES];
    memcpy(dst, text, len);
    dst[len] = 0;
    e->platform->log(dst);
}

static void dev_build_consume(EngineContext *e, const char *data, u32 size)
{
    DevBuild *b = &e->dev_build;
    for (u32 i = 0; i < size; ++i)
    {
        char c = data[i];
        if (c == '\r')
            continue;
        if (c == '\n')
        {
            dev_build_push_line(e, b->partial, b->partial_len);
            b->partial_len = 0;
        }
        else if (b->partial_len < DEV_BUILD_LINE_MAX - 1)
            b->partial[b->partial_len++] = c;
    }
}

static void dev_build_source_visit(const char *path, u64 modified_time, void *user)
{
    static const char *extensions[] = {".cpp", ".h", ".hpp", ".c", ".inl"};
    u64 *newest = (u64 *)user;
    const char *ext = strrchr(path, '.');
    if (!ext)
        return;
    for (u32 i = 0; i < ArrayCount(extensions); ++i)
    {
        if (!strcmp(ext, extensions[i]))
        {
            *newest = Max(*newest, modified_time);
            return;
        }
    }
}

static u64 dev_build_newest_source(EngineContext *e)
{
    u64 newest = 0;
    if (e->config.game_source_root[0])
        e->platform->dir_visit_files(e->config.game_source_root, dev_build_source_visit, &newest);
    return newest;
}

void dev_build_start(EngineContext *e)
{
    DevBuild *b = &e->dev_build;
    PlatformAPI *p = e->platform;
    if (b->status == DEV_BUILD_RUNNING)
    {
        return;
    }

    // Recorded before anything can fail. Saving mid-build triggers another build,
    // and failed builds do not retrigger until the source changes again.
    b->built_source_time = dev_build_newest_source(e);
    if (!e->config.build_game_command[0])
    {
        p->log("build: build_game_command is not set in engine.cfg");
        return;
    }

    b->line_count = 0;
    b->partial_len = 0;
    b->start_ticks = p->time_ticks();
    if (!p->process_start(&b->process, e->config.build_game_command, 0))
    {
        b->status = DEV_BUILD_FAILED;
        p->log("build: could not start the build command");
        return;
    }

    b->status = DEV_BUILD_RUNNING;
    p->log("build: started");
}

static void dev_build_watch(EngineContext *e)
{
    DevBuild *b = &e->dev_build;
    PlatformAPI *p = e->platform;
    if (!e->config.game_source_root[0] || b->status == DEV_BUILD_RUNNING)
    {
        return;
    }

    u64 now = p->time_ticks();
    if (now < b->next_watch_ticks)
    {
        return;
    }

    b->next_watch_ticks = now + p->time_frequency() * DEV_BUILD_WATCH_INTERVAL_MS / 1000;
    if (dev_build_newest_source(e) > b->built_source_time)
    {
        p->log("build: game source changed");
        dev_build_start(e);
    }
}

void dev_build_tick(EngineContext *e)
{
    DevBuild *b = &e->dev_build;
    PlatformAPI *p = e->platform;
    if (input_key_pressed(e, KEY_F5))
    {
        dev_build_start(e);
    }

    dev_build_watch(e);
    if (b->status != DEV_BUILD_RUNNING)
    {
        return;
    }

    i32 exit_code = 0;
    b32 exited = p->process_exited(&b->process, &exit_code); // sampled before the drain so the tail isn't lost
    char chunk[1024];
    u32 n;
    while ((n = p->process_read(&b->process, chunk, sizeof(chunk))) != 0)
    {
        dev_build_consume(e, chunk, n);
    }

    if (!exited)
    {
        return;
    }

    dev_build_push_line(e, b->partial, b->partial_len);
    b->partial_len = 0;
    p->process_release(&b->process);
    b->seconds = (f32)((f64)(p->time_ticks() - b->start_ticks) / (f64)p->time_frequency());
    b->status = (exit_code == 0) ? DEV_BUILD_OK : DEV_BUILD_FAILED;
    char msg[96];
    snprintf(msg, sizeof(msg), "build: %s in %.2f s", exit_code == 0 ? "ok" : "FAILED", b->seconds);
    p->log(msg);
}