#include "ui/ui_internal.h"
#include <string.h>
#include <math.h>

#define UI_ROOT_SEED 0x9E3779B97F4A7C15ull

// ----------------------------------- keys ----------------------------------
u64 ui_hash_bytes(u64 seed, const void *data, usize len)
{
    const u8 *p = (const u8 *)data;
    u64 h = 14695981039346656037ull ^ seed;
    for (usize i = 0; i < len; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h ? h : 1; // 0 is reserved for "no key"
}

UIKey ui_key_from_u64(u64 seed, u64 id)
{
    UIKey k;
    k.v = ui_hash_bytes(seed, &id, sizeof(id));
    return k;
}

void ui_id_push_raw(UIContext *ui, u64 seed)
{
    ASSERT(ui->id_count < UI_ID_STACK_MAX);
    ui->id_stack[ui->id_count++] = seed;
}

ENGINE_API UIKey ui_key(UIContext *ui, const char *string)
{
    UIKey k;
    k.v = ui_hash_bytes(ui->id_stack[ui->id_count - 1], string, strlen(string));
    return k;
}

ENGINE_API void ui_push_id(UIContext *ui, u64 id)
{
    ui_id_push_raw(ui, ui_hash_bytes(ui->id_stack[ui->id_count - 1], &id, sizeof(id)));
}

ENGINE_API void ui_pop_id(UIContext *ui)
{
    ASSERT(ui->id_count > 1);
    ui->id_count--;
}

// --------------------------------- context ---------------------------------
static UIColor ui_rgba(f32 r, f32 g, f32 b, f32 a)
{
    UIColor c = {r, g, b, a};
    return c;
}

static void ui_theme_default(UITheme *t)
{
    t->window_bg = ui_rgba(0.09f, 0.10f, 0.12f, 0.94f);
    t->title_bg = ui_rgba(0.16f, 0.18f, 0.23f, 1.00f);
    t->border = ui_rgba(0.30f, 0.33f, 0.40f, 1.00f);
    t->widget_bg = ui_rgba(0.20f, 0.22f, 0.27f, 1.00f);
    t->widget_hot = ui_rgba(0.28f, 0.32f, 0.40f, 1.00f);
    t->widget_active = ui_rgba(0.36f, 0.46f, 0.62f, 1.00f);
    t->accent = ui_rgba(0.26f, 0.50f, 0.82f, 1.00f);
    t->text = ui_rgba(0.90f, 0.91f, 0.93f, 1.00f);
    t->row_height = 22.0f;
    t->padding = 6.0f;
    t->gap = 4.0f;
    t->corner_radius = 3.0f;
    t->indent = 14.0f;
}

UIContext *ui_create(Arena *arena, const UIDesc *desc)
{
    ASSERT(desc->max_boxes > 0 && desc->max_quads > 0 && desc->build_arena_size > 0);
    UIContext *ui = PushStruct(arena, UIContext);
    ui->arena = arena;
    ui->max_quads = desc->max_quads;

    ui->box_capacity = desc->max_boxes;
    ui->box_pool = PushArray(arena, UIBox, desc->max_boxes);
    ui->box_live = PushArray(arena, UIBox *, desc->max_boxes);
    for (u32 i = desc->max_boxes; i-- > 0;)
    {
        ui->box_pool[i].hash_next = ui->box_free;
        ui->box_free = &ui->box_pool[i];
    }
    u32 table_size = 1;
    while (table_size < desc->max_boxes * 2)
    {
        table_size <<= 1;
    }
    ui->box_table = PushArray(arena, UIBox *, table_size);
    ui->box_table_mask = table_size - 1;

    ui_nodes_init(ui, arena, desc->max_nodes);
    ui_theme_default(&ui->theme);
    ui->font.atlas_pixels = PushArray(arena, u8, UI_FONT_ATLAS_SIZE * UI_FONT_ATLAS_SIZE);
    ui_font_init_fallback(&ui->font, 15.0f);

    void *build_mem = arena_push(arena, desc->build_arena_size, 16);
    arena_init(&ui->build, build_mem, desc->build_arena_size, "ui-build");
    return ui;
}

ENGINE_API UITheme *ui_theme(UIContext *ui) { return &ui->theme; }
ENGINE_API const UIStats *ui_stats(UIContext *ui) { return &ui->stats_prev; }
ENGINE_API b32 ui_wants_mouse(UIContext *ui) { return ui->hot_key.v != 0 || ui->active_key.v != 0; }
ENGINE_API void ui_next_width(UIContext *ui, UISize size) { ui->next_size[UI_AXIS_X] = size; }
ENGINE_API void ui_next_height(UIContext *ui, UISize size) { ui->next_size[UI_AXIS_Y] = size; }

// -------------------------------- box cache --------------------------------
UIBox *ui_box_lookup(UIContext *ui, UIKey key)
{
    if (!key.v)
    {
        return 0;
    }

    for (UIBox *b = ui->box_table[key.v & ui->box_table_mask]; b; b = b->hash_next)
    {
        if (b->key.v == key.v)
        {
            return b;
        }
    }
    return 0;
}

// TODO(BPL): UI_BOX_KEEP_STATE boxes are never evicted. LRU them under pool pressure. 2026-07-04
static void ui_box_prune(UIContext *ui)
{
    for (u32 i = 0; i < ui->box_live_count;)
    {
        UIBox *b = ui->box_live[i];
        if (b->last_frame == ui->frame_index || (b->flags & UI_BOX_KEEP_STATE))
        {
            ++i;
            continue;
        }
        UIBox **link = &ui->box_table[b->key.v & ui->box_table_mask];
        while (*link != b)
            link = &(*link)->hash_next;
        *link = b->hash_next;
        b->hash_next = ui->box_free;
        ui->box_free = b;
        ui->box_live[i] = ui->box_live[--ui->box_live_count];
    }
}

ENGINE_API UIBox *ui_box_make(UIContext *ui, u32 flags, UIKey key)
{
    ASSERT(ui->building && "UI is built between ui_begin_frame and ui_end_frame, on thread 0");
    UIBox *box = ui_box_lookup(ui, key);
    if (box && box->last_frame == ui->frame_index) // same key twice in one frame
    {
        ui->stats.key_collisions++;
        box = 0;
        key.v = 0;
    }
    if (!box && key.v)
    {
        if (ui->box_free)
        {
            box = ui->box_free;
            ui->box_free = box->hash_next;
            memset(box, 0, sizeof(*box));
            box->key = key;
            box->first_frame = ui->frame_index;
            UIBox **bucket = &ui->box_table[key.v & ui->box_table_mask];
            box->hash_next = *bucket;
            *bucket = box;
            ui->box_live[ui->box_live_count++] = box;
        }
        else
        {
            ui->stats.pool_overflows++; // degrade to a stateless box rather than fail
        }
    }
    if (!box)
    {
        box = PushStruct(&ui->build, UIBox);
    }

    const UITheme *th = &ui->theme;
    box->parent = box->first = box->last = box->next = box->prev = 0;
    box->last_frame = ui->frame_index;
    box->flags = flags;
    box->layout_axis = UI_AXIS_Y;
    box->size[0] = ui->next_size[0];
    box->size[1] = ui->next_size[1];
    ui->next_size[0].kind = UI_SIZE_NULL;
    ui->next_size[1].kind = UI_SIZE_NULL;
    box->padding[0] = box->padding[1] = 0.0f;
    box->child_gap = 0.0f;
    box->text_pad = th->padding;
    box->text = 0;
    box->text_len = 0;
    box->corner_radius = 0.0f;
    box->border_thickness = 1.0f;
    box->background = th->widget_bg;
    box->border_color = th->border;
    box->text_color = th->text;

    UIBox *parent = ui->parent_count ? ui->parent_stack[ui->parent_count - 1] : 0;
    if (parent)
    {
        box->parent = parent;
        box->prev = parent->last;
        if (parent->last)
            parent->last->next = box;
        else
            parent->first = box;
        parent->last = box;
    }
    ui->stats.boxes_built++;
    return box;
}

ENGINE_API void ui_push_parent(UIContext *ui, UIBox *box)
{
    ASSERT(ui->parent_count < UI_PARENT_STACK_MAX);
    ui->parent_stack[ui->parent_count++] = box;
    ui_id_push_raw(ui, box->key.v ? box->key.v : ui->id_stack[ui->id_count - 1]);
}

ENGINE_API void ui_pop_parent(UIContext *ui)
{
    ASSERT(ui->parent_count > 1 && ui->id_count > 1);
    ui->parent_count--;
    ui->id_count--;
}

// ------------------------------- interaction -------------------------------
// Uses the hit test from the end of last frame plus this frame's input.
ENGINE_API UISignal ui_signal_from_box(UIContext *ui, UIBox *box)
{
    UISignal s = {0};
    s.box = box;
    if (!box->key.v || !(box->flags & UI_BOX_CLICKABLE))
    {
        return s;
    }
    const UIInput *in = &ui->input;
    const u32 left = PLATFORM_MOUSE_LEFT;
    b32 hot = (ui->hot_key.v == box->key.v);
    b32 active = (ui->active_key.v == box->key.v);
    if (hot && !ui->active_key.v && in->mouse_pressed[left])
    {
        ui->active_key = box->key;
        active = true;
        s.pressed = true;
    }
    if (active)
    {
        if (in->mouse_down[left])
        {
            s.held = true;
            if (!s.pressed)
            {
                s.drag_dx = in->mouse_dx;
                s.drag_dy = in->mouse_dy;
            }
        }
        else
        {
            s.released = true;
            s.clicked = hot;
            ui->active_key.v = 0;
        }
    }
    s.hovered = hot && (s.held || !ui->active_key.v);
    box->hot_t += ((s.hovered ? 1.0f : 0.0f) - box->hot_t) * ui->anim_rate;
    box->active_t += ((s.held ? 1.0f : 0.0f) - box->active_t) * ui->anim_rate;
    return s;
}

// ---------------------------------- frame ----------------------------------
void ui_begin_frame(UIContext *ui, const UIInput *in)
{
    ui->frame_index++;
    ui->stats_prev = ui->stats;
    memset(&ui->stats, 0, sizeof(ui->stats));
    arena_reset(&ui->build);
    ui->input = *in;
    ui->anim_rate = 1.0f - expf(-20.0f * (in->dt > 0.0f ? in->dt : 0.0f));
    ui->draw_boxes = 0;
    ui->draw_box_count = 0;
    ui->batches = 0;
    ui->batch_count = 0;
    ui->quad_count = 0;
    ui->quad_dst = 0;

    if (!in->mouse_down[PLATFORM_MOUSE_LEFT] && !in->mouse_released[PLATFORM_MOUSE_LEFT])
    {
        ui->active_key.v = 0;
    }

    // The wheel goes to the scroll container last frame's hit test found under the cursor
    if (in->mouse_wheel != 0.0f)
    {
        UIBox *target = ui_box_lookup(ui, ui->scroll_key);
        if (target)
        {
            target->scroll[UI_AXIS_Y] -= in->mouse_wheel * 3.0f * ui->theme.row_height;
        }
    }

    ui->parent_count = 0;
    ui->id_count = 0;
    ui->id_stack[ui->id_count++] = UI_ROOT_SEED;
    ui->next_size[0].kind = UI_SIZE_NULL;
    ui->next_size[1].kind = UI_SIZE_NULL;
    ui->building = true;

    UIKey none = {0};
    UIBox *root = ui_box_make(ui, 0, none);
    root->size[UI_AXIS_X] = ui_px(in->window_w);
    root->size[UI_AXIS_Y] = ui_px(in->window_h);
    ui->root = root;
    ui->parent_stack[ui->parent_count++] = root;
}

void ui_end_frame(UIContext *ui)
{
    ui_nodes_emit_roots(ui);
    if (ui->parent_count != 1)
    {
        ASSERT(!"unbalanced ui_push_parent / *_begin without *_end");
        ui->parent_count = 1;
        ui->id_count = 1;
    }
    ui->building = false;
    ui_layout(ui);
    ui_draw_flatten(ui); // draw order, scissor batches, quad offsets, hit test
    ui_box_prune(ui);
    ui->stats.boxes_cached = ui->box_live_count;
    ui->stats.quads = ui->quad_count;
    ui->stats.batches = ui->batch_count;
    ui->stats.nodes_live = ui->node_live;
}