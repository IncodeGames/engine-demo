#include "engine/engine.h"
#include <string.h>
#include <stdio.h>

#define SHADER_POLL_INTERVAL_MS 250

typedef struct ShaderReload
{
    ShaderFile *file;
    PlatformFileContents source;
} ShaderReload;

// NOTE(BPL): A failed read doesn't update the timestamp, so next poll tries again.
static PlatformFileContents shader_file_read(EngineContext *e, ShaderFile *f, Arena *scratch)
{
    PlatformAPI *p = e->platform;
    u64 modified = p->file_modified_time(f->path); // sampled before the read
    PlatformFileContents source = p->file_read(f->path, scratch);
    if (source.ok)
    {
        f->modified_time = modified;
    }
    return source;
}

static b32 shader_file_compile(EngineContext *e, ShaderFile *f, PlatformFileContents source)
{
    b32 ok = e->renderer_api.compile_shader_hlsl(e->renderer, f->handle, (const char *)source.data,
                                                 source.size, f->path, f->entry);
    char msg[400];
    snprintf(msg, sizeof(msg), "shader: %s '%s' (%s)", ok ? "compiled" : "compile FAILED", f->path, f->entry);
    e->platform->log(msg);
    return ok;
}

static void shader_log_unreadable(EngineContext *e, const ShaderFile *f)
{
    char msg[400];
    snprintf(msg, sizeof(msg), "shader: could not read '%s'", f->path);
    e->platform->log(msg);
}

static void shader_file_pull_source(EngineContext *e, ShaderFile *f)
{
    PlatformAPI *p = e->platform;
    if (!f->source_path[0])
    {
        return;
    }

    u64 source_time = p->file_modified_time(f->source_path);
    u64 copy_time = p->file_modified_time(f->path);
    if (copy_time != 0 && source_time > copy_time)
    {
        p->file_copy(f->source_path, f->path, true);
    }
}

ENGINE_API ShaderHandle shader_load(EngineContext *e, const char *relative_path, ShaderStage stage, const char *entry_point)
{
    ShaderLibrary *lib = &e->shaders;
    ShaderHandle none = {0};
    char path[260];
    snprintf(path, sizeof(path), "%s/%s", e->config.assets_root, relative_path);
    for (u32 i = 0; i < lib->count; ++i)
    {
        ShaderFile *f = &lib->files[i];
        if (f->stage == stage && !strcmp(f->path, path) && !strcmp(f->entry, entry_point))
        {
            return f->handle; // a full game reset runs init again
        }
    }

    ASSERT(lib->count < SHADER_FILES_MAX && "shader library is full");
    if (lib->count >= SHADER_FILES_MAX)
    {
        return none;
    }

    ShaderHandle handle = e->renderer_api.reserve_shader(e->renderer, stage);
    if (!handle.id)
    {
        return none;
    }

    ShaderFile *f = &lib->files[lib->count++];
    memset(f, 0, sizeof(*f));
    f->handle = handle;
    f->stage = stage;
    snprintf(f->path, sizeof(f->path), "%s", path);
    if (e->config.assets_source_root[0])
    {
        snprintf(f->source_path, sizeof(f->source_path), "%s/%s", e->config.assets_source_root, relative_path);
    }

    snprintf(f->entry, sizeof(f->entry), "%s", entry_point);
    shader_file_pull_source(e, f);

    Arena *scratch = &e->memory.scratch[0];
    ArenaScope scope = arena_scope_begin(scratch);
    PlatformFileContents source = shader_file_read(e, f, scratch);
    if (source.ok)
    {
        shader_file_compile(e, f, source);
    }
    else
    {
        shader_log_unreadable(e, f);
    }
    arena_scope_end(scope);
    return handle;
}

void shaders_tick(EngineContext *e)
{
    ShaderLibrary *lib = &e->shaders;
    PlatformAPI *p = e->platform;
    b32 reload_all = lib->reload_all_requested || input_key_pressed(e, KEY_F6);
    lib->reload_all_requested = false;
    u64 now = p->time_ticks();
    if (!reload_all && now < lib->next_poll_ticks)
    {
        return;
    }

    lib->next_poll_ticks = now + p->time_frequency() * SHADER_POLL_INTERVAL_MS / 1000;
    Arena *scratch = &e->memory.scratch[0];
    ArenaScope scope = arena_scope_begin(scratch);
    ShaderReload *pending = PushArrayRaw(scratch, ShaderReload, lib->count);
    u32 pending_count = 0, ok = 0, failed = 0;
    for (u32 i = 0; i < lib->count; ++i)
    {
        ShaderFile *f = &lib->files[i];
        shader_file_pull_source(e, f);
        u64 modified = p->file_modified_time(f->path);
        if (!reload_all && (modified == 0 || modified == f->modified_time))
        {
            continue;
        }

        PlatformFileContents source = shader_file_read(e, f, scratch);
        if (source.ok)
        {
            pending[pending_count].file = f;
            pending[pending_count].source = source;
            pending_count++;
        }
        else if (reload_all)
        {
            shader_log_unreadable(e, f);
            failed++;
        }
    }
    if (pending_count)
    {
        engine_render_pause(e); // swap shaders only while idle!!!
        for (u32 i = 0; i < pending_count; ++i)
        {
            if (shader_file_compile(e, pending[i].file, pending[i].source))
            {
                ok++;
            }
            else
            {
                failed++;
            }
        }
        engine_render_resume(e);
    }
    arena_scope_end(scope);
    if (ok + failed)
    {
        lib->last_ok = ok;
        lib->last_failed = failed;
    }
    if (reload_all)
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "shaders: reload all: %u ok, %u failed", ok, failed);
        p->log(msg);
    }
}