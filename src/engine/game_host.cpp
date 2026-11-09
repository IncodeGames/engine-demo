#include "engine/engine.h"
#include <string.h>
#include <stdio.h>

#define GAME_RELOAD_MAX_ATTEMPTS 10

typedef enum GameLoadResult
{
    GAME_LOAD_OK,
    GAME_LOAD_NOT_READY, // copy or load failed: usually the linker is still writing, can retry
    GAME_LOAD_INCOMPATIBLE
} GameLoadResult;

// Each load copies game.dll to a uniquely-named temp, so the previous module can
// stay resident long enough to run its shutdown on a schema change.
// TODO(BPL): old temp DLLs are left on disk, clean them up once the loader is stable. 2026-05-20
static GameLoadResult game_host_try_load(GameHost *h, PlatformAPI *p)
{
    h->loaded = false;
    char tmp[260];
    snprintf(tmp, sizeof(tmp), "game_loaded_%u.dll", h->load_counter++);
    if (!p->file_copy(h->dll_path, tmp, true))
    {
        return GAME_LOAD_NOT_READY;
    }

    void *lib = p->library_load(tmp);
    if (!lib)
    {
        return GAME_LOAD_NOT_READY;
    }

    GetGameAPIFn get = (GetGameAPIFn)p->library_symbol(lib, "GetGameAPI");
    if (!get)
    {
        p->library_unload(lib);
        return GAME_LOAD_NOT_READY;
    }

    union
    {
        GameAPI api;
        u8 room[1024]; // Padding if new module header size difference between binaries
    } out;
    memset(&out, 0, sizeof(out));
    get(&out.api);
    if (out.api.version != GAME_API_VERSION || out.api.engine_abi_hash != engine_abi_hash())
    {
        p->log("game: game.dll was built against different engine headers than this exe; shut down and do a full rebuild");
        p->library_unload(lib);
        return GAME_LOAD_INCOMPATIBLE;
    }
    h->library = lib;
    strncpy(h->dll_load_path, tmp, sizeof(h->dll_load_path) - 1);
    h->api = out.api;
    h->loaded = true;
    return GAME_LOAD_OK;
}

void game_host_load(GameHost *h, PlatformAPI *p, EngineContext *e)
{
    (void)e;
    u32 counter = h->load_counter;
    memset(h, 0, sizeof(*h));
    h->load_counter = counter;
    strcpy(h->dll_path, "game.dll");
    GameLoadResult result = game_host_try_load(h, p);
    h->reload_status = (result == GAME_LOAD_OK)             ? GAME_RELOAD_NONE
                       : (result == GAME_LOAD_INCOMPATIBLE) ? GAME_RELOAD_RESTART_REQUIRED
                                                            : GAME_RELOAD_LOAD_FAILED;
    h->dll_modified_time = p->file_modified_time(h->dll_path);
    h->schema_hash = h->loaded ? h->api.schema_hash : 0;
}

void game_host_unload(GameHost *h, PlatformAPI *p)
{
    if (h->library)
        p->library_unload(h->library);
    h->library = 0;
    h->loaded = false;
    memset(&h->api, 0, sizeof(h->api));
}

void game_host_tick(GameHost *h, PlatformAPI *p, EngineContext *e)
{
    u64 t = p->file_modified_time(h->dll_path);
    if (t == 0 || t == h->dll_modified_time)
        return;
    if (p->file_exists("game.lock")) // held by the build script until the link has finished
        return;
    u64 now = p->time_ticks();
    if (now < h->retry_at_ticks)
        return;
    GameHost old = *h; // keep the old module resident
    h->library = 0;
    h->loaded = false;
    GameLoadResult result = game_host_try_load(h, p);
    if (result != GAME_LOAD_OK)
    {
        *h = old;
        if (result == GAME_LOAD_INCOMPATIBLE)
        {
            h->reload_status = GAME_RELOAD_RESTART_REQUIRED;
            h->dll_modified_time = t; // nothing to retry until game.dll changes again
            h->failed_attempts = 0;
            return;
        }
        h->retry_at_ticks = now + p->time_frequency() / 5; // try again in 200 ms
        if (++h->failed_attempts >= GAME_RELOAD_MAX_ATTEMPTS)
        {
            p->log("game: reload keeps failing (see errors above); keeping the previous module until game.dll changes again");
            h->reload_status = GAME_RELOAD_LOAD_FAILED;
            h->dll_modified_time = t;
            h->failed_attempts = 0;
        }
        return;
    }

    h->dll_modified_time = t;
    h->failed_attempts = 0;
    if (!old.loaded || h->api.schema_hash != old.schema_hash)
    {
        p->log("game: full reset (schema changed or no prior module)");
        if (old.loaded && old.api.shutdown && e->game_state)
        {
            old.api.shutdown(e, e->game_state);
        }

        if (old.library)
        {
            p->library_unload(old.library);
        }

        systems_clear_transient(&e->systems);
        ui_nodes_clear_transient(e->ui); // + game-owned retained trees (and their bind pointers) die with the game arena
        arena_reset(&e->ecs_arena);
        arena_reset(&e->game_arena);
        e->world = ecs_world_create(&e->ecs_arena, e->config.max_entities);
        e->game_state = h->api.init ? h->api.init(e) : 0;
        h->reload_status = GAME_RELOAD_STATE_RESET;
    }
    else
    {
        p->log("game: reloaded (code swapped, state preserved)");
        if (old.library)
        {
            p->library_unload(old.library);
        }

        systems_clear_transient(&e->systems);
        if (h->api.reload)
        {
            h->api.reload(e, e->game_state);
        }
        h->reload_status = GAME_RELOAD_CODE_SWAPPED;
    }
    h->schema_hash = h->api.schema_hash;
}