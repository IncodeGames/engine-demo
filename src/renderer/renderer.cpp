#include "renderer/renderer.h"
#include <string.h>

void render_list_init(RenderList *list, Arena *cmd_arena, Arena *upload_arena,
                      u32 thread_count, u32 per_thread, usize upload_bytes)
{
    list->thread_count = thread_count;
    list->buffers = PushArray(cmd_arena, RenderCommandBuffer, thread_count);
    for (u32 i = 0; i < thread_count; ++i)
    {
        list->buffers[i].commands = PushArray(cmd_arena, RenderCommand, per_thread);
        list->buffers[i].capacity = per_thread;
        list->buffers[i].count = 0;
    }
    void *mem = arena_push(upload_arena, upload_bytes, 16);
    arena_init(&list->upload, mem, upload_bytes, "render-upload");
}

void render_list_reset(RenderList *list)
{
    for (u32 i = 0; i < list->thread_count; ++i)
        list->buffers[i].count = 0;
    arena_reset(&list->upload);
}

ENGINE_API RenderCommand *render_cmd_push(RenderList *list, u32 thread_index, RenderCommandType type)
{
    RenderCommandBuffer *b = &list->buffers[thread_index];
    ASSERT(b->count < b->capacity && "render command buffer overflow");
    RenderCommand *cmd = &b->commands[b->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = (u32)type;
    return cmd;
}

ENGINE_API void *render_upload_alloc(RenderList *list, usize size, u32 *out_offset)
{
    void *p = arena_push_atomic(&list->upload, size);
    *out_offset = (u32)((u8 *)p - list->upload.base);
    return p;
}

u32 render_list_sort(const RenderList *list, SortedCommand *out, SortedCommand *tmp, u32 capacity)
{
    u32 n = 0;
    for (u32 t = 0; t < list->thread_count; ++t)
    {
        const RenderCommandBuffer *b = &list->buffers[t];
        for (u32 i = 0; i < b->count; ++i)
        {
            if (n >= capacity)
            {
                ASSERT(!"sorted render command capacity exceeded");
                return n;
            }
            out[n].sort_key = b->commands[i].sort_key;
            out[n].cmd = &b->commands[i];
            ++n;
        }
    }
    SortedCommand *src = out;
    SortedCommand *dst = tmp;
    for (u32 pass = 0; pass < 8; ++pass)
    {
        u32 shift = pass * 8;
        u32 hist[256];
        memset(hist, 0, sizeof(hist));
        for (u32 i = 0; i < n; ++i)
            hist[(src[i].sort_key >> shift) & 0xFF]++;
        u32 sum = 0;
        for (u32 b = 0; b < 256; ++b)
        {
            u32 c = hist[b];
            hist[b] = sum;
            sum += c;
        }
        for (u32 i = 0; i < n; ++i)
        {
            u32 bucket = (u32)((src[i].sort_key >> shift) & 0xFF);
            dst[hist[bucket]++] = src[i];
        }
        SortedCommand *swap = src;
        src = dst;
        dst = swap;
    }
    return n;
}