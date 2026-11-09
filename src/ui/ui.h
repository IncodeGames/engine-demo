#pragma once
#include "shared/types.h"
#include "shared/assert.h"
#include "memory/arena.h"
#include "platform/platform.h"
#include "renderer/renderer.h"

// NOT FINAL IMPLEMENTATION PROBABLY JANKY AS HELL
//    core      - a tree of UIBox rebuilt every frame; keyed boxes keep state across frames.
//    immediate - ui_button(), ui_window_begin(), ... build boxes straight from code.
//    retained  - ui_node_create() stores a persistent description tree that the UI
//                replays through the same builders every frame.
//  THREADING: thread 0 only, between ui_begin_frame and ui_end_frame (for the game:
//  inside update() when thread_index == 0, or a SYSTEM_MAIN_THREAD system).
//  ***ui_render_gather is called by the engine on every thread.***

typedef struct UIContext UIContext;

typedef struct UIKey
{
    u64 v; // 0 = no identity: the box is rebuilt from nothing every frame
} UIKey;

typedef struct UIRect
{
    f32 x0, y0, x1, y1;
} UIRect;

typedef struct UIColor
{
    f32 r, g, b, a;
} UIColor;

enum
{
    UI_AXIS_X = 0,
    UI_AXIS_Y = 1
};

// ----------------------------------- sizes ---------------------------------
typedef enum UISizeKind
{
    UI_SIZE_NULL = 0,    // unspecified: the widget picks its default
    UI_SIZE_PIXELS,      // value = pixels
    UI_SIZE_TEXT,        // text extent + 2 * value pixels of padding
    UI_SIZE_PARENT_PCT,  // value = fraction of the nearest explicitly-sized ancestor
    UI_SIZE_CHILDREN_SUM // wraps its children
} UISizeKind;

typedef struct UISize
{
    f32 value;
    f32 strictness; // 1 = never shrink, 0 = give everything up when the parent overflows
    u32 kind;       // UISizeKind
} UISize;

static FORCE_INLINE UISize ui_size(u32 kind, f32 value, f32 strictness)
{
    UISize s;
    s.value = value;
    s.strictness = strictness;
    s.kind = kind;
    return s;
}

#define ui_px(v) ui_size(UI_SIZE_PIXELS, (f32)(v), 1.0f)
#define ui_fit_text(pad) ui_size(UI_SIZE_TEXT, (f32)(pad), 1.0f)
#define ui_pct(v) ui_size(UI_SIZE_PARENT_PCT, (f32)(v), 1.0f)
#define ui_fill() ui_size(UI_SIZE_PARENT_PCT, 1.0f, 0.0f)
#define ui_children() ui_size(UI_SIZE_CHILDREN_SUM, 0.0f, 1.0f)

// ---------------------------------- boxes ----------------------------------
enum
{
    UI_BOX_CLICKABLE = 1 << 0, // takes part in hit testing (needs a key)
    UI_BOX_DRAW_BACKGROUND = 1 << 1,
    UI_BOX_DRAW_BORDER = 1 << 2,
    UI_BOX_DRAW_TEXT = 1 << 3,
    UI_BOX_TEXT_CENTER = 1 << 4,
    UI_BOX_HOT_ANIM = 1 << 5,  // background blends toward the theme's hot/active colors
    UI_BOX_CLIP = 1 << 6,      // children are scissored to this box
    UI_BOX_SCROLL_Y = 1 << 7,  // mouse wheel scrolls children (needs a key; children stack on Y)
    UI_BOX_FLOATING = 1 << 8,  // placed at float_pos relative to the parent; ignored by its flow
    UI_BOX_KEEP_STATE = 1 << 9 // state survives frames where the box is not built (windows, tree nodes)
};

typedef struct UIBox
{
    // tree links: rebuilt every frame
    struct UIBox *parent, *first, *last, *next, *prev;
    // cache: keyed boxes only
    struct UIBox *hash_next;
    UIKey key;
    u64 first_frame; // frame this box entered the cache
    u64 last_frame;  // last frame this box was built
    // description: written by the builder every frame
    u32 flags;
    u32 layout_axis; // axis children are stacked along
    UISize size[2];
    f32 padding[2];
    f32 child_gap;
    f32 text_pad;
    const char *text; // UI build arena; valid for the current frame only
    u32 text_len;
    f32 corner_radius;
    f32 border_thickness;
    UIColor background;
    UIColor border_color;
    UIColor text_color;
    // layout output: valid after ui_end_frame; read by drawing and by next frame's input
    f32 computed_size[2];
    UIRect rect; // absolute pixels
    UIRect clip; // scissor inherited from ancestors
    u32 first_quad;
    u32 quad_count;
    // persistent state: lives as long as the box stays in the cache
    f32 hot_t, active_t;
    f32 scroll[2];
    f32 float_pos[2];
    b32 open;
} UIBox;

typedef struct UISignal
{
    UIBox *box;  // current frame only
    b32 hovered; // true for topmost clickable box under the cursor
    b32 pressed; // left button down this frame
    b32 held;
    b32 released; // capture ended this frame
    b32 clicked;  // released while hovering
    b32 changed;
    f32 drag_dx, drag_dy;
} UISignal;

typedef struct UITheme
{
    UIColor window_bg, title_bg, border;
    UIColor widget_bg, widget_hot, widget_active, accent, text;
    f32 row_height, padding, gap, corner_radius, indent;
} UITheme;

typedef struct UIInput
{
    f32 mouse_x, mouse_y, mouse_dx, mouse_dy, mouse_wheel;
    b32 mouse_down[PLATFORM_MOUSE_BUTTON_COUNT];
    b32 mouse_pressed[PLATFORM_MOUSE_BUTTON_COUNT];
    b32 mouse_released[PLATFORM_MOUSE_BUTTON_COUNT];
    f32 dt;
    u32 window_w, window_h;
} UIInput;

typedef struct UIDesc
{
    u32 max_boxes;          // keyed boxes alive at once
    u32 max_nodes;          // retained nodes (<= 65536)
    u32 max_quads;          // GPU quads per frame
    usize build_arena_size; // per-frame memory, carved out of the UI arena
} UIDesc;

typedef struct UIStats // numbers from the last completed frame
{
    u32 boxes_built;
    u32 boxes_cached;
    u32 quads;
    u32 batches;
    u32 quads_dropped;  // max_quads or batch limit hit
    u32 key_collisions; // same key built twice in one frame; the second loses interaction
    u32 pool_overflows; // keyed box demoted to a stateless one: raise max_boxes
    u32 nodes_live;
} UIStats;

// ------------------------------ engine-side  ------------------------------
UIContext *ui_create(Arena *arena, const UIDesc *desc);
b32 ui_font_load(UIContext *ui, PlatformAPI *platform, const char *ttf_path, f32 pixel_height);
b32 ui_render_init(UIContext *ui, RendererAPI *api, Renderer *renderer);
void ui_begin_frame(UIContext *ui, const UIInput *input); // thread 0
void ui_end_frame(UIContext *ui);                         // thread 0
void ui_render_prepare(UIContext *ui, RenderList *list);  // thread 0, after render_list_reset
void ui_render_gather(UIContext *ui);                     // all threads
void ui_nodes_clear_transient(UIContext *ui);             // full game reset

// ----------------------------------- core ----------------------------------
ENGINE_API UIKey ui_key(UIContext *ui, const char *string); // hashed within the current id scope
ENGINE_API void ui_push_id(UIContext *ui, u64 id);          // extra scope for loops
ENGINE_API void ui_pop_id(UIContext *ui);
ENGINE_API UIBox *ui_box_make(UIContext *ui, u32 flags, UIKey key);
ENGINE_API void ui_box_set_text(UIContext *ui, UIBox *box, const char *text); // copies; stops at "##"
ENGINE_API void ui_push_parent(UIContext *ui, UIBox *box);
ENGINE_API void ui_pop_parent(UIContext *ui);
ENGINE_API UISignal ui_signal_from_box(UIContext *ui, UIBox *box);
ENGINE_API void ui_next_width(UIContext *ui, UISize size); // overrides the next box's default size
ENGINE_API void ui_next_height(UIContext *ui, UISize size);
ENGINE_API b32 ui_wants_mouse(UIContext *ui); // cursor is over UI or UI holds the capture
ENGINE_API UITheme *ui_theme(UIContext *ui);
ENGINE_API const UIStats *ui_stats(UIContext *ui);

// ------------------------------- immediate API ------------------------------
// Labels double as identity: "Save##settings" shows "Save" and hashes the whole string.
ENGINE_API void ui_label(UIContext *ui, const char *text);
ENGINE_API void ui_labelf(UIContext *ui, const char *fmt, ...);
ENGINE_API UISignal ui_button(UIContext *ui, const char *label);
ENGINE_API UISignal ui_checkbox(UIContext *ui, const char *label, b32 *value);
ENGINE_API UISignal ui_slider_f32(UIContext *ui, const char *label, f32 *value, f32 min, f32 max);
ENGINE_API void ui_meter(UIContext *ui, f32 fraction, const char *text);
ENGINE_API void ui_spacer(UIContext *ui, f32 pixels);
ENGINE_API void ui_row_begin(UIContext *ui);
ENGINE_API void ui_row_end(UIContext *ui);
ENGINE_API void ui_column_begin(UIContext *ui);
ENGINE_API void ui_column_end(UIContext *ui);
// Always pair with ui_window_end. Skip contents when it returns false.
ENGINE_API b32 ui_window_begin(UIContext *ui, const char *title, f32 x, f32 y, f32 w, f32 h);
ENGINE_API void ui_window_end(UIContext *ui);
ENGINE_API b32 ui_tree_begin(UIContext *ui, const char *label);
ENGINE_API void ui_tree_end(UIContext *ui);
ENGINE_API void ui_scroll_begin(UIContext *ui, const char *id, f32 height);
ENGINE_API void ui_scroll_end(UIContext *ui);

// ------------------------------- retained API -------------------------------
typedef struct UINodeHandle
{
    u32 id; // 0 = none
} UINodeHandle;

static FORCE_INLINE UINodeHandle ui_node_root(void)
{
    UINodeHandle h = {0};
    return h;
}

typedef enum UINodeKind
{
    UI_NODE_COLUMN,
    UI_NODE_ROW,
    UI_NODE_WINDOW,
    UI_NODE_LABEL,
    UI_NODE_BUTTON,
    UI_NODE_CHECKBOX,
    UI_NODE_SLIDER
} UINodeKind;

typedef struct UINodeDesc
{
    const char *text; // copied into the node
    u32 kind;         // UINodeKind
    UISize size[2];   // UI_SIZE_NULL = widget default
    f32 x, y, w, h;
    f32 min, max; // sliders
    // TODO(BPL) Do we need something more robust that this? Generate descriptions AOT?
    void *bind;               // optional b32* (checkbox) / f32* (slider); must outlive the node (arena, not stack)
    b32 persist_across_reset; // true for engine-owned trees pass. game side trees are destroyed on a full reset
} UINodeDesc;

// Top-level nodes (parent = ui_node_root()) are drawn automatically at the end of every
// frame, unless immediate-mode code placed them this frame with ui_node_mount.
ENGINE_API UINodeHandle ui_node_create(UIContext *ui, UINodeHandle parent, const UINodeDesc *desc);
ENGINE_API void ui_node_destroy(UIContext *ui, UINodeHandle node); // and its subtree
ENGINE_API b32 ui_node_valid(UIContext *ui, UINodeHandle node);
ENGINE_API void ui_node_set_text(UIContext *ui, UINodeHandle node, const char *text);
ENGINE_API void ui_node_set_textf(UIContext *ui, UINodeHandle node, const char *fmt, ...);
ENGINE_API void ui_node_set_visible(UIContext *ui, UINodeHandle node, b32 visible);
ENGINE_API f32 *ui_node_f32(UIContext *ui, UINodeHandle node); // slider storage (bound or internal)
ENGINE_API b32 *ui_node_b32(UIContext *ui, UINodeHandle node); // checkbox storage
// Signal from the last time the node was built (end of the previous frame for
// auto-drawn trees). Each edge (clicked, pressed, ...) is visible to exactly one update.
ENGINE_API UISignal ui_node_signal(UIContext *ui, UINodeHandle node);
ENGINE_API void ui_node_mount(UIContext *ui, UINodeHandle node); // build a top-level node under the current parent