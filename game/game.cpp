#ifndef GAME_DLL_BUILD
#error "game.cpp must be compiled via game_unity.cpp"
#endif

#include "engine/engine.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define TRIANGLE_EDGE 100u // grid edge; total = EDGE * EDGE
#define TRIANGLE_COUNT (TRIANGLE_EDGE * TRIANGLE_EDGE)

typedef struct Transform2D
{
    f32 x, y;
    f32 angle;
    f32 scale;
} Transform2D;

typedef struct Motion
{
    f32 vx, vy;
    f32 spin;
} Motion;

typedef struct Instance
{
    f32 x, y;
    f32 angle;
    f32 scale;
} Instance; // per-instance GPU data

typedef struct TriVertex
{
    f32 pos[2];
    f32 color[4];
} TriVertex;

typedef struct GameState
{
    ComponentId transform_id;
    ComponentId motion_id;
    ArchetypeId archetype;
    u32 entity_count;

    ShaderHandle vertex_shader;
    ShaderHandle pixel_shader;
    PipelineHandle pipeline;
    BufferHandle geometry_buffer;
    BufferHandle instance_buffers[2];

    UINodeHandle hud_quit;
    b32 paused;
    f32 time_scale;
} GameState;

static u64 game_schema_hash(void)
{
    u64 h = 1469598103934665603ull;
    usize parts[] = {sizeof(GameState), sizeof(Transform2D), sizeof(Motion), sizeof(Instance), sizeof(TriVertex)};
    for (u32 i = 0; i < ArrayCount(parts); ++i)
    {
        h ^= parts[i];
        h *= 1099511628211ull;
    }
    return h;
}

static f32 hash01(u32 x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return (f32)(x & 0x00FFFFFFu) / (f32)0x01000000u;
}

// ------------------------- systems -------------------------
static void spawn_system(EngineContext *e, const SystemIter *it, void *user)
{
    GameState *gs = (GameState *)user;
    Transform2D *t = (Transform2D *)ecs_column(it->world, it->archetype, gs->transform_id);
    Motion *m = (Motion *)ecs_column(it->world, it->archetype, gs->motion_id);
    const f32 step = 2.0f / (f32)TRIANGLE_EDGE;
    for (u32 idx = it->begin; idx < it->end; ++idx)
    {
        u32 gx = idx % TRIANGLE_EDGE;
        u32 gy = idx / TRIANGLE_EDGE;
        t[idx].x = -1.0f + step * ((f32)gx + 0.5f);
        t[idx].y = -1.0f + step * ((f32)gy + 0.5f);
        t[idx].angle = hash01(idx * 3u + 0u) * 6.2831853f;
        t[idx].scale = step * (0.30f + 0.20f * hash01(idx * 3u + 1u));
        f32 dir = hash01(idx * 7u + 1u) * 6.2831853f;
        f32 spd = 0.02f + 0.10f * hash01(idx * 7u + 2u);
        m[idx].vx = cosf(dir) * spd;
        m[idx].vy = sinf(dir) * spd;
        m[idx].spin = (hash01(idx * 7u + 3u) - 0.5f) * 6.0f;
    }
}

static void motion_system(EngineContext *e, const SystemIter *it, void *user)
{
    GameState *gs = (GameState *)user;
    Transform2D *t = (Transform2D *)ecs_column(it->world, it->archetype, gs->transform_id);
    Motion *m = (Motion *)ecs_column(it->world, it->archetype, gs->motion_id);
    f32 dt = e->dt;
    for (u32 i = it->begin; i < it->end; ++i)
    {
        f32 x = t[i].x + m[i].vx * dt;
        f32 y = t[i].y + m[i].vy * dt;
        x += (x > 1.0f) ? -2.0f : ((x < -1.0f) ? 2.0f : 0.0f);
        y += (y > 1.0f) ? -2.0f : ((y < -1.0f) ? 2.0f : 0.0f);
        t[i].x = x;
        t[i].y = y;
        t[i].angle += m[i].spin * dt;
    }
}

static void register_game_systems(EngineContext *e, GameState *gs)
{
    SystemDesc d = {0};
    d.name = "motion";
    d.order = 10;
    d.query.include[d.query.include_count++] = gs->transform_id;
    d.query.include[d.query.include_count++] = gs->motion_id;
    d.schedule = SYSTEM_PARALLEL_DYNAMIC;
    d.fn = motion_system;
    d.user = gs;
    system_register(e, &d);
}

// ---------------------------- gpu resources ----------------------------
static void create_render_resources(EngineContext *e, GameState *gs)
{
    RendererAPI *r = &e->renderer_api;
    gs->vertex_shader = shader_load(e, "shaders/triangle.hlsl", SHADER_VERTEX, "VSMain");
    gs->pixel_shader = shader_load(e, "shaders/triangle.hlsl", SHADER_PIXEL, "PSMain");
    PipelineDesc pd = {0};
    pd.vertex_shader = gs->vertex_shader;
    pd.pixel_shader = gs->pixel_shader;
    pd.topology = PRIMITIVE_TRIANGLE_LIST;
    pd.attributes[pd.attribute_count++] = {"POSITION", 0, VERTEX_FORMAT_FLOAT2, 0, 0, false};
    pd.attributes[pd.attribute_count++] = {"COLOR", 0, VERTEX_FORMAT_FLOAT4, 0, 8, false};
    pd.attributes[pd.attribute_count++] = {"IPOS", 0, VERTEX_FORMAT_FLOAT2, 1, 0, true};
    pd.attributes[pd.attribute_count++] = {"IANGLE", 0, VERTEX_FORMAT_FLOAT, 1, 8, true};
    pd.attributes[pd.attribute_count++] = {"ISCALE", 0, VERTEX_FORMAT_FLOAT, 1, 12, true};
    gs->pipeline = r->create_pipeline(e->renderer, &pd);
    TriVertex verts[3] = {
        {{0.00f, 1.00f}, {1.0f, 0.3f, 0.2f, 1.0f}},
        {{0.87f, -0.50f}, {0.2f, 1.0f, 0.3f, 1.0f}},
        {{-0.87f, -0.50f}, {0.3f, 0.4f, 1.0f, 1.0f}},
    };
    BufferDesc gb = {0};
    gb.usage = BUFFER_VERTEX;
    gb.size = sizeof(verts);
    gb.initial_data = verts;
    gs->geometry_buffer = r->create_buffer(e->renderer, &gb);
    BufferDesc ib = {0};
    ib.usage = BUFFER_VERTEX;
    ib.size = (usize)TRIANGLE_COUNT * sizeof(Instance); // DEFAULT usage -> per-frame UpdateSubresource
    gs->instance_buffers[0] = r->create_buffer(e->renderer, &ib);
    gs->instance_buffers[1] = r->create_buffer(e->renderer, &ib);
}

static void create_hud(EngineContext *e, GameState *gs)
{
    UIContext *ui = e->ui;
    gs->time_scale = 1.0f;
    UINodeDesc win = {0};
    win.kind = UI_NODE_WINDOW;
    win.text = "Game";
    win.x = 440.0f;
    win.y = 16.0f;
    win.w = 280.0f;
    win.h = 160.0f;
    UINodeHandle hud = ui_node_create(ui, ui_node_root(), &win);
    UINodeDesc label = {0};
    label.kind = UI_NODE_LABEL;
    ui_node_set_textf(ui, ui_node_create(ui, hud, &label), "%u triangles", gs->entity_count);
    UINodeDesc pause = {0};
    pause.kind = UI_NODE_CHECKBOX;
    pause.text = "Paused";
    pause.bind = &gs->paused;
    ui_node_create(ui, hud, &pause);
    UINodeDesc speed = {0};
    speed.kind = UI_NODE_SLIDER;
    speed.text = "Time scale";
    speed.min = 0.0f;
    speed.max = 4.0f;
    speed.bind = &gs->time_scale;
    ui_node_create(ui, hud, &speed);
    UINodeDesc quit = {0};
    quit.kind = UI_NODE_BUTTON;
    quit.text = "Quit";
    gs->hud_quit = ui_node_create(ui, hud, &quit);
}

// ------------------------------ game api -------------------------------
static void *game_init(EngineContext *e)
{
    GameState *gs = PushStruct(&e->game_arena, GameState);

    gs->transform_id = ecs_register_component(e->world, "Transform2D", sizeof(Transform2D), alignof(Transform2D));
    gs->motion_id = ecs_register_component(e->world, "Motion", sizeof(Motion), alignof(Motion));
    gs->paused = false;

    ComponentId ids[] = {gs->transform_id, gs->motion_id};
    gs->archetype = ecs_register_archetype(e->world, ids, ArrayCount(ids), TRIANGLE_COUNT);

    ecs_entity_create_bulk(e->world, gs->archetype, TRIANGLE_COUNT);
    gs->entity_count = TRIANGLE_COUNT;
    create_render_resources(e, gs);
    SystemDesc spawn = {0}; // registered here only, not on reload
    spawn.name = "spawn";
    spawn.order = 0;
    spawn.query.include[spawn.query.include_count++] = gs->transform_id;
    spawn.query.include[spawn.query.include_count++] = gs->motion_id;
    spawn.schedule = SYSTEM_PARALLEL_DYNAMIC;
    spawn.fn = spawn_system;
    spawn.user = gs;
    spawn.run_once = true;
    system_register(e, &spawn);
    register_game_systems(e, gs);

    char msg[96];
    snprintf(msg, sizeof(msg), "game: spawned %u triangle entities", gs->entity_count);
    e->platform->log(msg);
    return gs;
}

static void game_reload(EngineContext *e, void *state)
{
    register_game_systems(e, (GameState *)state);
}

static void game_update(EngineContext *e, ThreadContext *thread, void *state)
{
    (void)state;
    if (thread->thread_index == 0 && input_key_pressed(e, KEY_ESCAPE))
    {
        engine_request_quit(e);
    }

    if (thread->thread_index == 0)
    {
        GameState *gs = (GameState *)state;
        if (ui_node_signal(e->ui, gs->hud_quit).clicked)
            engine_request_quit(e);
        if (ui_window_begin(e->ui, "Tweaks", 440.0f, 200.0f, 280.0f, 120.0f))
        {
            if (ui_button(e->ui, "Reset time scale").clicked)
                gs->time_scale = 1.0f;
            ui_labelf(e->ui, "dt %.2f ms", e->dt * 1000.0f);
        }
        ui_window_end(e->ui);
    }
}

static void game_build_render_list(EngineContext *e, ThreadContext *thread, void *state, RenderList *out)
{
    GameState *gs = (GameState *)state;
    u32 idx = (u32)(e->frame_index & 1);
    BufferHandle instance_buffer = gs->instance_buffers[idx];

    // Parallel gather -- each worker writes its slice of instance data and records
    // the upload + instanced draw for that slice.
    u32 begin, end;
    work_split(gs->entity_count, thread->thread_index, thread->thread_count, &begin, &end);
    u32 count = end - begin;
    if (count > 0)
    {
        Transform2D *t = (Transform2D *)ecs_column(e->world, gs->archetype, gs->transform_id);

        u32 upload_offset;
        Instance *dst = (Instance *)render_upload_alloc(out, (usize)count * sizeof(Instance), &upload_offset);
        for (u32 i = 0; i < count; ++i)
        {
            dst[i].x = t[begin + i].x;
            dst[i].y = t[begin + i].y;
            dst[i].angle = t[begin + i].angle;
            dst[i].scale = t[begin + i].scale;
        }

        RenderCommand *up = render_cmd_push(out, thread->thread_index, RENDER_CMD_UPDATE_BUFFER);
        up->sort_key = 100;
        up->update_buffer.buffer = instance_buffer.id;
        up->update_buffer.dst_offset = begin * (u32)sizeof(Instance);
        up->update_buffer.size = count * (u32)sizeof(Instance);
        up->update_buffer.upload_offset = upload_offset;

        RenderCommand *draw = render_cmd_push(out, thread->thread_index, RENDER_CMD_DRAW_INSTANCED);
        draw->sort_key = 200;
        draw->draw_instanced.vertex_count = 3;
        draw->draw_instanced.instance_count = count;
        draw->draw_instanced.first_vertex = 0;
        draw->draw_instanced.first_instance = begin;
    }

    if (thread->thread_index == 0)
    {
        u32 w = 0, h = 0;
        e->platform->window_size(&w, &h);

        RenderCommand *vp = render_cmd_push(out, 0, RENDER_CMD_SET_VIEWPORT);
        vp->sort_key = 0;
        vp->viewport.w = (f32)w;
        vp->viewport.h = (f32)h;

        RenderCommand *clear = render_cmd_push(out, 0, RENDER_CMD_CLEAR);
        clear->sort_key = 1;
        clear->clear.clear_color = true;
        clear->clear.color[0] = 0.02f;
        clear->clear.color[1] = 0.02f;
        clear->clear.color[2] = 0.04f;
        clear->clear.color[3] = 1.0f;

        RenderCommand *pipe = render_cmd_push(out, 0, RENDER_CMD_SET_PIPELINE);
        pipe->sort_key = 2;
        pipe->pipeline.pipeline = gs->pipeline.id;

        RenderCommand *vb0 = render_cmd_push(out, 0, RENDER_CMD_SET_VERTEX_BUFFER);
        vb0->sort_key = 3;
        vb0->vertex_buffer.buffer = gs->geometry_buffer.id;
        vb0->vertex_buffer.slot = 0;
        vb0->vertex_buffer.stride = sizeof(TriVertex);

        RenderCommand *vb1 = render_cmd_push(out, 0, RENDER_CMD_SET_VERTEX_BUFFER);
        vb1->sort_key = 4;
        vb1->vertex_buffer.buffer = instance_buffer.id;
        vb1->vertex_buffer.slot = 1;
        vb1->vertex_buffer.stride = sizeof(Instance);
    }
}

static void game_shutdown(EngineContext *e, void *state)
{
    (void)state;
    e->platform->log("game: shutdown");
}

GAME_API void GetGameAPI(GameAPI *out)
{
    out->version = GAME_API_VERSION;
    out->engine_abi_hash = engine_abi_hash();
    out->schema_hash = game_schema_hash();
    out->init = game_init;
    out->reload = game_reload;
    out->update = game_update;
    out->build_render_list = game_build_render_list;
    out->shutdown = game_shutdown;
}