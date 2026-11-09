#include "renderer/renderer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <string.h>

#define D3D11_MAX_BUFFERS 1024
#define D3D11_MAX_SHADERS 256
#define D3D11_MAX_PIPELINES 256
#define D3D11_CONSTANT_SLOTS 8
#define D3D11_MAX_TEXTURES 256
#define D3D11_TEXTURE_SLOTS 8
#define D3D11_SEMANTIC_NAME_MAX 32

typedef struct D3D11Buffer
{
    ID3D11Buffer *buffer;
} D3D11Buffer;

typedef struct D3D11Shader
{
    ShaderStage stage;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3DBlob *blob;
    const void *bytecode_ptr;
    usize bytecode_size;
} D3D11Shader;

typedef struct D3D11Texture
{
    ID3D11Texture2D *texture;
    ID3D11ShaderResourceView *srv;
} D3D11Texture;

typedef struct D3D11Pipeline
{
    ID3D11InputLayout *layout;
    u32 vertex_shader;
    u32 pixel_shader;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11BlendState *blend; // null = opaque
    ID3D11RasterizerState *raster;
    VertexAttribute attributes[16];
    char semantics[16][D3D11_SEMANTIC_NAME_MAX];
    u32 attribute_count;
} D3D11Pipeline;

struct Renderer
{
    PlatformAPI *platform;
    HWND hwnd;
    b32 ok;

    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain *swapchain;
    ID3D11RenderTargetView *backbuffer_rtv;
    ID3D11RasterizerState *raster_state;
    u32 width, height;

    ID3D11RasterizerState *raster_state_scissor;
    ID3D11BlendState *blend_alpha;
    ID3D11SamplerState *sampler_linear;
    D3D11Texture textures[D3D11_MAX_TEXTURES];
    u32 texture_count;

    D3D11Buffer buffers[D3D11_MAX_BUFFERS];
    u32 buffer_count;
    D3D11Shader shaders[D3D11_MAX_SHADERS];
    u32 shader_count;
    D3D11Pipeline pipelines[D3D11_MAX_PIPELINES];
    u32 pipeline_count;
    b32 pipeline_ready;
    ID3D11Buffer *constant_buffers[D3D11_CONSTANT_SLOTS];

    SortedCommand *sort_out;
    SortedCommand *sort_tmp;
    u32 sort_capacity;
};

static void d3d11_log(Renderer *r, const char *msg)
{
    if (r->platform && r->platform->log)
    {
        r->platform->log(msg);
    }
}

static DXGI_FORMAT to_dxgi(VertexFormat f)
{
    switch (f)
    {
    case VERTEX_FORMAT_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case VERTEX_FORMAT_FLOAT2:
        return DXGI_FORMAT_R32G32_FLOAT;
    case VERTEX_FORMAT_FLOAT3:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case VERTEX_FORMAT_FLOAT4:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VERTEX_FORMAT_UBYTE4_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

static D3D11_PRIMITIVE_TOPOLOGY to_topology(PrimitiveTopology t)
{
    switch (t)
    {
    case PRIMITIVE_TRIANGLE_LIST:
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PRIMITIVE_TRIANGLE_STRIP:
        return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case PRIMITIVE_LINE_LIST:
        return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

static Renderer *d3d11_create(Arena *persistent) { return PushStruct(persistent, Renderer); }

static b32 d3d11_create_backbuffer(Renderer *r)
{
    ID3D11Texture2D *backbuffer = 0;
    if (FAILED(r->swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backbuffer)))
        return false;
    HRESULT hr = r->device->CreateRenderTargetView(backbuffer, 0, &r->backbuffer_rtv);
    backbuffer->Release();
    return SUCCEEDED(hr);
}

static b32 d3d11_init(Renderer *r, const RendererInitDesc *desc)
{
    r->platform = desc->platform;
    r->hwnd = (HWND)desc->native_window_handle;
    r->width = desc->width ? desc->width : 1;
    r->height = desc->height ? desc->height : 1;

    r->sort_capacity = desc->worker_thread_count * desc->max_commands_per_thread;
    r->sort_out = PushArray(desc->persistent, SortedCommand, r->sort_capacity);
    r->sort_tmp = PushArray(desc->persistent, SortedCommand, r->sort_capacity);

    DXGI_SWAP_CHAIN_DESC scd = {0};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = r->width;
    scd.BufferDesc.Height = r->height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = r->hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
#if ENGINE_DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, flags, want, 1,
                                               D3D11_SDK_VERSION, &scd, &r->swapchain, &r->device, &got, &r->context);
#if ENGINE_DEBUG
    if (FAILED(hr))
    {
        flags &= ~(UINT)D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, flags, want, 1,
                                           D3D11_SDK_VERSION, &scd, &r->swapchain, &r->device, &got, &r->context);
    }
#endif
    if (FAILED(hr))
    {
        d3d11_log(r, "d3d11: device creation failed");
        return false;
    }
    if (!d3d11_create_backbuffer(r))
    {
        d3d11_log(r, "d3d11: render target view failed");
        return false;
    }

    D3D11_RASTERIZER_DESC rs = {0};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE; // stress test: never cull
    rs.DepthClipEnable = TRUE;
    r->device->CreateRasterizerState(&rs, &r->raster_state);

    rs.ScissorEnable = TRUE;
    r->device->CreateRasterizerState(&rs, &r->raster_state_scissor);
    D3D11_BLEND_DESC bs;
    memset(&bs, 0, sizeof(bs));
    bs.RenderTarget[0].BlendEnable = TRUE;
    bs.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bs.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bs.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bs.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bs.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bs.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bs.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    r->device->CreateBlendState(&bs, &r->blend_alpha);
    D3D11_SAMPLER_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    r->device->CreateSamplerState(&sd, &r->sampler_linear);

    D3D11_BUFFER_DESC cbd = {0};
    cbd.ByteWidth = RENDER_INLINE_CONSTANTS_SIZE;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (u32 i = 0; i < D3D11_CONSTANT_SLOTS; ++i)
        r->device->CreateBuffer(&cbd, 0, &r->constant_buffers[i]);

    r->ok = true;
    return true;
}

static void d3d11_resize(Renderer *r, u32 w, u32 h)
{
    if (!r->ok || w == 0 || h == 0 || (w == r->width && h == r->height))
        return;
    r->context->OMSetRenderTargets(0, 0, 0);
    if (r->backbuffer_rtv)
    {
        r->backbuffer_rtv->Release();
        r->backbuffer_rtv = 0;
    }
    r->swapchain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    r->width = w;
    r->height = h;
    d3d11_create_backbuffer(r);
}

static void d3d11_shutdown(Renderer *r)
{
    if (!r->device)
        return;
    for (u32 i = 0; i < D3D11_CONSTANT_SLOTS; ++i)
        if (r->constant_buffers[i])
            r->constant_buffers[i]->Release();
    for (u32 i = 0; i < r->pipeline_count; ++i)
        if (r->pipelines[i].layout)
            r->pipelines[i].layout->Release();
    for (u32 i = 0; i < r->shader_count; ++i)
    {
        if (r->shaders[i].vs)
            r->shaders[i].vs->Release();
        if (r->shaders[i].ps)
            r->shaders[i].ps->Release();
        if (r->shaders[i].blob)
            r->shaders[i].blob->Release();
    }
    for (u32 i = 0; i < r->buffer_count; ++i)
        if (r->buffers[i].buffer)
            r->buffers[i].buffer->Release();
    for (u32 i = 0; i < r->texture_count; ++i)
    {
        if (r->textures[i].srv)
            r->textures[i].srv->Release();
        if (r->textures[i].texture)
            r->textures[i].texture->Release();
    }
    if (r->sampler_linear)
        r->sampler_linear->Release();
    if (r->blend_alpha)
        r->blend_alpha->Release();
    if (r->raster_state_scissor)
        r->raster_state_scissor->Release();
    if (r->raster_state)
        r->raster_state->Release();
    if (r->backbuffer_rtv)
        r->backbuffer_rtv->Release();
    if (r->swapchain)
        r->swapchain->Release();
    if (r->context)
        r->context->Release();
    if (r->device)
        r->device->Release();
    PlatformAPI *plat = r->platform;
    memset(r, 0, sizeof(*r));
    r->platform = plat;
}

static BufferHandle d3d11_create_buffer(Renderer *r, const BufferDesc *desc)
{
    BufferHandle h = {0};
    if (!r->ok || r->buffer_count >= D3D11_MAX_BUFFERS)
        return h;

    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = (UINT)desc->size;
    switch (desc->usage)
    {
    case BUFFER_VERTEX:
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        break;
    case BUFFER_INDEX:
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        break;
    case BUFFER_CONSTANT:
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        break;
    }
    if (desc->dynamic)
    {
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    }
    else if (desc->initial_data)
    {
        bd.Usage = D3D11_USAGE_IMMUTABLE;
    }
    else
    {
        bd.Usage = D3D11_USAGE_DEFAULT;
    } // updates via RENDER_CMD_UPDATE_BUFFER

    D3D11_SUBRESOURCE_DATA init = {0};
    D3D11_SUBRESOURCE_DATA *init_ptr = 0;
    if (desc->initial_data)
    {
        init.pSysMem = desc->initial_data;
        init_ptr = &init;
    }

    ID3D11Buffer *buffer = 0;
    if (FAILED(r->device->CreateBuffer(&bd, init_ptr, &buffer)))
    {
        d3d11_log(r, "d3d11: CreateBuffer failed");
        return h;
    }

    u32 idx = r->buffer_count++;
    r->buffers[idx].buffer = buffer;
    h.id = idx + 1;
    return h;
}

static TextureHandle d3d11_create_texture(Renderer *r, const TextureDesc *desc)
{
    TextureHandle h = {0};
    if (!r->ok || r->texture_count >= D3D11_MAX_TEXTURES || desc->width == 0 || desc->height == 0)
        return h;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    u32 bytes_per_pixel = 0;
    switch (desc->format)
    {
    case TEX_R8:
        format = DXGI_FORMAT_R8_UNORM;
        bytes_per_pixel = 1;
        break;
    case TEX_RGBA8:
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
        bytes_per_pixel = 4;
        break;
    default:
        d3d11_log(r, "d3d11: texture format not supported yet"); // TODO: depth / render targets
        return h;
    }
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = desc->width;
    td.Height = desc->height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = desc->initial_data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {0};
    init.pSysMem = desc->initial_data;
    init.SysMemPitch = desc->width * bytes_per_pixel;
    ID3D11Texture2D *texture = 0;
    if (FAILED(r->device->CreateTexture2D(&td, desc->initial_data ? &init : 0, &texture)))
    {
        d3d11_log(r, "d3d11: CreateTexture2D failed");
        return h;
    }
    ID3D11ShaderResourceView *srv = 0;
    if (FAILED(r->device->CreateShaderResourceView(texture, 0, &srv)))
    {
        texture->Release();
        d3d11_log(r, "d3d11: CreateShaderResourceView failed");
        return h;
    }
    u32 idx = r->texture_count++;
    r->textures[idx].texture = texture;
    r->textures[idx].srv = srv;
    h.id = idx + 1;
    return h;
}

static void d3d11_release_shader(D3D11Shader *s)
{
    if (s->vs)
        s->vs->Release();
    if (s->ps)
        s->ps->Release();
    if (s->blob)
        s->blob->Release();
    s->vs = 0;
    s->ps = 0;
    s->blob = 0;
    s->bytecode_ptr = 0;
    s->bytecode_size = 0;
}
static b32 d3d11_build_shader(Renderer *r, D3D11Shader *out, const char *source, usize source_size,
                              const char *source_name, const char *entry)
{
    const char *target = (out->stage == SHADER_VERTEX) ? "vs_5_0" : "ps_5_0";
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if ENGINE_DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    // #include resolves relative to the source file; in-memory sources have nothing to resolve against.
    ID3DInclude *include = source_name ? D3D_COMPILE_STANDARD_FILE_INCLUDE : 0;
    ID3DBlob *code = 0;
    ID3DBlob *errors = 0;
    HRESULT hr = D3DCompile(source, source_size, source_name, 0, include, entry, target, flags, 0, &code, &errors);
    if (errors)
    {
        d3d11_log(r, (const char *)errors->GetBufferPointer());
        errors->Release();
    }
    if (FAILED(hr))
    {
        if (code)
            code->Release();
        return false;
    }
    hr = (out->stage == SHADER_VERTEX)
             ? r->device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &out->vs)
             : r->device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &out->ps);
    if (FAILED(hr))
    {
        code->Release();
        d3d11_log(r, "d3d11: shader object creation failed");
        return false;
    }
    out->blob = code;
    out->bytecode_ptr = code->GetBufferPointer();
    out->bytecode_size = code->GetBufferSize();
    return true;
}

static b32 d3d11_create_layout(Renderer *r, const D3D11Pipeline *p, const D3D11Shader *vs, ID3D11InputLayout **out)
{
    D3D11_INPUT_ELEMENT_DESC elems[16] = {0};
    for (u32 i = 0; i < p->attribute_count; ++i)
    {
        const VertexAttribute *a = &p->attributes[i];
        elems[i].SemanticName = p->semantics[i];
        elems[i].SemanticIndex = a->semantic_index;
        elems[i].Format = to_dxgi(a->format);
        elems[i].InputSlot = a->input_slot;
        elems[i].AlignedByteOffset = a->offset;
        elems[i].InputSlotClass = a->per_instance ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
        elems[i].InstanceDataStepRate = a->per_instance ? 1 : 0;
    }
    return SUCCEEDED(r->device->CreateInputLayout(elems, p->attribute_count,
                                                  vs->bytecode_ptr, vs->bytecode_size, out));
}

static ShaderHandle d3d11_create_shader_hlsl(Renderer *r, ShaderStage stage, const char *source, const char *entry)
{
    ShaderHandle h = {0};
    if (!r->ok || r->shader_count >= D3D11_MAX_SHADERS)
        return h;
    D3D11Shader s;
    memset(&s, 0, sizeof(s));
    s.stage = stage;
    if (!d3d11_build_shader(r, &s, source, strlen(source), 0, entry))
        return h;
    u32 idx = r->shader_count++;
    r->shaders[idx] = s;
    h.id = idx + 1;
    return h;
}

static ShaderHandle d3d11_reserve_shader(Renderer *r, ShaderStage stage)
{
    ShaderHandle h = {0};
    if (!r->ok || r->shader_count >= D3D11_MAX_SHADERS)
        return h;
    u32 idx = r->shader_count++;
    memset(&r->shaders[idx], 0, sizeof(r->shaders[idx]));
    r->shaders[idx].stage = stage;
    h.id = idx + 1;
    return h;
}

static b32 d3d11_compile_shader_hlsl(Renderer *r, ShaderHandle shader, const char *source, usize source_size,
                                     const char *source_name, const char *entry)
{
    if (!r->ok || shader.id == 0 || shader.id > r->shader_count)
        return false;
    D3D11Shader *current = &r->shaders[shader.id - 1];
    D3D11Shader fresh;
    memset(&fresh, 0, sizeof(fresh));
    fresh.stage = current->stage;
    if (!d3d11_build_shader(r, &fresh, source, source_size, source_name, entry))
    {
        return false;
    }

    ID3D11InputLayout *layouts[D3D11_MAX_PIPELINES] = {0};
    b32 layouts_ok = true;
    if (fresh.stage == SHADER_VERTEX)
    {
        for (u32 i = 0; i < r->pipeline_count && layouts_ok; ++i)
        {
            D3D11Pipeline *p = &r->pipelines[i];
            if (p->vertex_shader == shader.id && p->attribute_count > 0)
                layouts_ok = d3d11_create_layout(r, p, &fresh, &layouts[i]);
        }
    }
    if (!layouts_ok)
    {
        for (u32 i = 0; i < r->pipeline_count; ++i)
        {
            if (layouts[i])
            {
                layouts[i]->Release();
            }
        }
        d3d11_release_shader(&fresh);
        d3d11_log(r, "d3d11: vertex shader inputs do not match the vertex layout of a pipeline that uses it");
        return false;
    }
    for (u32 i = 0; i < r->pipeline_count; ++i)
    {
        if (!layouts[i])
        {
            continue;
        }

        if (r->pipelines[i].layout)
        {
            r->pipelines[i].layout->Release();
        }
        r->pipelines[i].layout = layouts[i];
    }
    d3d11_release_shader(current);
    *current = fresh;
    return true;
}

static ShaderHandle d3d11_create_shader(Renderer *r, const ShaderDesc *desc)
{
    ShaderHandle h = {0};
    if (!r->ok || r->shader_count >= D3D11_MAX_SHADERS)
        return h;
    D3D11Shader s = {0};
    s.stage = desc->stage;
    s.bytecode_ptr = desc->bytecode;
    s.bytecode_size = desc->bytecode_size;
    HRESULT hr = (desc->stage == SHADER_VERTEX)
                     ? r->device->CreateVertexShader(desc->bytecode, desc->bytecode_size, 0, &s.vs)
                     : r->device->CreatePixelShader(desc->bytecode, desc->bytecode_size, 0, &s.ps);
    if (FAILED(hr))
    {
        return h;
    }
    u32 idx = r->shader_count++;
    r->shaders[idx] = s;
    h.id = idx + 1;
    return h;
}

static PipelineHandle d3d11_create_pipeline(Renderer *r, const PipelineDesc *desc)
{
    PipelineHandle h = {0};
    if (!r->ok || r->pipeline_count >= D3D11_MAX_PIPELINES)
    {
        return h;
    }
    if (desc->vertex_shader.id == 0 || desc->vertex_shader.id > r->shader_count)
    {
        return h;
    }
    if (desc->attribute_count > ArrayCount(desc->attributes))
    {
        return h;
    }
    D3D11Shader *vs = &r->shaders[desc->vertex_shader.id - 1];
    D3D11Pipeline p;
    memset(&p, 0, sizeof(p));
    p.vertex_shader = desc->vertex_shader.id;
    p.pixel_shader = (desc->pixel_shader.id <= r->shader_count) ? desc->pixel_shader.id : 0;
    p.topology = to_topology(desc->topology);
    p.blend = (desc->blend == BLEND_ALPHA) ? r->blend_alpha : 0;
    p.raster = desc->scissor_test ? r->raster_state_scissor : r->raster_state;
    p.attribute_count = desc->attribute_count;
    for (u32 i = 0; i < desc->attribute_count; ++i)
    {
        usize len = strlen(desc->attributes[i].semantic);
        if (len >= D3D11_SEMANTIC_NAME_MAX)
        {
            d3d11_log(r, "d3d11: vertex attribute semantic name is too long");
            return h;
        }
        p.attributes[i] = desc->attributes[i];
        p.attributes[i].semantic = 0; // the caller's string is not ours to keep; the copy lives in p.semantics
        memcpy(p.semantics[i], desc->attributes[i].semantic, len + 1);
    }
    // A vertex shader that has not compiled yet gets its layout when it does.
    if (p.attribute_count > 0 && vs->bytecode_ptr && !d3d11_create_layout(r, &p, vs, &p.layout))
    {
        d3d11_log(r, "d3d11: CreateInputLayout failed");
        return h;
    }
    u32 idx = r->pipeline_count++;
    r->pipelines[idx] = p;
    h.id = idx + 1;
    return h;
}

static void d3d11_execute(Renderer *r, const RenderList *list)
{
    if (!r->ok)
    {
        return;
    }
    RECT rc;
    GetClientRect(r->hwnd, &rc);
    u32 cw = (u32)(rc.right - rc.left), ch = (u32)(rc.bottom - rc.top);
    if (cw && ch && (cw != r->width || ch != r->height))
        d3d11_resize(r, cw, ch);
    ID3D11DeviceContext *ctx = r->context;
    ctx->OMSetRenderTargets(1, &r->backbuffer_rtv, 0);
    ctx->RSSetState(r->raster_state);
    u32 n = render_list_sort(list, r->sort_out, r->sort_tmp, r->sort_capacity);
    for (u32 i = 0; i < n; ++i)
    {
        const RenderCommand *c = r->sort_out[i].cmd;
        switch (c->type)
        {
        case RENDER_CMD_SET_VIEWPORT:
        {
            D3D11_VIEWPORT vp = {0};
            vp.TopLeftX = c->viewport.x;
            vp.TopLeftY = c->viewport.y;
            vp.Width = c->viewport.w;
            vp.Height = c->viewport.h;
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &vp);
        }
        break;
        case RENDER_CMD_CLEAR:
            if (c->clear.clear_color && r->backbuffer_rtv)
                ctx->ClearRenderTargetView(r->backbuffer_rtv, c->clear.color);
            break;
        case RENDER_CMD_SET_PIPELINE:
        {
            u32 id = c->pipeline.pipeline;
            r->pipeline_ready = false;
            if (id && id <= r->pipeline_count)
            {
                D3D11Pipeline *p = &r->pipelines[id - 1];
                ID3D11VertexShader *vs = r->shaders[p->vertex_shader - 1].vs;
                ID3D11PixelShader *ps = p->pixel_shader ? r->shaders[p->pixel_shader - 1].ps : 0;
                r->pipeline_ready = vs && (ps || !p->pixel_shader) && (p->layout || !p->attribute_count);
                if (r->pipeline_ready)
                {
                    ctx->IASetInputLayout(p->layout);
                    ctx->IASetPrimitiveTopology(p->topology);
                    ctx->VSSetShader(vs, 0, 0);
                    ctx->PSSetShader(ps, 0, 0);
                    ctx->RSSetState(p->raster);
                    ctx->OMSetBlendState(p->blend, 0, 0xFFFFFFFF);
                }
            }
        }
        break;
        case RENDER_CMD_SET_VERTEX_BUFFER:
        {
            u32 id = c->vertex_buffer.buffer;
            if (id && id <= r->buffer_count)
            {
                UINT stride = c->vertex_buffer.stride, offset = c->vertex_buffer.offset;
                ctx->IASetVertexBuffers(c->vertex_buffer.slot, 1, &r->buffers[id - 1].buffer, &stride, &offset);
            }
        }
        break;
        case RENDER_CMD_SET_INDEX_BUFFER:
        {
            u32 id = c->index_buffer.buffer;
            if (id && id <= r->buffer_count)
                ctx->IASetIndexBuffer(r->buffers[id - 1].buffer,
                                      c->index_buffer.index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
                                      c->index_buffer.offset);
        }
        break;
        case RENDER_CMD_SET_CONSTANTS:
        {
            u32 slot = c->constants.slot;
            if (slot < D3D11_CONSTANT_SLOTS && r->constant_buffers[slot])
            {
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ctx->Map(r->constant_buffers[slot], 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
                {
                    memcpy(m.pData, c->constants.data, Min(c->constants.size, (u32)RENDER_INLINE_CONSTANTS_SIZE));
                    ctx->Unmap(r->constant_buffers[slot], 0);
                }
                ctx->VSSetConstantBuffers(slot, 1, &r->constant_buffers[slot]);
                ctx->PSSetConstantBuffers(slot, 1, &r->constant_buffers[slot]);
            }
        }
        break;
        case RENDER_CMD_UPDATE_BUFFER:
        {
            u32 id = c->update_buffer.buffer;
            if (id && id <= r->buffer_count && r->buffers[id - 1].buffer && c->update_buffer.size > 0)
            {
                const u8 *src = (const u8 *)list->upload.base + c->update_buffer.upload_offset;
                D3D11_BOX box;
                box.left = c->update_buffer.dst_offset;
                box.right = c->update_buffer.dst_offset + c->update_buffer.size;
                box.top = 0;
                box.bottom = 1;
                box.front = 0;
                box.back = 1;
                ctx->UpdateSubresource(r->buffers[id - 1].buffer, 0, &box, src, 0, 0);
            }
        }
        break;
        case RENDER_CMD_DRAW:
            if (r->pipeline_ready)
                ctx->Draw(c->draw.vertex_count, c->draw.first_vertex);
            break;
        case RENDER_CMD_DRAW_INSTANCED:
            if (r->pipeline_ready)
                ctx->DrawInstanced(c->draw_instanced.vertex_count, c->draw_instanced.instance_count,
                                   c->draw_instanced.first_vertex, c->draw_instanced.first_instance);
            break;
        case RENDER_CMD_DRAW_INDEXED:
            if (r->pipeline_ready)
                ctx->DrawIndexed(c->draw_indexed.index_count, c->draw_indexed.first_index, c->draw_indexed.base_vertex);
            break;
        case RENDER_CMD_SET_SCISSOR:
        {
            D3D11_RECT sr;
            sr.left = c->scissor.x;
            sr.top = c->scissor.y;
            sr.right = c->scissor.x + c->scissor.w;
            sr.bottom = c->scissor.y + c->scissor.h;
            ctx->RSSetScissorRects(1, &sr);
        }
        break;
        case RENDER_CMD_SET_TEXTURE:
        {
            u32 id = c->texture.texture;
            u32 slot = c->texture.slot;
            if (id && id <= r->texture_count && slot < D3D11_TEXTURE_SLOTS)
            {
                ctx->PSSetShaderResources(slot, 1, &r->textures[id - 1].srv);
                ctx->PSSetSamplers(slot, 1, &r->sampler_linear);
            }
        }
        break;
        }
    }
}

static void d3d11_present(Renderer *r)
{
    if (r->ok && r->swapchain)
        r->swapchain->Present(1, 0);
}

void renderer_d3d11_get_api(RendererAPI *out)
{
    out->create = d3d11_create;
    out->init = d3d11_init;
    out->shutdown = d3d11_shutdown;
    out->resize = d3d11_resize;
    out->create_buffer = d3d11_create_buffer;
    out->create_texture = d3d11_create_texture;
    out->create_shader = d3d11_create_shader;
    out->create_shader_hlsl = d3d11_create_shader_hlsl;
    out->reserve_shader = d3d11_reserve_shader;
    out->compile_shader_hlsl = d3d11_compile_shader_hlsl;
    out->create_pipeline = d3d11_create_pipeline;
    out->execute = d3d11_execute;
    out->present = d3d11_present;
}