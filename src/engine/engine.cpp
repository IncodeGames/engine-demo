#include "engine/engine.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// --------------------------------- config ----------------------------------
static usize parse_size(const char *s)
{
    usize v = 0;
    while (*s >= '0' && *s <= '9')
    {
        v = v * 10 + (usize)(*s - '0');
        ++s;
    }
    while (*s == ' ' || *s == '\t')
        ++s;
    if (*s == 'K' || *s == 'k')
        return KB(v);
    if (*s == 'M' || *s == 'm')
        return MB(v);
    if (*s == 'G' || *s == 'g')
        return GB(v);
    return v;
}

static void trim(const char **b, const char **e)
{
    while (*b < *e && (**b == ' ' || **b == '\t' || **b == '\r'))
        (*b)++;
    while (*e > *b && ((*e)[-1] == ' ' || (*e)[-1] == '\t' || (*e)[-1] == '\r'))
        (*e)--;
}

void config_defaults(EngineConfig *c)
{
    memset(c, 0, sizeof(*c));
    strcpy(c->build_game_command, "..\\build\\build.bat game nopause");
    c->persistent_arena_size = MB(128);
    c->ecs_arena_size = MB(256);
    c->game_arena_size = MB(32);
    c->systems_arena_size = MB(8);
    c->frame_arena_size = MB(16);
    c->scratch_arena_size = MB(4);
    c->renderer_arena_size = MB(64);
    c->upload_arena_size = MB(96);
    c->job_queue_capacity = 65536;
    c->reserved_thread_count = 1;
    c->max_entities = 1u << 21;
    strcpy(c->window_title, "Engine");
    c->window_width = 1600;
    c->window_height = 900;
    strcpy(c->assets_root, "data");
    c->ui_enabled = true;
    c->ui_arena_size = MB(32);
    strcpy(c->ui_font_path, "C:/Windows/Fonts/arial.ttf");
    c->ui_font_size = 15.0f;
}

b32 config_load(EngineConfig *out, PlatformAPI *platform, Arena *scratch, const char *path)
{
    config_defaults(out);
    PlatformFileContents file = platform->file_read(path, scratch);
    if (!file.ok)
        return false;

    const char *p = (const char *)file.data;
    const char *end = p + file.size;
    while (p < end)
    {
        const char *ls = p;
        while (p < end && *p != '\n')
            ++p;
        const char *le = p;
        if (p < end)
            ++p;
        trim(&ls, &le);
        if (ls >= le || *ls == '#')
            continue;

        const char *eq = ls;
        while (eq < le && *eq != '=')
            ++eq;
        if (eq >= le)
            continue;

        const char *kb = ls;
        const char *ke = eq;
        const char *vb = eq + 1;
        const char *ve = le;
        trim(&kb, &ke);
        trim(&vb, &ve);

        char key[64];
        char val[256];
        usize kn = Min((usize)(ke - kb), sizeof(key) - 1);
        usize vn = Min((usize)(ve - vb), sizeof(val) - 1);
        memcpy(key, kb, kn);
        key[kn] = 0;
        memcpy(val, vb, vn);
        val[vn] = 0;

        if (!strcmp(key, "persistent_arena_size"))
            out->persistent_arena_size = parse_size(val);
        else if (!strcmp(key, "ecs_arena_size"))
            out->ecs_arena_size = parse_size(val);
        else if (!strcmp(key, "game_arena_size"))
            out->game_arena_size = parse_size(val);
        else if (!strcmp(key, "systems_arena_size"))
            out->systems_arena_size = parse_size(val);
        else if (!strcmp(key, "frame_arena_size"))
            out->frame_arena_size = parse_size(val);
        else if (!strcmp(key, "scratch_arena_size"))
            out->scratch_arena_size = parse_size(val);
        else if (!strcmp(key, "renderer_arena_size"))
            out->renderer_arena_size = parse_size(val);
        else if (!strcmp(key, "upload_arena_size"))
            out->upload_arena_size = parse_size(val);
        else if (!strcmp(key, "job_queue_capacity"))
            out->job_queue_capacity = (u32)strtoul(val, 0, 10);
        else if (!strcmp(key, "reserved_thread_count"))
            out->reserved_thread_count = (u32)atoi(val);
        else if (!strcmp(key, "max_entities"))
            out->max_entities = (u32)strtoul(val, 0, 10);
        else if (!strcmp(key, "window_width"))
            out->window_width = (u32)atoi(val);
        else if (!strcmp(key, "window_height"))
            out->window_height = (u32)atoi(val);
        else if (!strcmp(key, "window_title"))
            strncpy(out->window_title, val, sizeof(out->window_title) - 1);
        else if (!strcmp(key, "assets_root"))
            strncpy(out->assets_root, val, sizeof(out->assets_root) - 1);
        else if (!strcmp(key, "assets_source_root"))
            strncpy(out->assets_source_root, val, sizeof(out->assets_source_root) - 1);
        else if (!strcmp(key, "ui_enabled"))
            out->ui_enabled = atoi(val) != 0;
        else if (!strcmp(key, "ui_arena_size"))
            out->ui_arena_size = parse_size(val);
        else if (!strcmp(key, "ui_font_path"))
            strncpy(out->ui_font_path, val, sizeof(out->ui_font_path) - 1);
        else if (!strcmp(key, "ui_font_size"))
            out->ui_font_size = (f32)atof(val);
        else if (!strcmp(key, "build_game_command"))
            strncpy(out->build_game_command, val, sizeof(out->build_game_command) - 1);
        else if (!strcmp(key, "game_source_root"))
            strncpy(out->game_source_root, val, sizeof(out->game_source_root) - 1);
    }
    return true;
}

ENGINE_API void engine_request_quit(EngineContext *e) { e->quit_requested = true; }

// ------------------------------ render thread ------------------------------
static void engine_acquire_render_list(EngineContext *e, u32 list_idx)
{
    PlatformAPI *p = e->platform;
    EngineRenderState *rs = &e->render;
    while (!p->semaphore_wait_timeout(rs->buffer_free[list_idx], 2000))
    {
        u32 stage = rs->stage, submitted = rs->submit_count, consumed = rs->consume_count;
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "engine: waited 2s for render list %u. render thread: %s; submitted %u, consumed %u%s",
                 list_idx, RENDER_STAGE_NAMES[stage], submitted, consumed,
                 (stage == RENDER_STAGE_IDLE && submitted == consumed)
                     ? "  <-- nothing in flight: a list token was lost (handoff bug, not the GPU)"
                     : "");
        p->log(msg);
    }
}

void engine_render_pause(EngineContext *e)
{
    engine_acquire_render_list(e, 0);
    engine_acquire_render_list(e, 1);
}

void engine_render_resume(EngineContext *e)
{
    e->platform->semaphore_signal(e->render.buffer_free[0], 1);
    e->platform->semaphore_signal(e->render.buffer_free[1], 1);
}

static void render_thread_main(void *user)
{
    EngineContext *e = (EngineContext *)user;
    PlatformAPI *p = e->platform;
    EngineRenderState *rs = &e->render;
    for (;;)
    {
        rs->stage = RENDER_STAGE_IDLE;
        p->semaphore_wait(rs->ready);
        p->sleep_ms((u32)2);
        if (rs->quit)
            break;
        u32 idx = rs->submit_queue[rs->consume_count & 1]; // the frame this wake-up is for
        rs->stage = RENDER_STAGE_EXECUTE;
        e->renderer_api.execute(e->renderer, &rs->lists[idx]);
        rs->stage = RENDER_STAGE_PRESENT;
        e->renderer_api.present(e->renderer);
        rs->consume_count++;
        if (!p->semaphore_signal(rs->buffer_free[idx], 1))
            p->log("render: list returned twice (handoff protocol violated)");
    }
    rs->stage = RENDER_STAGE_EXITED;
}

// ------------------------------- engine ------------------------------------
static void engine_init(EngineContext *e)
{
    PlatformAPI *p = e->platform;
    Arena *persistent = &e->memory.persistent;

    p->window_open(e->config.window_title, e->config.window_width, e->config.window_height);

    e->world = ecs_world_create(&e->ecs_arena, e->config.max_entities);
    e->jobs = jobs_create(persistent, e->config.job_queue_capacity);
    input_init(&e->input, persistent);
    systems_init(&e->systems, persistent, e->thread_count);

    u32 w = 0, h = 0;
    p->window_size(&w, &h);
    renderer_d3d11_get_api(&e->renderer_api);
    e->renderer = e->renderer_api.create(persistent);
    RendererInitDesc rd = {0};
    rd.native_window_handle = p->window_handle();
    rd.width = w;
    rd.height = h;
    rd.worker_thread_count = e->thread_count;
    rd.max_commands_per_thread = RENDER_MAX_COMMANDS_PER_THREAD;
    rd.platform = p;
    rd.persistent = persistent;
    e->renderer_api.init(e->renderer, &rd);

    UIDesc ud = {0};
    ud.max_boxes = 16384;
    ud.max_nodes = 4096;
    ud.max_quads = 65536;
    ud.build_arena_size = e->config.ui_arena_size / 2;
    e->ui = ui_create(&e->ui_arena, &ud);
    e->ui->platform = p;
    if (e->config.ui_enabled)
    {
        ui_font_load(e->ui, p, e->config.ui_font_path, e->config.ui_font_size);
        ui_render_init(e->ui, &e->renderer_api, e->renderer);
        e->inspector_open = true;
    }

    audio_stub_get_api(&e->audio_api);
    e->audio = e->audio_api.create(persistent);
    e->audio_api.init(e->audio);

    physics_stub_get_api(&e->physics_api);
    e->physics = e->physics_api.create(persistent);
    e->physics_api.init(e->physics);

    e->assets = assets_create(persistent, p, e->config.assets_root);

    usize per_list_upload = e->config.upload_arena_size / 2;
    render_list_init(&e->render.lists[0], &e->renderer_arena, &e->upload_arena,
                     e->thread_count, RENDER_MAX_COMMANDS_PER_THREAD, per_list_upload);
    render_list_init(&e->render.lists[1], &e->renderer_arena, &e->upload_arena,
                     e->thread_count, RENDER_MAX_COMMANDS_PER_THREAD, per_list_upload);
    e->render.ready = p->semaphore_create(0, 2);
    e->render.buffer_free[0] = p->semaphore_create(1, 1);
    e->render.buffer_free[1] = p->semaphore_create(1, 1);
    e->render.quit = 0;
    e->render.thread = p->thread_start(render_thread_main, e);

    game_host_load(&e->game_host, p, e);
    e->dev_build.built_source_time = e->game_host.dll_modified_time;
    if (e->game_host.loaded && e->game_host.api.init)
        e->game_state = e->game_host.api.init(e);

    e->last_ticks = p->time_ticks();
    e->frame_index = 0;
}

static b32 engine_begin_frame(EngineContext *e)
{
    PlatformAPI *p = e->platform;
    b32 running = p->window_pump() && !e->quit_requested;

    input_begin_frame(&e->input, p);

    u64 now = p->time_ticks();
    u64 freq = p->time_frequency();
    e->dt = (f32)((f64)(now - e->last_ticks) / (f64)freq);
    if (e->dt > 0.1f)
        e->dt = 0.1f;
    e->last_ticks = now;

    e->stat_accum_seconds += e->dt;
    e->stat_frame_count++;
    if (e->stat_accum_seconds >= 1.0)
    {
        f64 avg_ms = 1000.0 * e->stat_accum_seconds / (f64)e->stat_frame_count;
        f64 fps = (f64)e->stat_frame_count / e->stat_accum_seconds;
        char buf[160];
        snprintf(buf, sizeof(buf), "frame: %.3f ms  (%.1f fps)  [frame %llu]",
                 avg_ms, fps, (unsigned long long)e->frame_index);
        p->log(buf);
        e->stat_accum_seconds = 0.0;
        e->stat_frame_count = 0;
        e->stat_last_ms = (f32)avg_ms;
        e->stat_last_fps = (f32)fps;
    }

    dev_build_tick(e);
    game_host_tick(&e->game_host, p, e);
    shaders_tick(e);
    assets_poll_hot_reload(e->assets);
    e->audio_api.update(e->audio);

    {
        UIInput in = {0};
        u32 w = 0, h = 0;
        p->window_size(&w, &h);
        in.mouse_x = (f32)e->input.mouse_x;
        in.mouse_y = (f32)e->input.mouse_y;
        in.mouse_dx = (f32)e->input.mouse_dx;
        in.mouse_dy = (f32)e->input.mouse_dy;
        in.mouse_wheel = e->input.mouse_wheel;
        for (u32 b = 0; b < PLATFORM_MOUSE_BUTTON_COUNT; ++b)
        {
            in.mouse_down[b] = e->input.mouse_down[b];
            in.mouse_pressed[b] = e->input.mouse_pressed[b];
            in.mouse_released[b] = e->input.mouse_released[b];
        }
        in.dt = e->dt;
        in.window_w = w;
        in.window_h = h;
        ui_begin_frame(e->ui, &in);
    }
    return running;
}

static void engine_shutdown(EngineContext *e)
{
    PlatformAPI *p = e->platform;
    if (e->game_host.loaded && e->game_host.api.shutdown)
        e->game_host.api.shutdown(e, e->game_state);
    game_host_unload(&e->game_host, p);

    e->render.quit = 1;
    p->semaphore_signal(e->render.ready, 1);
    p->thread_join(e->render.thread);
    p->semaphore_destroy(e->render.ready);
    p->semaphore_destroy(e->render.buffer_free[0]);
    p->semaphore_destroy(e->render.buffer_free[1]);

    e->renderer_api.shutdown(e->renderer);
    e->audio_api.shutdown(e->audio);
    e->physics_api.shutdown(e->physics);

    p->process_release(&e->dev_build.process);
}

// --------------------------------- workers ---------------------------------
typedef struct WorkerArgs
{
    u32 thread_index;
    u32 thread_count;
    PlatformAPI *platform;
    EngineContext *engine;
    PlatformBarrier *barrier;
} WorkerArgs;

void EntryPoint(u32 ti, u32 tc, PlatformAPI *p, EngineContext *e, PlatformBarrier *barrier)
{
    ThreadContext thread;
    thread.thread_index = ti;
    thread.thread_count = tc;
    thread.frame = &e->memory.frame[ti];
    thread.scratch = &e->memory.scratch[ti];

    if (ti == 0)
    {
        engine_init(e);
        e->running = true;
    }
    p->barrier_wait(barrier);

    for (;;)
    {
        if (ti == 0)
            e->running = engine_begin_frame(e);
        p->barrier_wait(barrier);
        if (!e->running)
            break;

        arena_reset(&e->memory.frame[ti]);

        if (e->game_host.api.update)
            e->game_host.api.update(e, &thread, e->game_state);
        p->barrier_wait(barrier);

        systems_run_all(e, ti, tc);
        if (ti == 0)
        {
            e->physics_api.step(e->physics, e->dt);
            inspector_build(e);  // world is quiescent: read anything
            ui_end_frame(e->ui); // retained replay -> layout -> draw list -> hit test
        }

        p->barrier_wait(barrier);
        u32 list_idx = (u32)(e->frame_index & 1);
        if (ti == 0)
        {
            engine_acquire_render_list(e, list_idx);
            render_list_reset(&e->render.lists[list_idx]);
            ui_render_prepare(e->ui, &e->render.lists[list_idx]);
        }

        p->barrier_wait(barrier);
        if (e->game_host.api.build_render_list)
        {
            e->game_host.api.build_render_list(e, &thread, e->game_state, &e->render.lists[list_idx]);
        }
        ui_render_gather(e->ui); // every thread pulls chunks of boxes and writes quads
        p->barrier_wait(barrier);

        if (ti == 0)
        {
            EngineRenderState *rs = &e->render;
            rs->submit_queue[rs->submit_count & 1] = list_idx;
            rs->submit_count++;
            if (!p->semaphore_signal(rs->ready, 1))
                p->log("engine: render submit failed (more than two frames queued)");
            e->frame_index++;
        }
        p->barrier_wait(barrier);
    }

    p->barrier_wait(barrier);
    if (ti == 0)
        engine_shutdown(e);
}

static void worker_thread_main(void *user)
{
    WorkerArgs *a = (WorkerArgs *)user;
    EntryPoint(a->thread_index, a->thread_count, a->platform, a->engine, a->barrier);
}

// ------------------------------------------------------------ bootstrap ----
void BootstrapEntryPoint(PlatformAPI *platform)
{
    usize boot_size = MB(2);
    void *boot_mem = platform->mem_reserve(boot_size);
    if (!boot_mem || !platform->mem_commit(boot_mem, boot_size))
    {
        platform->log("fatal: bootstrap alloc failed");
        return;
    }
    Arena boot;
    arena_init(&boot, boot_mem, boot_size, "bootstrap");

    EngineConfig cfg;
    if (!config_load(&cfg, platform, &boot, "config/engine.cfg"))
        platform->log("config/engine.cfg not found; using defaults");

    u32 cores = platform->logical_core_count();
    u32 reserved = (cfg.reserved_thread_count > cores - 1) ? cores - 1 : cfg.reserved_thread_count;
    u32 workers = cores > reserved ? cores - reserved : 1;

    usize per_thread = cfg.frame_arena_size + cfg.scratch_arena_size;
    usize total = cfg.persistent_arena_size + cfg.ecs_arena_size + cfg.game_arena_size +
                  cfg.systems_arena_size + cfg.renderer_arena_size + cfg.upload_arena_size +
                  cfg.ui_arena_size + per_thread * workers;

    void *block = platform->mem_reserve(total);
    if (!block || !platform->mem_commit(block, total))
    {
        platform->log("fatal: engine alloc failed");
        return;
    }

    u8 *cur = (u8 *)block;
    Arena persistent;
    arena_init(&persistent, cur, cfg.persistent_arena_size, "persistent");
    cur += cfg.persistent_arena_size;
    Arena ecs_arena;
    arena_init(&ecs_arena, cur, cfg.ecs_arena_size, "ecs");
    cur += cfg.ecs_arena_size;
    Arena game_arena;
    arena_init(&game_arena, cur, cfg.game_arena_size, "game");
    cur += cfg.game_arena_size;
    Arena sys_arena;
    arena_init(&sys_arena, cur, cfg.systems_arena_size, "systems");
    cur += cfg.systems_arena_size;
    Arena rdr_arena;
    arena_init(&rdr_arena, cur, cfg.renderer_arena_size, "renderer");
    cur += cfg.renderer_arena_size;
    Arena upl_arena;
    arena_init(&upl_arena, cur, cfg.upload_arena_size, "upload");
    cur += cfg.upload_arena_size;
    Arena ui_arena; // after upl_arena
    arena_init(&ui_arena, cur, cfg.ui_arena_size, "ui");
    cur += cfg.ui_arena_size;

    EngineContext *engine = PushStruct(&persistent, EngineContext);
    engine->platform = platform;
    engine->config = cfg;
    engine->thread_count = workers;
    engine->ecs_arena = ecs_arena;
    engine->game_arena = game_arena;
    engine->systems_arena = sys_arena;
    engine->renderer_arena = rdr_arena;
    engine->upload_arena = upl_arena;
    engine->ui_arena = ui_arena;
    engine->memory.frame = PushArray(&persistent, Arena, workers);
    engine->memory.scratch = PushArray(&persistent, Arena, workers);
    for (u32 i = 0; i < workers; ++i)
    {
        arena_init(&engine->memory.frame[i], cur, cfg.frame_arena_size, "frame");
        cur += cfg.frame_arena_size;
        arena_init(&engine->memory.scratch[i], cur, cfg.scratch_arena_size, "scratch");
        cur += cfg.scratch_arena_size;
    }
    engine->memory.persistent = persistent;

    PlatformBarrier *barrier = PushStruct(&engine->memory.persistent, PlatformBarrier);
    platform->barrier_init(barrier, workers);
    engine->frame_barrier = barrier;

    WorkerArgs *args = PushArray(&engine->memory.persistent, WorkerArgs, workers);
    PlatformThread *threads = PushArray(&engine->memory.persistent, PlatformThread, workers);
    for (u32 i = 0; i < workers; ++i)
    {
        args[i].thread_index = i;
        args[i].thread_count = workers;
        args[i].platform = platform;
        args[i].engine = engine;
        args[i].barrier = barrier;
    }

    for (u32 i = 1; i < workers; ++i)
    {
        threads[i] = platform->thread_start(worker_thread_main, &args[i]);
    }
    worker_thread_main(&args[0]);
    for (u32 i = 1; i < workers; ++i)
    {
        platform->thread_join(threads[i]);
    }

    platform->barrier_destroy(barrier);
    platform->mem_release(block, total);
    platform->mem_release(boot_mem, boot_size);
}