#pragma once
#include "shared/types.h"
#include "shared/assert.h"
#include "memory/arena.h"
#include "platform/platform.h"

typedef struct Renderer Renderer;

typedef struct BufferHandle
{
    u32 id;
} BufferHandle;
typedef struct TextureHandle
{
    u32 id;
} TextureHandle;
typedef struct ShaderHandle
{
    u32 id;
} ShaderHandle;
typedef struct PipelineHandle
{
    u32 id;
} PipelineHandle;

typedef enum BufferUsage
{
    BUFFER_VERTEX,
    BUFFER_INDEX,
    BUFFER_CONSTANT
} BufferUsage;
typedef enum TextureFormat
{
    TEX_RGBA8,
    TEX_D32F,
    TEX_R8
} TextureFormat;
typedef enum BlendMode
{
    BLEND_OPAQUE,
    BLEND_ALPHA // straight alpha: src * a + dst * (1 - a)
} BlendMode;
typedef enum ShaderStage
{
    SHADER_VERTEX,
    SHADER_PIXEL
} ShaderStage;

typedef enum VertexFormat
{
    VERTEX_FORMAT_FLOAT,
    VERTEX_FORMAT_FLOAT2,
    VERTEX_FORMAT_FLOAT3,
    VERTEX_FORMAT_FLOAT4,
    VERTEX_FORMAT_UBYTE4_UNORM
} VertexFormat;

typedef enum PrimitiveTopology
{
    PRIMITIVE_TRIANGLE_LIST,
    PRIMITIVE_TRIANGLE_STRIP,
    PRIMITIVE_LINE_LIST
} PrimitiveTopology;

typedef struct VertexAttribute
{
    const char *semantic; // valid only during the create call
    u32 semantic_index;
    VertexFormat format;
    u32 input_slot;   // which bound vertex buffer this attribute reads from
    u32 offset;       // byte offset within an element in that slot
    b32 per_instance; // false => per-vertex
} VertexAttribute;

typedef struct BufferDesc
{
    BufferUsage usage;
    usize size;
    const void *initial_data;
    b32 dynamic;
} BufferDesc;
typedef struct TextureDesc
{
    u32 width, height;
    TextureFormat format;
    const void *initial_data;
    b32 render_target;
} TextureDesc;
typedef struct ShaderDesc
{
    ShaderStage stage;
    const void *bytecode;
    usize bytecode_size;
} ShaderDesc;

typedef struct PipelineDesc
{
    ShaderHandle vertex_shader;
    ShaderHandle pixel_shader;
    VertexAttribute attributes[16];
    u32 attribute_count;
    PrimitiveTopology topology;
    BlendMode blend;
    b32 scissor_test;
} PipelineDesc;

// ---------------------------------------------------------- command list ---
#define RENDER_MAX_COMMANDS_PER_THREAD 4096
#define RENDER_INLINE_CONSTANTS_SIZE 128
#define RENDER_SORT_LAYER_UI (0xF0ull << 56)

typedef enum RenderCommandType
{
    RENDER_CMD_SET_VIEWPORT,
    RENDER_CMD_CLEAR,
    RENDER_CMD_SET_PIPELINE,
    RENDER_CMD_SET_VERTEX_BUFFER,
    RENDER_CMD_SET_INDEX_BUFFER,
    RENDER_CMD_SET_CONSTANTS,
    RENDER_CMD_UPDATE_BUFFER,
    RENDER_CMD_DRAW,
    RENDER_CMD_DRAW_INSTANCED,
    RENDER_CMD_DRAW_INDEXED,
    RENDER_CMD_SET_SCISSOR,
    RENDER_CMD_SET_TEXTURE
} RenderCommandType;

// Commands execute in ascending sort_key order (stable within equal keys). Data
// copied by RENDER_CMD_UPDATE_BUFFER lives in the frame's upload area
// (render_upload_alloc); do not reference per-frame arenas from a command.
typedef struct RenderCommand
{
    u32 type;
    u64 sort_key;
    union
    {
        struct
        {
            f32 x, y, w, h;
        } viewport;
        struct
        {
            f32 color[4];
            f32 depth;
            b32 clear_color;
            b32 clear_depth;
        } clear;
        struct
        {
            u32 pipeline;
        } pipeline;
        struct
        {
            u32 buffer;
            u32 slot;
            u32 stride;
            u32 offset;
        } vertex_buffer;
        struct
        {
            u32 buffer;
            u32 offset;
            b32 index32;
        } index_buffer;
        struct
        {
            u32 slot;
            u32 stage;
            u32 size;
            u8 data[RENDER_INLINE_CONSTANTS_SIZE];
        } constants;
        struct
        {
            u32 buffer;
            u32 dst_offset;
            u32 size;
            u32 upload_offset;
        } update_buffer;
        struct
        {
            u32 vertex_count;
            u32 first_vertex;
        } draw;
        struct
        {
            u32 vertex_count;
            u32 instance_count;
            u32 first_vertex;
            u32 first_instance;
        } draw_instanced;
        struct
        {
            u32 index_count;
            u32 first_index;
            i32 base_vertex;
        } draw_indexed;
        struct
        {
            i32 x, y, w, h;
        } scissor;
        struct
        {
            u32 texture;
            u32 slot;
        } texture;
    };
} RenderCommand;

typedef struct RenderCommandBuffer
{
    RenderCommand *commands;
    u32 count;
    u32 capacity;
} RenderCommandBuffer;

typedef struct RenderList
{
    RenderCommandBuffer *buffers; // one per worker thread
    u32 thread_count;
    Arena upload; // per-frame CPU staging for buffer uploads
} RenderList;

typedef struct SortedCommand
{
    u64 sort_key;
    const RenderCommand *cmd;
} SortedCommand;

void render_list_init(RenderList *list, Arena *cmd_arena, Arena *upload_arena,
                      u32 thread_count, u32 commands_per_thread, usize upload_bytes);
void render_list_reset(RenderList *list);
u32 render_list_sort(const RenderList *list, SortedCommand *out, SortedCommand *tmp, u32 capacity);

ENGINE_API RenderCommand *render_cmd_push(RenderList *list, u32 thread_index, RenderCommandType type);

// Reserve 'size' bytes in the frame's upload staging area. Write buffer data into
// the returned pointer, then reference it from RENDER_CMD_UPDATE_BUFFER via
// *out_offset. Thread-safe (atomic bump).
ENGINE_API void *render_upload_alloc(RenderList *list, usize size, u32 *out_offset);

// ------------------------------------------------------ backend interface ---
typedef struct RendererInitDesc
{
    void *native_window_handle;
    u32 width, height;
    u32 worker_thread_count;
    u32 max_commands_per_thread;
    PlatformAPI *platform;
    Arena *persistent;
} RendererInitDesc;

typedef struct RendererAPI
{
    Renderer *(*create)(Arena *persistent);
    b32 (*init)(Renderer *r, const RendererInitDesc *desc);
    void (*shutdown)(Renderer *r);
    void (*resize)(Renderer *r, u32 width, u32 height);
    BufferHandle (*create_buffer)(Renderer *r, const BufferDesc *desc);
    TextureHandle (*create_texture)(Renderer *r, const TextureDesc *desc);
    ShaderHandle (*create_shader)(Renderer *r, const ShaderDesc *desc);
    ShaderHandle (*create_shader_hlsl)(Renderer *r, ShaderStage stage,
                                       const char *source, const char *entry_point);
    ShaderHandle (*reserve_shader)(Renderer *r, ShaderStage stage);
    b32 (*compile_shader_hlsl)(Renderer *r, ShaderHandle shader, const char *source, usize source_size,
                               const char *source_name, const char *entry_point);
    PipelineHandle (*create_pipeline)(Renderer *r, const PipelineDesc *desc);
    void (*execute)(Renderer *r, const RenderList *list);
    void (*present)(Renderer *r);
} RendererAPI;

void renderer_d3d11_get_api(RendererAPI *out);