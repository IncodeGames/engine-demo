#include "ui/ui_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Retained mode = a persistent description tree, replayed through the builders in
// ui_widgets.cpp once per frame. Nodes hold data only (no callbacks), so they
// survive a game code swap untouched; the game polls ui_node_signal().

#define UI_RETAINED_SEED 0x52455441494E4544ull

void ui_nodes_init(UIContext *ui, Arena *arena, u32 capacity)
{
    ASSERT(capacity > 1 && capacity <= 65536);
    ui->nodes = PushArray(arena, UINode, capacity);
    ui->node_capacity = capacity;
    ui->node_high_water = 1; // node 0 is the implicit parent of all top-level nodes
    ui->node_free = 0;
    ui->node_live = 0;
}

static UINode *ui_node_from_handle(UIContext *ui, UINodeHandle h)
{
    u32 index = h.id & 0xFFFFu;
    if (index == 0 || index >= ui->node_capacity)
    {
        return 0;
    }
    UINode *n = &ui->nodes[index];
    if (!n->alive || n->generation != (u16)(h.id >> 16))
    {
        return 0;
    }
    return n;
}

static UINodeHandle ui_node_handle(UIContext *ui, const UINode *n)
{
    UINodeHandle h;
    h.id = ((u32)n->generation << 16) | (u32)(n - ui->nodes);
    return h;
}

static void ui_node_copy_text(UINode *n, const char *text)
{
    usize len = text ? strlen(text) : 0;
    if (len > UI_NODE_TEXT_MAX - 1)
    {
        len = UI_NODE_TEXT_MAX - 1;
    }
    if (len)
    {
        memcpy(n->text, text, len);
    }
    n->text[len] = 0;
    n->text_len = (u32)len;
}

static void ui_node_unlink(UIContext *ui, u32 index)
{
    UINode *n = &ui->nodes[index];
    UINode *p = &ui->nodes[n->parent];
    if (n->prev)
    {
        ui->nodes[n->prev].next = n->next;
    }
    else
    {
        p->first = n->next;
    }

    if (n->next)
    {
        ui->nodes[n->next].prev = n->prev;
    }
    else
    {
        p->last = n->prev;
    }
}

static void ui_node_free_subtree(UIContext *ui, u32 index)
{
    UINode *n = &ui->nodes[index];
    for (u32 c = n->first; c;)
    {
        u32 next = ui->nodes[c].next;
        ui_node_free_subtree(ui, c);
        c = next;
    }
    n->alive = false;
    n->generation++;
    n->first = n->last = 0;
    n->next = ui->node_free;
    ui->node_free = index;
    ui->node_live--;
}

ENGINE_API UINodeHandle ui_node_create(UIContext *ui, UINodeHandle parent, const UINodeDesc *desc)
{
    UINodeHandle none = {0};
    u32 parent_index = 0;
    if (parent.id)
    {
        UINode *p = ui_node_from_handle(ui, parent);
        ASSERT(p && "ui_node_create: stale parent handle");
        if (!p)
        {
            return none;
        }
        parent_index = (u32)(p - ui->nodes);
    }
    u32 index;
    if (ui->node_free)
    {
        index = ui->node_free;
        ui->node_free = ui->nodes[index].next;
    }
    else
    {
        ASSERT(ui->node_high_water < ui->node_capacity && "retained node pool exhausted");
        if (ui->node_high_water >= ui->node_capacity)
        {
            return none;
        }
        index = ui->node_high_water++;
    }
    UINode *n = &ui->nodes[index];
    u16 generation = n->generation;
    memset(n, 0, sizeof(*n));
    n->generation = generation;
    n->alive = true;
    n->visible = true;
    n->kind = desc->kind;
    n->persist = desc->persist_across_reset;
    n->size[0] = desc->size[0];
    n->size[1] = desc->size[1];
    n->x = desc->x;
    n->y = desc->y;
    n->w = desc->w;
    n->h = desc->h;
    n->min = desc->min;
    n->max = desc->max;
    n->value_f32 = desc->min;
    n->bind = desc->bind;
    ui_node_copy_text(n, desc->text);

    UINode *p = &ui->nodes[parent_index];
    n->parent = parent_index;
    n->prev = p->last;
    if (p->last)
    {
        ui->nodes[p->last].next = index;
    }
    else
    {
        p->first = index;
    }
    p->last = index;
    ui->node_live++;
    return ui_node_handle(ui, n);
}

ENGINE_API void ui_node_destroy(UIContext *ui, UINodeHandle node)
{
    UINode *n = ui_node_from_handle(ui, node);
    if (!n)
    {
        return;
    }
    u32 index = (u32)(n - ui->nodes);
    ui_node_unlink(ui, index);
    ui_node_free_subtree(ui, index);
}

void ui_nodes_clear_transient(UIContext *ui)
{
    for (u32 c = ui->nodes[0].first; c;)
    {
        u32 next = ui->nodes[c].next;
        if (!ui->nodes[c].persist)
        {
            ui_node_unlink(ui, c);
            ui_node_free_subtree(ui, c);
        }
        c = next;
    }
}

ENGINE_API b32 ui_node_valid(UIContext *ui, UINodeHandle node) { return ui_node_from_handle(ui, node) != 0; }
ENGINE_API void ui_node_set_text(UIContext *ui, UINodeHandle node, const char *text)
{
    UINode *n = ui_node_from_handle(ui, node);
    if (n)
    {
        ui_node_copy_text(n, text);
    }
}

ENGINE_API void ui_node_set_textf(UIContext *ui, UINodeHandle node, const char *fmt, ...)
{
    UINode *n = ui_node_from_handle(ui, node);
    if (!n)
    {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(n->text, sizeof(n->text), fmt, args);
    va_end(args);
    if (len < 0)
    {
        len = 0;
    }
    if (len > (int)sizeof(n->text) - 1)
    {
        len = (int)sizeof(n->text) - 1;
    }
    n->text_len = (u32)len;
}

ENGINE_API void ui_node_set_visible(UIContext *ui, UINodeHandle node, b32 visible)
{
    UINode *n = ui_node_from_handle(ui, node);
    if (n)
    {
        n->visible = visible;
    }
}

ENGINE_API f32 *ui_node_f32(UIContext *ui, UINodeHandle node)
{
    UINode *n = ui_node_from_handle(ui, node);
    if (!n)
    {
        return 0;
    }
    return n->bind ? (f32 *)n->bind : &n->value_f32;
}

ENGINE_API b32 *ui_node_b32(UIContext *ui, UINodeHandle node)
{
    UINode *n = ui_node_from_handle(ui, node);
    if (!n)
    {
        return 0;
    }
    return n->bind ? (b32 *)n->bind : &n->value_b32;
}

ENGINE_API UISignal ui_node_signal(UIContext *ui, UINodeHandle node)
{
    UISignal none = {0};
    UINode *n = ui_node_from_handle(ui, node);
    return n ? n->signal : none;
}

// ---------------------------------- replay ---------------------------------
static void ui_node_emit(UIContext *ui, u32 index)
{
    UINode *n = &ui->nodes[index];
    UISignal none = {0};
    n->signal = none;
    if (!n->visible)
    {
        return;
    }
    // Identity comes from the handle
    UIKey key = ui_key_from_u64(UI_RETAINED_SEED, ui_node_handle(ui, n).id);
    ui_next_width(ui, n->size[0]);
    ui_next_height(ui, n->size[1]);
    switch (n->kind)
    {
    case UI_NODE_COLUMN:
    case UI_NODE_ROW:
        ui_push_parent(ui, ui_build_container(ui, n->kind == UI_NODE_ROW ? UI_AXIS_X : UI_AXIS_Y));
        for (u32 c = n->first; c; c = ui->nodes[c].next)
        {
            ui_node_emit(ui, c);
        }
        ui_pop_parent(ui);
        break;
    case UI_NODE_WINDOW:
        if (ui_build_window_begin(ui, key, n->text, n->text_len, n->x, n->y, n->w, n->h))
        {
            for (u32 c = n->first; c; c = ui->nodes[c].next)
            {
                ui_node_emit(ui, c);
            }
        }
        ui_window_end(ui);
        break;
    case UI_NODE_LABEL:
        ui_build_label(ui, n->text, n->text_len);
        break;
    case UI_NODE_BUTTON:
        n->signal = ui_build_button(ui, key, n->text, n->text_len);
        break;
    case UI_NODE_CHECKBOX:
        n->signal = ui_build_checkbox(ui, key, n->text, n->text_len,
                                      n->bind ? (b32 *)n->bind : &n->value_b32);
        break;
    case UI_NODE_SLIDER:
        n->signal = ui_build_slider(ui, key, n->text, n->text_len,
                                    n->bind ? (f32 *)n->bind : &n->value_f32, n->min, n->max);
        break;
    }
}

void ui_nodes_emit_roots(UIContext *ui)
{
    for (u32 c = ui->nodes[0].first; c; c = ui->nodes[c].next)
    {
        if (ui->nodes[c].mounted_frame != ui->frame_index)
        {
            ui_node_emit(ui, c);
        }
    }
}

ENGINE_API void ui_node_mount(UIContext *ui, UINodeHandle node)
{
    UINode *n = ui_node_from_handle(ui, node);
    if (!n)
    {
        return;
    }

    ASSERT(n->parent == 0 && "only top-level nodes can be mounted");
    if (n->parent != 0 || n->mounted_frame == ui->frame_index)
    {
        return;
    }
    n->mounted_frame = ui->frame_index;
    ui_node_emit(ui, (u32)(n - ui->nodes));
}