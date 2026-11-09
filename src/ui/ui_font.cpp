#include "ui/ui_internal.h"
#include <string.h>
#include <math.h>

// stb_truetype allocations routed to a scratch arena via fontinfo.userdata.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(x, u) arena_push((Arena *)(u), (usize)(x), 16)
#define STBTT_free(x, u) ((void)(x), (void)(u))
#define STBTT_assert(x) ASSERT(x)
#pragma warning(push, 0)
#include "third_party/stb_truetype.h"
#pragma warning(pop)

// Rough layout when no font could be loaded.
// TODO(BPL): Fallback to separate font 2026-07-20
void ui_font_init_fallback(UIFont *font, f32 pixel_height)
{
    u8 *pixels = font->atlas_pixels;
    memset(font, 0, sizeof(*font));
    font->atlas_pixels = pixels;
    font->pixel_height = pixel_height;
    font->ascent = floorf(pixel_height * 0.8f);
    font->descent = floorf(pixel_height * 0.2f);
    font->line_height = font->ascent + font->descent;
    for (u32 i = 0; i < UI_FONT_GLYPH_COUNT; ++i)
        font->glyphs[i].advance = floorf(pixel_height * 0.55f);
}

f32 ui_font_text_width(const UIFont *font, const char *text, u32 len)
{
    f32 w = 0.0f;
    for (u32 i = 0; i < len; ++i)
        w += ui_font_glyph(font, text[i])->advance;
    return w;
}

static b32 ui_font_bake(UIFont *font, Arena *scratch, const u8 *ttf, f32 pixel_height)
{
    stbtt_fontinfo info;
    memset(&info, 0, sizeof(info));
    info.userdata = scratch;
    int offset = stbtt_GetFontOffsetForIndex(ttf, 0);
    if (offset < 0 || !stbtt_InitFont(&info, ttf, offset))
    {
        return false;
    }

    f32 scale = stbtt_ScaleForPixelHeight(&info, pixel_height);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    font->ascent = ceilf((f32)ascent * scale);
    font->descent = ceilf((f32)-descent * scale);
    font->line_height = font->ascent + font->descent;

    const int aw = UI_FONT_ATLAS_SIZE, ah = UI_FONT_ATLAS_SIZE;
    memset(font->atlas_pixels, 0, (usize)aw * (usize)ah);
    int pen_x = 1, pen_y = 1, row_h = 0; // shelf packer with 1px gutters
    for (int i = 0; i < UI_FONT_GLYPH_COUNT; ++i)
    {
        int cp = UI_FONT_FIRST_CHAR + i;
        UIGlyph *g = &font->glyphs[i];
        int advance, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&info, cp, &advance, &lsb);
        stbtt_GetCodepointBitmapBox(&info, cp, scale, scale, &x0, &y0, &x1, &y1);
        memset(g, 0, sizeof(*g));
        g->advance = floorf((f32)advance * scale + 0.5f);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0)
        {
            continue; // whitespace
        }
        if (pen_x + gw + 1 > aw)
        {
            pen_x = 1;
            pen_y += row_h + 1;
            row_h = 0;
        }
        if (pen_y + gh + 1 > ah)
        {
            return false; // atlas too small for this pixel height
        }

        ArenaScope tmp = arena_scope_begin(scratch);
        stbtt_MakeCodepointBitmap(&info, font->atlas_pixels + pen_y * aw + pen_x, gw, gh, aw, scale, scale, cp);
        arena_scope_end(tmp);

        g->x0 = (f32)x0;
        g->y0 = (f32)y0;
        g->x1 = (f32)x1;
        g->y1 = (f32)y1;
        g->u0 = (f32)pen_x / (f32)aw;
        g->v0 = (f32)pen_y / (f32)ah;
        g->u1 = (f32)(pen_x + gw) / (f32)aw;
        g->v1 = (f32)(pen_y + gh) / (f32)ah;
        pen_x += gw + 1;
        if (gh > row_h)
            row_h = gh;
    }
    font->ok = true;
    return true;
}

// Call before init. Uses the UI build arena as scratch. Empty until the first ui_begin_frame.
// TODO(BPL): Build this into init?
b32 ui_font_load(UIContext *ui, PlatformAPI *platform, const char *ttf_path, f32 pixel_height)
{
    UIFont *font = &ui->font;
    ui_font_init_fallback(font, pixel_height);
    ArenaScope scope = arena_scope_begin(&ui->build);
    PlatformFileContents file = platform->file_read(ttf_path, &ui->build);
    b32 ok = false;
    if (!file.ok)
    {
        char msg[320];
        snprintf(msg, sizeof(msg), "ui: could not read font '%s'", ttf_path);
        platform->log(msg);
    }
    else
    {
        ok = ui_font_bake(font, &ui->build, (const u8 *)file.data, pixel_height);
        if (!ok)
        {
            platform->log("ui: font bake failed (unsupported file or atlas too small for this size)");
        }
    }
    arena_scope_end(scope);
    if (!ok)
    {
        ui_font_init_fallback(font, pixel_height);
    }
    ui->theme.row_height = font->line_height + 6.0f;
    return ok;
}