#include "ui/ui_internal.h"
#include <math.h>

// Per axis: (1) sizes that depend on nothing or on ancestors, top-down;
// (2) sizes that wrap children, bottom-up; (3) top-down again: re-resolve percent
// children against the parent's final size, then shrink whatever overflows by as
// much as each child's strictness allows. Finally place everything.

static FORCE_INLINE b32 ui_size_wraps(const UIBox *b, u32 axis)
{
    u32 k = b->size[axis].kind;
    return k == UI_SIZE_CHILDREN_SUM || k == UI_SIZE_NULL;
}

// Percent sizes resolve against the nearest ancestor whose size does not depend on its children
static f32 ui_layout_pct_basis(const UIBox *from, u32 axis)
{
    const UIBox *p = from;
    while (p && ui_size_wraps(p, axis))
    {
        p = p->parent;
    }
    if (!p)
    {
        return 0.0f;
    }
    f32 v = p->computed_size[axis] - 2.0f * p->padding[axis];
    return v > 0.0f ? v : 0.0f;
}

static void ui_layout_sizes_down(UIContext *ui, UIBox *b, u32 axis)
{
    UISize s = b->size[axis];
    switch (s.kind)
    {
    case UI_SIZE_PIXELS:
        b->computed_size[axis] = s.value;
        break;
    case UI_SIZE_TEXT:
    {
        f32 extent = (axis == UI_AXIS_X) ? ui_font_text_width(&ui->font, b->text, b->text_len)
                                         : ui->font.line_height;
        b->computed_size[axis] = extent + 2.0f * s.value;
    }
    break;
    case UI_SIZE_PARENT_PCT:
        b->computed_size[axis] = ui_layout_pct_basis(b->parent, axis) * s.value;
        break;
    default:
        b->computed_size[axis] = 0.0f;
        break;
    }
    for (UIBox *c = b->first; c; c = c->next)
        ui_layout_sizes_down(ui, c, axis);
}

static void ui_layout_sizes_up(UIBox *b, u32 axis)
{
    f32 sum = 0.0f, largest = 0.0f;
    u32 n = 0;
    for (UIBox *c = b->first; c; c = c->next)
    {
        ui_layout_sizes_up(c, axis);
        if (c->flags & UI_BOX_FLOATING)
        {
            continue;
        }
        sum += c->computed_size[axis];
        largest = Max(largest, c->computed_size[axis]);
        n++;
    }
    if (ui_size_wraps(b, axis))
    {
        f32 content = (axis == b->layout_axis) ? sum + b->child_gap * (f32)(n > 1 ? n - 1 : 0) : largest;
        b->computed_size[axis] = content + 2.0f * b->padding[axis];
    }
}

static void ui_layout_solve(UIBox *b, u32 axis)
{
    f32 avail = b->computed_size[axis] - 2.0f * b->padding[axis];
    if (avail < 0.0f)
        avail = 0.0f;
    f32 total = 0.0f, budget = 0.0f;
    u32 n = 0;
    for (UIBox *c = b->first; c; c = c->next)
    {
        if (c->size[axis].kind == UI_SIZE_PARENT_PCT)
        {
            c->computed_size[axis] = ui_layout_pct_basis(b, axis) * c->size[axis].value;
        }
        if (c->flags & UI_BOX_FLOATING)
        {
            continue;
        }
        total += c->computed_size[axis];
        budget += c->computed_size[axis] * (1.0f - c->size[axis].strictness);
        n++;
    }
    if (!ui_size_wraps(b, axis))
    {
        if (axis == b->layout_axis)
        {
            total += b->child_gap * (f32)(n > 1 ? n - 1 : 0);
            f32 over = total - avail;
            if (over > 0.0f && budget > 0.0f)
            {
                f32 k = over / budget;
                if (k > 1.0f)
                {
                    k = 1.0f; // whatever is left overflows (and scrolls, if the box scrolls)
                }
                for (UIBox *c = b->first; c; c = c->next)
                {
                    if (!(c->flags & UI_BOX_FLOATING))
                    {
                        c->computed_size[axis] -= c->computed_size[axis] * (1.0f - c->size[axis].strictness) * k;
                    }
                }
            }
        }
        else
        {
            for (UIBox *c = b->first; c; c = c->next)
            {
                if (c->flags & UI_BOX_FLOATING)
                {
                    continue;
                }

                f32 over = c->computed_size[axis] - avail;
                if (over > 0.0f)
                {
                    f32 give = c->computed_size[axis] * (1.0f - c->size[axis].strictness);
                    c->computed_size[axis] -= Min(over, give);
                }
            }
        }
    }
    for (UIBox *c = b->first; c; c = c->next)
        ui_layout_solve(c, axis);
}

static void ui_layout_place(UIBox *b)
{
    u32 la = b->layout_axis;
    if (b->flags & UI_BOX_SCROLL_Y)
    {
        f32 extent = 2.0f * b->padding[UI_AXIS_Y];
        u32 n = 0;
        for (UIBox *c = b->first; c; c = c->next)
        {
            if (!(c->flags & UI_BOX_FLOATING))
            {
                extent += c->computed_size[UI_AXIS_Y];
                n++;
            }
        }
        extent += b->child_gap * (f32)(n > 1 ? n - 1 : 0);
        f32 max_scroll = extent - b->computed_size[UI_AXIS_Y];
        if (max_scroll < 0.0f)
        {
            max_scroll = 0.0f;
        }
        if (b->scroll[UI_AXIS_Y] > max_scroll)
        {
            b->scroll[UI_AXIS_Y] = max_scroll;
        }
        if (b->scroll[UI_AXIS_Y] < 0.0f)
        {
            b->scroll[UI_AXIS_Y] = 0.0f;
        }
    }
    else
    {
        b->scroll[0] = b->scroll[1] = 0.0f;
    }

    f32 cursor = b->padding[la];
    for (UIBox *c = b->first; c; c = c->next)
    {
        f32 rel[2];
        if (c->flags & UI_BOX_FLOATING)
        {
            rel[0] = c->float_pos[0];
            rel[1] = c->float_pos[1];
        }
        else
        {
            rel[la] = cursor - b->scroll[la];
            rel[1 - la] = b->padding[1 - la] - b->scroll[1 - la];
            cursor += c->computed_size[la] + b->child_gap;
        }
        // Snap both edges so neighbours share pixels exactly and text stays crisp.
        c->rect.x0 = floorf(b->rect.x0 + rel[0]);
        c->rect.y0 = floorf(b->rect.y0 + rel[1]);
        c->rect.x1 = floorf(b->rect.x0 + rel[0] + c->computed_size[0]);
        c->rect.y1 = floorf(b->rect.y0 + rel[1] + c->computed_size[1]);
        ui_layout_place(c);
    }
}

void ui_layout(UIContext *ui)
{
    UIBox *root = ui->root;
    for (u32 axis = 0; axis < 2; ++axis)
    {
        ui_layout_sizes_down(ui, root, axis);
        ui_layout_sizes_up(root, axis);
        ui_layout_solve(root, axis);
    }
    root->rect.x0 = 0.0f;
    root->rect.y0 = 0.0f;
    root->rect.x1 = root->computed_size[0];
    root->rect.y1 = root->computed_size[1];
    ui_layout_place(root);
}