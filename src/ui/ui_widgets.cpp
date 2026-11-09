#include "ui/ui_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Builders take an explicit key and text. The immediate API derives the key from
// the label; the retained API derives it from the node handle.

static FORCE_INLINE void ui_size_default(UIBox *b, u32 axis, UISize s)
{
    if (b->size[axis].kind == UI_SIZE_NULL)
    {
        b->size[axis] = s;
    }
}

static FORCE_INLINE f32 ui_clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static u32 ui_display_len(const char *label)
{
    const char *cut = strstr(label, "##");
    return (u32)(cut ? (usize)(cut - label) : strlen(label));
}

void ui_box_set_text_n(UIContext *ui, UIBox *box, const char *text, u32 len)
{
    // Must be copied. Game.dll string literals disappear on reload.
    char *dst = PushArrayRaw(&ui->build, char, len + 1);
    memcpy(dst, text, len);
    dst[len] = 0;
    box->text = dst;
    box->text_len = len;
}

ENGINE_API void ui_box_set_text(UIContext *ui, UIBox *box, const char *text)
{
    ui_box_set_text_n(ui, box, text, ui_display_len(text));
}

static void ui_box_set_textf(UIContext *ui, UIBox *box, const char *fmt, ...)
{
    char tmp[UI_TEXT_TMP_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n < 0)
    {
        n = 0;
    }
    if (n > (int)sizeof(tmp) - 1)
    {
        n = (int)sizeof(tmp) - 1;
    }
    ui_box_set_text_n(ui, box, tmp, (u32)n);
}

// --------------------------------- builders --------------------------------
UIBox *ui_build_container(UIContext *ui, u32 axis)
{
    UIKey none = {0};
    UIBox *b = ui_box_make(ui, 0, none);
    b->layout_axis = axis;
    b->child_gap = ui->theme.gap;
    ui_size_default(b, UI_AXIS_X, ui_fill());
    ui_size_default(b, UI_AXIS_Y, ui_children());
    return b;
}

UIBox *ui_build_label(UIContext *ui, const char *text, u32 len)
{
    UIKey none = {0};
    UIBox *b = ui_box_make(ui, UI_BOX_DRAW_TEXT, none);
    ui_box_set_text_n(ui, b, text, len);
    ui_size_default(b, UI_AXIS_X, ui_fit_text(ui->theme.padding));
    ui_size_default(b, UI_AXIS_Y, ui_px(ui->theme.row_height));
    return b;
}

UISignal ui_build_button(UIContext *ui, UIKey key, const char *text, u32 len)
{
    UIBox *b = ui_box_make(ui, UI_BOX_CLICKABLE | UI_BOX_DRAW_BACKGROUND | UI_BOX_DRAW_BORDER | UI_BOX_DRAW_TEXT | UI_BOX_TEXT_CENTER | UI_BOX_HOT_ANIM, key);
    ui_box_set_text_n(ui, b, text, len);
    ui_size_default(b, UI_AXIS_X, ui_fit_text(ui->theme.padding * 2.0f));
    ui_size_default(b, UI_AXIS_Y, ui_px(ui->theme.row_height));
    b->corner_radius = ui->theme.corner_radius;
    return ui_signal_from_box(ui, b);
}

UISignal ui_build_checkbox(UIContext *ui, UIKey key, const char *text, u32 len, b32 *value)
{
    UIBox *b = ui_box_make(ui, UI_BOX_CLICKABLE | UI_BOX_DRAW_BACKGROUND | UI_BOX_DRAW_TEXT | UI_BOX_HOT_ANIM, key);
    UISignal s = ui_signal_from_box(ui, b);
    if (s.clicked && value)
    {
        *value = !*value;
        s.changed = true;
    }

    ui_box_set_textf(ui, b, "[%c] %.*s", (value && *value) ? 'x' : ' ', (int)len, text);
    ui_size_default(b, UI_AXIS_X, ui_fit_text(ui->theme.padding));
    ui_size_default(b, UI_AXIS_Y, ui_px(ui->theme.row_height));
    b->background.a = 0.0f;
    b->corner_radius = ui->theme.corner_radius;
    return s;
}

// Fill bar with centered text layered inside track as floating children.
static void ui_build_bar_interior(UIContext *ui, UIBox *track, f32 t, const char *text, u32 len)
{
    UIKey none = {0};
    ui_push_parent(ui, track);
    UIBox *fill = ui_box_make(ui, UI_BOX_DRAW_BACKGROUND | UI_BOX_FLOATING, none);
    fill->size[UI_AXIS_X] = ui_pct(ui_clamp01(t));
    fill->size[UI_AXIS_Y] = ui_pct(1.0f);
    fill->background = ui->theme.accent;
    fill->corner_radius = track->corner_radius;
    UIBox *label = ui_box_make(ui, UI_BOX_DRAW_TEXT | UI_BOX_TEXT_CENTER | UI_BOX_FLOATING, none);
    label->size[UI_AXIS_X] = ui_pct(1.0f);
    label->size[UI_AXIS_Y] = ui_pct(1.0f);
    ui_box_set_text_n(ui, label, text, len);
    ui_pop_parent(ui);
}

UISignal ui_build_slider(UIContext *ui, UIKey key, const char *text, u32 len, f32 *value, f32 lo, f32 hi)
{
    UIBox *track = ui_box_make(ui, UI_BOX_CLICKABLE | UI_BOX_DRAW_BACKGROUND | UI_BOX_DRAW_BORDER | UI_BOX_HOT_ANIM | UI_BOX_CLIP, key);
    ui_size_default(track, UI_AXIS_X, ui_fill());
    ui_size_default(track, UI_AXIS_Y, ui_px(ui->theme.row_height));
    track->corner_radius = ui->theme.corner_radius;
    UISignal s = ui_signal_from_box(ui, track);
    f32 v = value ? *value : lo;
    f32 w = track->rect.x1 - track->rect.x0; // last frame's layout
    if (s.held && value && w > 0.0f && hi > lo)
    {
        f32 nv = lo + ui_clamp01((ui->input.mouse_x - track->rect.x0) / w) * (hi - lo);
        if (nv != v)
        {
            v = nv;
            *value = nv;
            s.changed = true;
        }
    }
    char tmp[UI_TEXT_TMP_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%.*s: %.3f", (int)len, text, v);
    if (n < 0)
        n = 0;
    if (n > (int)sizeof(tmp) - 1)
        n = (int)sizeof(tmp) - 1;
    ui_build_bar_interior(ui, track, hi > lo ? (v - lo) / (hi - lo) : 0.0f, tmp, (u32)n);
    return s;
}

// TODO(BPL): z-order / bring-to-front, resize grip, close button.
b32 ui_build_window_begin(UIContext *ui, UIKey key, const char *title, u32 len, f32 x, f32 y, f32 w, f32 h)
{
    const UITheme *th = &ui->theme;
    UIKey none = {0};
    UIBox *win = ui_box_make(ui, UI_BOX_CLICKABLE | UI_BOX_DRAW_BACKGROUND | UI_BOX_DRAW_BORDER | UI_BOX_CLIP | UI_BOX_FLOATING | UI_BOX_KEEP_STATE, key);
    if (!win->key.v || win->first_frame == ui->frame_index)
    {
        win->float_pos[0] = x;
        win->float_pos[1] = y;
        win->open = true;
    }
    win->background = th->window_bg;
    win->corner_radius = th->corner_radius;
    win->padding[0] = win->padding[1] = 1.0f; // keep children off the border
    ui_signal_from_box(ui, win);              // swallow clicks so they do not reach the game
    ui_push_parent(ui, win);

    UIBox *bar = ui_box_make(ui, UI_BOX_DRAW_BACKGROUND, none);
    bar->layout_axis = UI_AXIS_X;
    bar->size[UI_AXIS_X] = ui_fill();
    bar->size[UI_AXIS_Y] = ui_px(th->row_height);
    bar->background = th->title_bg;
    ui_push_parent(ui, bar);
    {
        UIBox *toggle = ui_box_make(ui, UI_BOX_CLICKABLE | UI_BOX_DRAW_BACKGROUND | UI_BOX_DRAW_TEXT | UI_BOX_TEXT_CENTER | UI_BOX_HOT_ANIM, ui_key(ui, "##toggle"));
        toggle->size[UI_AXIS_X] = ui_px(th->row_height);
        toggle->size[UI_AXIS_Y] = ui_px(th->row_height);
        toggle->background = th->title_bg;
        if (ui_signal_from_box(ui, toggle).clicked)
            win->open = !win->open;
        ui_box_set_text_n(ui, toggle, win->open ? "-" : "+", 1);

        UIBox *grip = ui_box_make(ui, UI_BOX_CLICKABLE | UI_BOX_DRAW_TEXT, ui_key(ui, "##grip"));
        grip->size[UI_AXIS_X] = ui_fill();
        grip->size[UI_AXIS_Y] = ui_px(th->row_height);
        ui_box_set_text_n(ui, grip, title, len);
        UISignal drag = ui_signal_from_box(ui, grip);
        if (drag.held)
        {
            win->float_pos[0] += drag.drag_dx;
            win->float_pos[1] += drag.drag_dy;
        }
    }
    ui_pop_parent(ui);

    win->size[UI_AXIS_X] = ui_px(w);
    win->size[UI_AXIS_Y] = win->open ? ui_px(h) : ui_children();

    UIBox *body = ui_box_make(ui, UI_BOX_CLIP | UI_BOX_SCROLL_Y, ui_key(ui, "##body"));
    body->size[UI_AXIS_X] = ui_fill();
    body->size[UI_AXIS_Y] = win->open ? ui_fill() : ui_px(0);
    body->padding[0] = body->padding[1] = th->padding;
    body->child_gap = th->gap;
    ui_push_parent(ui, body);
    return win->open;
}

// ------------------------------ immediate API ------------------------------
ENGINE_API void ui_label(UIContext *ui, const char *text) { ui_build_label(ui, text, (u32)strlen(text)); }
ENGINE_API void ui_labelf(UIContext *ui, const char *fmt, ...)
{
    char tmp[UI_TEXT_TMP_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n < 0)
        n = 0;
    if (n > (int)sizeof(tmp) - 1)
        n = (int)sizeof(tmp) - 1;
    ui_build_label(ui, tmp, (u32)n);
}

ENGINE_API UISignal ui_button(UIContext *ui, const char *label)
{
    return ui_build_button(ui, ui_key(ui, label), label, ui_display_len(label));
}

ENGINE_API UISignal ui_checkbox(UIContext *ui, const char *label, b32 *value)
{
    return ui_build_checkbox(ui, ui_key(ui, label), label, ui_display_len(label), value);
}

ENGINE_API UISignal ui_slider_f32(UIContext *ui, const char *label, f32 *value, f32 min, f32 max)
{
    return ui_build_slider(ui, ui_key(ui, label), label, ui_display_len(label), value, min, max);
}

ENGINE_API void ui_meter(UIContext *ui, f32 fraction, const char *text)
{
    UIKey none = {0};
    UIBox *track = ui_box_make(ui, UI_BOX_DRAW_BACKGROUND | UI_BOX_DRAW_BORDER | UI_BOX_CLIP, none);
    ui_size_default(track, UI_AXIS_X, ui_fill());
    ui_size_default(track, UI_AXIS_Y, ui_px(ui->theme.row_height));
    track->corner_radius = ui->theme.corner_radius;
    ui_build_bar_interior(ui, track, fraction, text, (u32)strlen(text));
}

ENGINE_API void ui_spacer(UIContext *ui, f32 pixels)
{
    UIKey none = {0};
    u32 axis = ui->parent_stack[ui->parent_count - 1]->layout_axis;
    UIBox *b = ui_box_make(ui, 0, none);
    b->size[axis] = ui_px(pixels);
    b->size[1 - axis] = ui_px(0);
}

ENGINE_API void ui_row_begin(UIContext *ui) { ui_push_parent(ui, ui_build_container(ui, UI_AXIS_X)); }
ENGINE_API void ui_row_end(UIContext *ui) { ui_pop_parent(ui); }
ENGINE_API void ui_column_begin(UIContext *ui) { ui_push_parent(ui, ui_build_container(ui, UI_AXIS_Y)); }
ENGINE_API void ui_column_end(UIContext *ui) { ui_pop_parent(ui); }

ENGINE_API b32 ui_window_begin(UIContext *ui, const char *title, f32 x, f32 y, f32 w, f32 h)
{
    return ui_build_window_begin(ui, ui_key(ui, title), title, ui_display_len(title), x, y, w, h);
}

ENGINE_API void ui_window_end(UIContext *ui)
{
    ui_pop_parent(ui); // body
    ui_pop_parent(ui); // window
}

ENGINE_API b32 ui_tree_begin(UIContext *ui, const char *label)
{
    const UITheme *th = &ui->theme;
    UIKey none = {0};
    UIKey key = ui_key(ui, label);
    UIBox *head = ui_box_make(ui, UI_BOX_CLICKABLE | UI_BOX_DRAW_BACKGROUND | UI_BOX_DRAW_TEXT | UI_BOX_HOT_ANIM | UI_BOX_KEEP_STATE, key);
    ui_size_default(head, UI_AXIS_X, ui_fill());
    ui_size_default(head, UI_AXIS_Y, ui_px(th->row_height));
    head->corner_radius = th->corner_radius;
    if (ui_signal_from_box(ui, head).clicked)
    {
        head->open = !head->open;
    }
    ui_box_set_textf(ui, head, "%c %.*s", head->open ? '-' : '+', (int)ui_display_len(label), label);
    if (!head->open)
    {
        return false;
    }

    ui_id_push_raw(ui, key.v); // children are scoped to this node
    UIBox *row = ui_build_container(ui, UI_AXIS_X);
    row->child_gap = 0.0f;
    ui_push_parent(ui, row);
    UIBox *indent = ui_box_make(ui, 0, none);
    indent->size[UI_AXIS_X] = ui_px(th->indent);
    indent->size[UI_AXIS_Y] = ui_px(0);
    ui_push_parent(ui, ui_build_container(ui, UI_AXIS_Y));
    return true;
}

ENGINE_API void ui_tree_end(UIContext *ui)
{
    ui_pop_parent(ui); // column
    ui_pop_parent(ui); // indent row
    ui_pop_id(ui);
}

ENGINE_API void ui_scroll_begin(UIContext *ui, const char *id, f32 height)
{
    UIBox *b = ui_box_make(ui, UI_BOX_CLIP | UI_BOX_SCROLL_Y | UI_BOX_DRAW_BORDER, ui_key(ui, id));
    ui_size_default(b, UI_AXIS_X, ui_fill());
    ui_size_default(b, UI_AXIS_Y, ui_px(height));
    b->padding[0] = b->padding[1] = ui->theme.padding;
    b->child_gap = ui->theme.gap;
    ui_push_parent(ui, b);
}

ENGINE_API void ui_scroll_end(UIContext *ui) { ui_pop_parent(ui); }