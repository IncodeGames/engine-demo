#include "engine/engine.h"
#include <stdio.h>

// Engine inspector using the immediate mode API. Runs on thread 0 after
// systems and physics, while every other thread is parked at the frame barrier,
// so it can read any engine state without synchronization. Toggle with F1.

static void inspector_arena(UIContext *ui, const char *name, const Arena *a)
{
    char text[128];
    snprintf(text, sizeof(text), "%s  %.1f / %.1f MB", name,
             (f64)a->used / (1024.0 * 1024.0), (f64)a->size / (1024.0 * 1024.0));
    ui_meter(ui, a->size ? (f32)((f64)a->used / (f64)a->size) : 0.0f, text);
}

void inspector_build(EngineContext *e)
{
    if (!e->config.ui_enabled)
    {
        return;
    }

    UIContext *ui = e->ui;
    if (input_key_pressed(e, KEY_F1))
    {
        e->inspector_open = !e->inspector_open;
    }

    if (!e->inspector_open)
    {
        return;
    }

    if (ui_window_begin(ui, "Inspector (F1)", 16.0f, 16.0f, 400.0f, 600.0f))
    {
        static const char *reload_names[] = {
            "loaded at startup",
            "code swapped, state kept",
            "layout changed, state was reset",
            "reload FAILED (see log), old code running",
            "RESTART REQUIRED: shut down, full rebuild"};
        ui_labelf(ui, "%.2f ms   %.0f fps   frame %llu   %u threads",
                  e->stat_last_ms, e->stat_last_fps, (unsigned long long)e->frame_index, e->thread_count);
        ui_labelf(ui, "game code: %s", reload_names[e->game_host.reload_status]);

        if (ui_tree_begin(ui, "Build"))
        {
            static const char *status_names[] = {"idle", "building...", "ok", "FAILED"};
            DevBuild *b = &e->dev_build;
            ShaderLibrary *sl = &e->shaders;
            ui_labelf(ui, "watching: %s", e->config.game_source_root[0] ? e->config.game_source_root : "off (game_source_root not set)");
            ui_row_begin(ui);
            if (ui_button(ui, "Rebuild game (F5)").clicked)
            {
                dev_build_start(e);
            }
            if (ui_button(ui, "Reload shaders (F6)").clicked)
            {
                sl->reload_all_requested = true; // picked up by shaders_tick next frame
            }
            ui_row_end(ui);
            ui_labelf(ui, "game: %s  %.1f s", status_names[b->status], b->seconds);
            ui_labelf(ui, "shaders: %u files   last reload: %u ok, %u failed", sl->count, sl->last_ok, sl->last_failed);
            u32 shown = Min(b->line_count, 12u);
            for (u32 i = b->line_count - shown; i < b->line_count; ++i)
            {
                ui_label(ui, b->lines[i % DEV_BUILD_LOG_LINES]);
            }
            ui_tree_end(ui);
        }

        if (ui_tree_begin(ui, "Memory"))
        {
            inspector_arena(ui, "persistent", &e->memory.persistent);
            inspector_arena(ui, "ecs", &e->ecs_arena);
            inspector_arena(ui, "game", &e->game_arena);
            inspector_arena(ui, "systems", &e->systems_arena);
            inspector_arena(ui, "renderer", &e->renderer_arena);
            inspector_arena(ui, "upload[0]", &e->render.lists[0].upload);
            inspector_arena(ui, "upload[1]", &e->render.lists[1].upload);
            inspector_arena(ui, "ui", &e->ui_arena);
            if (ui_tree_begin(ui, "Per-thread frame arenas"))
            {
                for (u32 i = 0; i < e->thread_count; ++i)
                {
                    char name[32];
                    snprintf(name, sizeof(name), "frame[%u]", i);
                    inspector_arena(ui, name, &e->memory.frame[i]);
                }
                ui_tree_end(ui);
            }
            ui_tree_end(ui);
        }

        if (ui_tree_begin(ui, "ECS"))
        {
            World *w = e->world;
            ui_labelf(ui, "entities %u / %u   component types %u",
                      w->next_index - 1 - w->free_count, w->max_entities, w->component_type_count);
            for (u32 i = 0; i < w->archetype_count; ++i)
            {
                Archetype *a = &w->archetypes[i];
                char text[96];
                snprintf(text, sizeof(text), "archetype %u  (%u columns)  %u / %u rows",
                         i, a->component_count, a->count, a->capacity);
                ui_meter(ui, a->capacity ? (f32)a->count / (f32)a->capacity : 0.0f, text);
            }
            ui_tree_end(ui);
        }

        if (ui_tree_begin(ui, "Systems"))
        {
            static const char *schedule_names[] = {"main", "static", "dynamic"};
            SystemRegistry *reg = &e->systems;
            f64 ms_per_tick = 1000.0 / (f64)e->platform->time_frequency();
            for (u32 i = 0; i < reg->active_count; ++i)
            {
                System *s = &reg->systems[reg->order_index[i]];
                ui_labelf(ui, "%4d  %-12s %-7s %8u rows  %6.3f ms", s->desc.order,
                          s->desc.name ? s->desc.name : "?", schedule_names[s->desc.schedule],
                          s->total_rows, (f64)s->last_ticks * ms_per_tick);
            }
            if (ui_tree_begin(ui, "Rows per thread"))
            {
                u32 busiest = 1;
                for (u32 t = 0; t < reg->thread_count; ++t)
                {
                    busiest = Max(busiest, reg->thread_stats[t].rows);
                }
                for (u32 t = 0; t < reg->thread_count; ++t)
                {
                    char text[64];
                    snprintf(text, sizeof(text), "t%02u  %u rows  %u chunks", t,
                             reg->thread_stats[t].rows, reg->thread_stats[t].chunks);
                    ui_meter(ui, (f32)reg->thread_stats[t].rows / (f32)busiest, text);
                }
                ui_tree_end(ui);
            }
            ui_tree_end(ui);
        }

        if (ui_tree_begin(ui, "UI"))
        {
            const UIStats *st = ui_stats(ui);
            ui_labelf(ui, "boxes %u (cached %u)   quads %u   batches %u",
                      st->boxes_built, st->boxes_cached, st->quads, st->batches);
            ui_labelf(ui, "retained nodes %u", st->nodes_live);
            if (st->key_collisions || st->pool_overflows || st->quads_dropped)
                ui_labelf(ui, "! key collisions %u  pool overflows %u  dropped quads %u",
                          st->key_collisions, st->pool_overflows, st->quads_dropped);
            ui_tree_end(ui);
        }
    }
    ui_window_end(ui);
}