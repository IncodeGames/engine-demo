#pragma once
#include "shared/types.h"
#include "shared/assert.h"
#include "shared/math.h"
#include "memory/arena.h"
#include "threading/sync.h"
#include "platform/platform.h"
#include "ecs/ecs.h"
#include "renderer/renderer.h"
#include "audio/audio.h"
#include "physics/physics.h"
#include "assets/assets.h"
#include "ui/ui.h"
#include "engine/input.h"
#include "engine/systems.h"
#include "engine/game_api.h"
#include "threading/sync.h"
#include "threading/jobs.h"

#define DEV_BUILD_LOG_LINES 32
#define DEV_BUILD_LINE_MAX 192
#define SHADER_FILES_MAX 256

typedef enum DevBuildStatus
{
    DEV_BUILD_IDLE,
    DEV_BUILD_RUNNING,
    DEV_BUILD_OK,
    DEV_BUILD_FAILED
} DevBuildStatus;

typedef struct DevBuild
{
    PlatformProcess process;
    u32 status;
    u64 start_ticks;
    f32 seconds;
    char lines[DEV_BUILD_LOG_LINES][DEV_BUILD_LINE_MAX];
    u32 line_count;
    char partial[DEV_BUILD_LINE_MAX];
    u32 partial_len;
    u64 built_source_time;
    u64 next_watch_ticks;
} DevBuild;

typedef struct EngineConfig
{
    char build_game_command[260];
    char game_source_root[240];
    char assets_source_root[240];
    usize persistent_arena_size;
    usize ecs_arena_size;
    usize game_arena_size;
    usize systems_arena_size;
    usize frame_arena_size;    // per worker thread
    usize scratch_arena_size;  // per worker thread
    usize renderer_arena_size; // render command memory, both frames
    usize upload_arena_size;   // CPU upload staging, both frames
    u32 job_queue_capacity;
    u32 reserved_thread_count;
    u32 max_entities;
    char window_title[128];
    u32 window_width;
    u32 window_height;
    char assets_root[240];
    b32 ui_enabled;
    usize ui_arena_size;
    char ui_font_path[260];
    f32 ui_font_size;
} EngineConfig;

typedef struct EngineStats
{
    f32 dt_accum;
    f64 systems_seconds_accum;
    u32 sample_count;
} EngineStats;

typedef struct EngineMemory
{
    Arena persistent;
    Arena *frame;   // [thread_count]
    Arena *scratch; // [thread_count]
} EngineMemory;

typedef struct ThreadContext
{
    u32 thread_index;
    u32 thread_count;
    Arena *frame;
    Arena *scratch;
} ThreadContext;

typedef enum RenderThreadStage
{
    RENDER_STAGE_IDLE,
    RENDER_STAGE_EXECUTE,
    RENDER_STAGE_PRESENT,
    RENDER_STAGE_EXITED
} RenderThreadStage;

typedef struct EngineRenderState
{
    RenderList lists[2];
    u32 submit_queue[2];
    volatile u32 submit_count;
    volatile u32 consume_count;
    volatile u32 stage;
    volatile u32 quit;
    PlatformSemaphore ready;
    PlatformSemaphore buffer_free[2];
    PlatformThread thread;
} EngineRenderState;

typedef struct ShaderFile
{
    ShaderHandle handle;
    ShaderStage stage;
    u64 modified_time;
    char path[260];
    char source_path[260];
    char entry[64];
} ShaderFile;

typedef struct ShaderLibrary
{
    ShaderFile files[SHADER_FILES_MAX];
    u32 count;
    b32 reload_all_requested; // Inspector button; same as F6
    u64 next_poll_ticks;
    u32 last_ok, last_failed; // result of the most recent reload pass
} ShaderLibrary;

typedef enum GameReloadStatus
{
    GAME_RELOAD_NONE,
    GAME_RELOAD_CODE_SWAPPED,
    GAME_RELOAD_STATE_RESET,
    GAME_RELOAD_LOAD_FAILED,
    GAME_RELOAD_RESTART_REQUIRED
} GameReloadStatus;

typedef struct GameHost
{
    void *library;
    GameAPI api;
    u64 dll_modified_time;
    u64 schema_hash;
    u32 load_counter;
    char dll_path[260];
    char dll_load_path[260];
    b32 loaded;
    u64 retry_at_ticks;
    u32 failed_attempts;
    u32 reload_status;
} GameHost;

typedef struct EngineContext
{
    DevBuild dev_build;
    ShaderLibrary shaders;

    PlatformAPI *platform;
    EngineConfig config;
    EngineMemory memory;
    u32 thread_count;
    PlatformBarrier *frame_barrier;

    Arena ecs_arena;
    Arena game_arena;
    Arena systems_arena;
    Arena renderer_arena;
    Arena upload_arena;
    Arena ui_arena;
    b32 inspector_open;

    World *world;
    JobSystem *jobs;
    RendererAPI renderer_api;
    UIContext *ui;
    Renderer *renderer;
    AudioAPI audio_api;
    Audio *audio;
    PhysicsAPI physics_api;
    Physics *physics;
    AssetRegistry *assets;

    InputState input;
    SystemRegistry systems;

    EngineRenderState render;

    f32 stat_last_ms;
    f32 stat_last_fps;
    u64 frame_index;
    u64 last_ticks;
    f32 dt;
    f64 stat_accum_seconds;
    u32 stat_frame_count;
    volatile b32 running;
    volatile b32 quit_requested;

    GameHost game_host;
    void *game_state;
} EngineContext;

static const char *RENDER_STAGE_NAMES[] = {"idle (waiting for a submit)", "execute", "present", "exited"};

static FORCE_INLINE u64 engine_abi_hash(void)
{
    const usize sizes[] = {
        sizeof(EngineContext), sizeof(ThreadContext), sizeof(PlatformAPI), sizeof(PlatformInputEvent),
        sizeof(World), sizeof(EntitySlot), sizeof(EcsQueryResult), sizeof(SystemIter),
        sizeof(JobSystem), sizeof(AssetRegistry), sizeof(AssetEntry),
        sizeof(RenderCommand), sizeof(RenderCommandBuffer),
        sizeof(UIBox), sizeof(UISignal), sizeof(UITheme), sizeof(UIStats), sizeof(UINodeDesc), sizeof(UIInput)};
    u64 h = 14695981039346656037ull;
    for (u32 i = 0; i < ArrayCount(sizes); ++i)
    {
        h ^= sizes[i];
        h *= 1099511628211ull;
    }
    return h;
}

// config
void config_defaults(EngineConfig *out);
b32 config_load(EngineConfig *out, PlatformAPI *platform, Arena *scratch, const char *path);

// entry points
void BootstrapEntryPoint(PlatformAPI *platform);
void EntryPoint(u32 thread_index, u32 thread_count, PlatformAPI *platform,
                EngineContext *engine, PlatformBarrier *barrier);

// game host
void game_host_load(GameHost *host, PlatformAPI *platform, EngineContext *engine);
void game_host_unload(GameHost *host, PlatformAPI *platform);
void game_host_tick(GameHost *host, PlatformAPI *platform, EngineContext *engine);

// render thread handoff (thread 0): pause returns once the render thread is idle
void engine_render_pause(EngineContext *engine);
void engine_render_resume(EngineContext *engine);

// shaders
void shaders_tick(EngineContext *engine); // thread 0, start of frame

// engine helpers
void inspector_build(EngineContext *engine);

// game-facing helpers
ENGINE_API void engine_request_quit(EngineContext *engine);

// Thread 0 only. 'relative_path' is relative to assets_root.
ENGINE_API ShaderHandle shader_load(EngineContext *engine, const char *relative_path,
                                    ShaderStage stage, const char *entry_point);