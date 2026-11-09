#pragma once
#include "ui/ui.h"
#include "threading/sync.h"

#define UI_PARENT_STACK_MAX 64
#define UI_ID_STACK_MAX 128
#define UI_MAX_DRAW_BATCHES 512
#define UI_DRAW_CHUNK_BOXES 64
#define UI_TEXT_TMP_MAX 256
#define UI_NODE_TEXT_MAX 64
#define UI_FONT_FIRST_CHAR 32
// TODO(BPL): UTF-8 + on-demand glyph cache 2026-07-04
#define UI_FONT_GLYPH_COUNT 95 // printable ASCII
#define UI_FONT_ATLAS_SIZE 512

typedef struct UIGlyph
{
    f32 x0, y0, x1, y1; // quad offsets from the pen position on the baseline (y down)
    f32 u0, v0, u1, v1;
    f32 advance;
} UIGlyph;

typedef struct UIFont
{
    UIGlyph glyphs[UI_FONT_GLYPH_COUNT];
    f32 pixel_height;
    f32 ascent, descent, line_height;
    u8 *atlas_pixels; // R8, UI_FONT_ATLAS_SIZE squared, owned by the UI arena
    TextureHandle texture;
    b32 ok;
} UIFont;

static FORCE_INLINE const UIGlyph *ui_font_glyph(const UIFont *f, char c)
{
    u32 i = (u32)(u8)c - UI_FONT_FIRST_CHAR;
    if (i >= UI_FONT_GLYPH_COUNT)
        i = (u32)'?' - UI_FONT_FIRST_CHAR;
    return &f->glyphs[i];
}

// One GPU instance. A rounded rect, a border, or a glyph depending on params.
typedef struct UIQuad
{
    f32 rect[4]; // x0 y0 x1 y1, pixels
    f32 uv[4];   // u0 v0 u1 v1
    f32 color[4];
    f32 params[4]; // corner radius, border thickness (0 = filled), edge softness, atlas mix
} UIQuad;
static_assert(sizeof(UIQuad) == 64, "UIQuad must match the UI pipeline's input layout");

typedef struct UIDrawBatch
{
    UIRect scissor;
    u32 first_quad;
    u32 quad_count;
} UIDrawBatch;

typedef struct UINode
{
    u32 parent, first, last, next, prev; // pool indices; 0 = none (node 0 owns all top-level nodes)
    u16 generation;
    u16 alive;
    u32 kind;
    b32 visible;
    b32 persist;
    u64 mounted_frame;
    UISize size[2];
    f32 x, y, w, h;
    f32 min, max;
    f32 value_f32;
    b32 value_b32;
    void *bind;
    UISignal signal;
    u32 text_len;
    char text[UI_NODE_TEXT_MAX];
} UINode;

struct UIContext
{
    Arena *arena; // persistent UI memory
    Arena build;  // per-frame: strings, keyless boxes, draw lists
    UITheme theme;
    UIFont font;
    // box cache
    UIBox *box_pool;
    UIBox *box_free;
    UIBox **box_live;
    u32 box_live_count;
    u32 box_capacity;
    UIBox **box_table;
    u32 box_table_mask;
    // build state
    u64 frame_index;
    b32 building;
    UIBox *root;
    UIBox *parent_stack[UI_PARENT_STACK_MAX];
    u32 parent_count;
    u64 id_stack[UI_ID_STACK_MAX];
    u32 id_count;
    UISize next_size[2];
    // input
    UIInput input;
    f32 anim_rate;
    UIKey hot_key;    // topmost clickable box under the cursor, from last frame's layout
    UIKey active_key; // box holding the mouse capture
    UIKey scroll_key; // scroll container under the cursor
    UIBox *hit_box;
    UIBox *hit_scroll;
    // retained nodes
    UINode *nodes;
    u32 node_capacity;
    u32 node_high_water;
    u32 node_free;
    u32 node_live;
    // draw list: built by thread 0 in ui_end_frame, read by all threads in ui_render_gather
    UIBox **draw_boxes;
    u32 draw_box_count;
    UIDrawBatch *batches;
    u32 batch_count;
    u32 quad_count;
    u32 max_quads;
    UIQuad *quad_dst;
    WorkCounter draw_chunks;
    // gpu
    ShaderHandle vertex_shader;
    ShaderHandle pixel_shader;
    PipelineHandle pipeline;
    BufferHandle corner_buffer; // + 4 per-vertex corners, slot 0 (same shape as the game's geometry buffer)
    BufferHandle quad_buffer;   //   per-instance data, slot 1
    PlatformAPI *platform;      // + for diagnostics
    b32 render_ok;
    UIStats stats;
    UIStats stats_prev;
};

// ui_core.cpp
u64 ui_hash_bytes(u64 seed, const void *data, usize len);
UIKey ui_key_from_u64(u64 seed, u64 id);
UIBox *ui_box_lookup(UIContext *ui, UIKey key);
void ui_id_push_raw(UIContext *ui, u64 seed);
// ui_layout.cpp
void ui_layout(UIContext *ui);
// ui_font.cpp
void ui_font_init_fallback(UIFont *font, f32 pixel_height);
f32 ui_font_text_width(const UIFont *font, const char *text, u32 len);
// ui_render.cpp
void ui_draw_flatten(UIContext *ui);
// ui_widgets.cpp: builders with explicit identity, shared by both front ends
void ui_box_set_text_n(UIContext *ui, UIBox *box, const char *text, u32 len);
UIBox *ui_build_container(UIContext *ui, u32 axis);
UIBox *ui_build_label(UIContext *ui, const char *text, u32 len);
UISignal ui_build_button(UIContext *ui, UIKey key, const char *text, u32 len);
UISignal ui_build_checkbox(UIContext *ui, UIKey key, const char *text, u32 len, b32 *value);
UISignal ui_build_slider(UIContext *ui, UIKey key, const char *text, u32 len, f32 *value, f32 lo, f32 hi);
b32 ui_build_window_begin(UIContext *ui, UIKey key, const char *title, u32 len, f32 x, f32 y, f32 w, f32 h);
// ui_retained.cpp
void ui_nodes_init(UIContext *ui, Arena *arena, u32 capacity);
void ui_nodes_emit_roots(UIContext *ui);