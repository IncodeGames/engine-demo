#include "ui/ui_internal.h"
#include <string.h>
#include <math.h>

// Serial part (thread 0, ui_end_frame): walk the tree in paint order, give every visible
// box a range in one quad array, split draws where the scissor changes, hit-test the
// cursor.
//
// Parallel part (all threads, ui_render_gather): pull chunks of boxes and write
// their quads straight into the frame's upload memory.
//
// GPU side mirrors the game's triangle path -- slot 0 = 4 per-vertex corners,
// slot 1 = per-instance quads, TRIANGLE_STRIP, UpdateSubresource before binding.

typedef struct UICorner
{
    f32 x, y;
} UICorner;

static const char *UI_HLSL =
    "cbuffer UIFrame : register(b0) { float2 inv_viewport; float2 pad0; };\n"
    "Texture2D    atlas : register(t0);\n"
    "SamplerState smp   : register(s0);\n"
    "struct VSIn {\n"
    "  float2 corner : UICORNER;\n"
    "  float4 rc     : UIRECT;\n"
    "  float4 uv     : UIUV;\n"
    "  float4 col    : UICOLOR;\n"
    "  float4 prm    : UIPARAMS;\n"
    "};\n"
    "struct VSOut {\n"
    "  float4 pos : SV_POSITION;\n"
    "  float4 col : COLOR0;\n"
    "  float2 uv  : TEXCOORD0;\n"
    "  float2 lp  : TEXCOORD1;\n"
    "  nointerpolation float2 hs  : TEXCOORD2;\n"
    "  nointerpolation float4 prm : TEXCOORD3;\n"
    "};\n"
    "VSOut VSMain(VSIn i) {\n"
    "  float2 c  = i.corner;\n"
    "  float2 p  = lerp(i.rc.xy, i.rc.zw, c);\n"
    "  float2 hs = (i.rc.zw - i.rc.xy) * 0.5;\n"
    "  VSOut o;\n"
    "  o.pos = float4(p.x * inv_viewport.x * 2.0 - 1.0, 1.0 - p.y * inv_viewport.y * 2.0, 0.0, 1.0);\n"
    "  o.col = i.col;\n"
    "  o.uv  = lerp(i.uv.xy, i.uv.zw, c);\n"
    "  o.lp  = (c * 2.0 - 1.0) * hs;\n"
    "  o.hs  = hs;\n"
    "  o.prm = i.prm;\n"
    "  return o;\n"
    "}\n"
    "float sd_box(float2 p, float2 h, float r) {\n"
    "  float2 q = abs(p) - h + r;\n"
    "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "float4 PSMain(VSOut i) : SV_TARGET {\n"
    "  float soft = max(i.prm.z, 0.0001);\n"
    "  float a = saturate(0.5 - sd_box(i.lp, i.hs, i.prm.x) / soft);\n"
    "  if (i.prm.y > 0.0) {\n"
    "    float di = sd_box(i.lp, i.hs - i.prm.y, max(i.prm.x - i.prm.y, 0.0));\n"
    "    a *= saturate(0.5 + di / soft);\n"
    "  }\n"
    "  float t = lerp(1.0, atlas.Sample(smp, i.uv).r, i.prm.w);\n"
    "  return float4(i.col.rgb, i.col.a * a * t);\n"
    "}\n";

static void ui_render_log(UIContext *ui, const char *msg)
{
    if (ui->platform && ui->platform->log)
        ui->platform->log(msg);
}

b32 ui_render_init(UIContext *ui, RendererAPI *api, Renderer *renderer)
{
    ui->render_ok = false;
    ui->vertex_shader = api->create_shader_hlsl(renderer, SHADER_VERTEX, UI_HLSL, "VSMain");
    ui->pixel_shader = api->create_shader_hlsl(renderer, SHADER_PIXEL, UI_HLSL, "PSMain");
    if (!ui->vertex_shader.id || !ui->pixel_shader.id)
    {
        ui_render_log(ui, "ui: shader compile failed (see compiler output above); UI disabled");
        return false;
    }

    PipelineDesc pd = {0};
    pd.vertex_shader = ui->vertex_shader;
    pd.pixel_shader = ui->pixel_shader;
    pd.topology = PRIMITIVE_TRIANGLE_STRIP;
    pd.blend = BLEND_ALPHA;
    pd.scissor_test = true;
    pd.attributes[pd.attribute_count++] = {"UICORNER", 0, VERTEX_FORMAT_FLOAT2, 0, 0, false};
    pd.attributes[pd.attribute_count++] = {"UIRECT", 0, VERTEX_FORMAT_FLOAT4, 1, 0, true};
    pd.attributes[pd.attribute_count++] = {"UIUV", 0, VERTEX_FORMAT_FLOAT4, 1, 16, true};
    pd.attributes[pd.attribute_count++] = {"UICOLOR", 0, VERTEX_FORMAT_FLOAT4, 1, 32, true};
    pd.attributes[pd.attribute_count++] = {"UIPARAMS", 0, VERTEX_FORMAT_FLOAT4, 1, 48, true};
    ui->pipeline = api->create_pipeline(renderer, &pd);
    if (!ui->pipeline.id)
    {
        ui_render_log(ui, "ui: pipeline creation failed; UI disabled");
        return false;
    }

    UICorner corners[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    BufferDesc cb;
    memset(&cb, 0, sizeof(cb));
    cb.usage = BUFFER_VERTEX;
    cb.size = sizeof(corners);
    cb.initial_data = corners;
    ui->corner_buffer = api->create_buffer(renderer, &cb);

    BufferDesc qb;
    memset(&qb, 0, sizeof(qb));
    qb.usage = BUFFER_VERTEX;
    qb.size = (usize)ui->max_quads * sizeof(UIQuad); // DEFAULT usage, updated per frame like the game's instance buffer
    ui->quad_buffer = api->create_buffer(renderer, &qb);
    if (!ui->corner_buffer.id || !ui->quad_buffer.id)
    {
        ui_render_log(ui, "ui: vertex buffer creation failed; UI disabled");
        return false;
    }

    if (ui->font.ok)
    {
        TextureDesc td = {0};
        td.width = UI_FONT_ATLAS_SIZE;
        td.height = UI_FONT_ATLAS_SIZE;
        td.format = TEX_R8;
        td.initial_data = ui->font.atlas_pixels;
        ui->font.texture = api->create_texture(renderer, &td);
        if (!ui->font.texture.id)
        {
            ui_render_log(ui, "ui: font atlas texture creation failed; text will not render");
        }
    }
    ui->render_ok = true;
    return true;
}

// -------------------------- draw list (thread 0) -------------------------
static FORCE_INLINE UIRect ui_rect_intersect(UIRect a, UIRect b)
{
    UIRect r;
    r.x0 = Max(a.x0, b.x0);
    r.y0 = Max(a.y0, b.y0);
    r.x1 = Min(a.x1, b.x1);
    r.y1 = Min(a.y1, b.y1);
    return r;
}

// Must match ui_box_emit_quads exactly. Text is one quad per byte (spaces are degenerate).
static FORCE_INLINE u32 ui_box_quad_count(const UIBox *b)
{
    u32 n = 0;
    if (b->flags & UI_BOX_DRAW_BACKGROUND)
    {
        n++;
    }
    if (b->flags & UI_BOX_DRAW_BORDER)
    {
        n++;
    }
    if (b->flags & UI_BOX_DRAW_TEXT)
    {
        n += b->text_len;
    }
    return n;
}

static void ui_flatten_box(UIContext *ui, UIBox *b, UIRect clip)
{
    b->clip = clip;
    b->first_quad = ui->quad_count;
    b->quad_count = 0;
    UIRect vis = ui_rect_intersect(b->rect, clip);
    b32 visible = (vis.x1 > vis.x0) && (vis.y1 > vis.y0);
    if (visible)
    {
        u32 n = ui_box_quad_count(b);
        if (n > 0)
        {
            UIDrawBatch *batch = 0;
            if (ui->quad_count + n <= ui->max_quads)
            {
                batch = ui->batch_count ? &ui->batches[ui->batch_count - 1] : 0;
                if (!batch || memcmp(&batch->scissor, &clip, sizeof(clip)) != 0)
                {
                    batch = 0;
                    if (ui->batch_count < UI_MAX_DRAW_BATCHES)
                    {
                        batch = &ui->batches[ui->batch_count++];
                        batch->scissor = clip;
                        batch->first_quad = ui->quad_count;
                        batch->quad_count = 0;
                    }
                }
            }
            if (batch)
            {
                batch->quad_count += n;
                b->quad_count = n;
                ui->quad_count += n;
                ui->draw_boxes[ui->draw_box_count++] = b;
            }
            else
            {
                ui->stats.quads_dropped += n;
            }
        }

        f32 mx = ui->input.mouse_x, my = ui->input.mouse_y;
        if (mx >= vis.x0 && mx < vis.x1 && my >= vis.y0 && my < vis.y1)
        {
            if ((b->flags & UI_BOX_CLICKABLE) && b->key.v)
            {
                ui->hit_box = b;
                UIBox *s = b;
                while (s && !((s->flags & UI_BOX_SCROLL_Y) && s->key.v))
                {
                    s = s->parent;
                }
                ui->hit_scroll = s;
            }
            else if ((b->flags & UI_BOX_SCROLL_Y) && b->key.v)
            {
                ui->hit_scroll = b;
            }
        }
    }
    if ((b->flags & UI_BOX_CLIP) && !visible)
    {
        return;
    }
    UIRect child_clip = (b->flags & UI_BOX_CLIP) ? vis : clip;
    for (UIBox *c = b->first; c; c = c->next)
    {
        ui_flatten_box(ui, c, child_clip);
    }
}

void ui_draw_flatten(UIContext *ui)
{
    ui->draw_boxes = PushArrayRaw(&ui->build, UIBox *, ui->stats.boxes_built);
    ui->batches = PushArrayRaw(&ui->build, UIDrawBatch, UI_MAX_DRAW_BATCHES);
    ui->draw_box_count = 0;
    ui->batch_count = 0;
    ui->quad_count = 0;
    ui->hit_box = 0;
    ui->hit_scroll = 0;
    ui_flatten_box(ui, ui->root, ui->root->rect);
    ui->hot_key.v = ui->hit_box ? ui->hit_box->key.v : 0;
    ui->scroll_key.v = ui->hit_scroll ? ui->hit_scroll->key.v : 0;
}

// -------------------------- quad fill (any thread) -------------------------
static FORCE_INLINE void ui_quad_write(UIQuad *q, f32 x0, f32 y0, f32 x1, f32 y1,
                                       f32 u0, f32 v0, f32 u1, f32 v1, UIColor c,
                                       f32 radius, f32 thickness, f32 softness, f32 atlas_mix)
{
    q->rect[0] = x0;
    q->rect[1] = y0;
    q->rect[2] = x1;
    q->rect[3] = y1;
    q->uv[0] = u0;
    q->uv[1] = v0;
    q->uv[2] = u1;
    q->uv[3] = v1;
    q->color[0] = c.r;
    q->color[1] = c.g;
    q->color[2] = c.b;
    q->color[3] = c.a;
    q->params[0] = radius;
    q->params[1] = thickness;
    q->params[2] = softness;
    q->params[3] = atlas_mix;
}
static FORCE_INLINE UIColor ui_color_lerp(UIColor a, UIColor b, f32 t)
{
    UIColor r = {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
    return r;
}

static void ui_box_emit_quads(const UIContext *ui, const UIBox *b, UIQuad *q)
{
    const UITheme *th = &ui->theme;
    UIRect r = b->rect;
    if (b->flags & UI_BOX_DRAW_BACKGROUND)
    {
        UIColor c = b->background;
        if (b->flags & UI_BOX_HOT_ANIM)
        {
            c = ui_color_lerp(c, th->widget_hot, b->hot_t);
            c = ui_color_lerp(c, th->widget_active, b->active_t);
        }
        ui_quad_write(q++, r.x0, r.y0, r.x1, r.y1, 0, 0, 0, 0, c, b->corner_radius, 0.0f, 1.0f, 0.0f);
    }
    if (b->flags & UI_BOX_DRAW_BORDER)
    {
        ui_quad_write(q++, r.x0, r.y0, r.x1, r.y1, 0, 0, 0, 0, b->border_color,
                      b->corner_radius, b->border_thickness, 1.0f, 0.0f);
    }
    if ((b->flags & UI_BOX_DRAW_TEXT) && b->text_len)
    {
        const UIFont *f = &ui->font;
        f32 w = r.x1 - r.x0, h = r.y1 - r.y0;
        f32 pen = (b->flags & UI_BOX_TEXT_CENTER)
                      ? r.x0 + (w - ui_font_text_width(f, b->text, b->text_len)) * 0.5f
                      : r.x0 + b->text_pad;
        pen = floorf(pen);
        f32 baseline = floorf(r.y0 + (h - f->line_height) * 0.5f + f->ascent);
        for (u32 i = 0; i < b->text_len; ++i)
        {
            const UIGlyph *g = ui_font_glyph(f, b->text[i]);
            ui_quad_write(q++, pen + g->x0, baseline + g->y0, pen + g->x1, baseline + g->y1,
                          g->u0, g->v0, g->u1, g->v1, b->text_color, 0.0f, 0.0f, 0.0f, 1.0f);
            pen += g->advance;
        }
    }
}

// Thread 0, right after render_list_reset: reserve the upload block and record every
// command. The block's contents are filled in by ui_render_gather on all threads.
void ui_render_prepare(UIContext *ui, RenderList *list)
{
    ui->quad_dst = 0;
    u32 w = ui->input.window_w, h = ui->input.window_h;
    if (!ui->render_ok || ui->quad_count == 0 || w == 0 || h == 0)
    {
        return;
    }

    u32 upload_offset = 0;
    ui->quad_dst = (UIQuad *)render_upload_alloc(list, (usize)ui->quad_count * sizeof(UIQuad), &upload_offset);
    work_counter_init(&ui->draw_chunks, (ui->draw_box_count + UI_DRAW_CHUNK_BOXES - 1) / UI_DRAW_CHUNK_BOXES);

    u64 key = RENDER_SORT_LAYER_UI;
    RenderCommand *c;

    // Same shape and order as the game's frame: upload first, then bind, then draw.
    c = render_cmd_push(list, 0, RENDER_CMD_UPDATE_BUFFER);
    c->sort_key = key++;
    c->update_buffer.buffer = ui->quad_buffer.id;
    c->update_buffer.dst_offset = 0;
    c->update_buffer.size = ui->quad_count * (u32)sizeof(UIQuad);
    c->update_buffer.upload_offset = upload_offset;

    c = render_cmd_push(list, 0, RENDER_CMD_SET_VIEWPORT);
    c->sort_key = key++;
    c->viewport.w = (f32)w;
    c->viewport.h = (f32)h;

    c = render_cmd_push(list, 0, RENDER_CMD_SET_PIPELINE);
    c->sort_key = key++;
    c->pipeline.pipeline = ui->pipeline.id;

    f32 frame_constants[4] = {1.0f / (f32)w, 1.0f / (f32)h, 0.0f, 0.0f};
    c = render_cmd_push(list, 0, RENDER_CMD_SET_CONSTANTS);
    c->sort_key = key++;
    c->constants.slot = 0;
    c->constants.size = sizeof(frame_constants);
    memcpy(c->constants.data, frame_constants, sizeof(frame_constants));

    c = render_cmd_push(list, 0, RENDER_CMD_SET_VERTEX_BUFFER);
    c->sort_key = key++;
    c->vertex_buffer.buffer = ui->corner_buffer.id;
    c->vertex_buffer.slot = 0;
    c->vertex_buffer.stride = sizeof(UICorner);

    c = render_cmd_push(list, 0, RENDER_CMD_SET_VERTEX_BUFFER);
    c->sort_key = key++;
    c->vertex_buffer.buffer = ui->quad_buffer.id;
    c->vertex_buffer.slot = 1;
    c->vertex_buffer.stride = sizeof(UIQuad);

    c = render_cmd_push(list, 0, RENDER_CMD_SET_TEXTURE);
    c->sort_key = key++;
    c->texture.texture = ui->font.texture.id;
    c->texture.slot = 0;

    for (u32 i = 0; i < ui->batch_count; ++i)
    {
        const UIDrawBatch *b = &ui->batches[i];
        c = render_cmd_push(list, 0, RENDER_CMD_SET_SCISSOR);
        c->sort_key = key++;
        c->scissor.x = (i32)b->scissor.x0;
        c->scissor.y = (i32)b->scissor.y0;
        c->scissor.w = (i32)(b->scissor.x1 - b->scissor.x0);
        c->scissor.h = (i32)(b->scissor.y1 - b->scissor.y0);

        c = render_cmd_push(list, 0, RENDER_CMD_DRAW_INSTANCED);
        c->sort_key = key++;
        c->draw_instanced.vertex_count = 4;
        c->draw_instanced.instance_count = b->quad_count;
        c->draw_instanced.first_vertex = 0;
        c->draw_instanced.first_instance = b->first_quad;
    }
}

// All threads. Each box owns a disjoint [first_quad, first_quad + quad_count) range,
// so idle threads just keep grabbing the next chunk of boxes.
void ui_render_gather(UIContext *ui)
{
    if (!ui->quad_dst)
    {
        return;
    }

    u32 chunk;
    while (work_counter_next(&ui->draw_chunks, &chunk))
    {
        u32 begin = chunk * UI_DRAW_CHUNK_BOXES;
        u32 end = Min(begin + UI_DRAW_CHUNK_BOXES, ui->draw_box_count);
        for (u32 i = begin; i < end; ++i)
        {
            const UIBox *b = ui->draw_boxes[i];
            ui_box_emit_quads(ui, b, ui->quad_dst + b->first_quad);
        }
    }
}