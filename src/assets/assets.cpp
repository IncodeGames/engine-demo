#include "assets/assets.h"
#include "shared/assert.h"
#include <string.h>
#include <stdio.h>

// TODO(BPL): scan <root>/**/*.meta for {GUID, type, source, cooked-blob}
// records and populate the registry automatically. Move loads onto worker threads. 2026-06-02
ENGINE_API b32 assets_guid_equal(AssetGuid a, AssetGuid b) { return a.hi == b.hi && a.lo == b.lo; }
ENGINE_API AssetRegistry *assets_create(Arena *persistent, PlatformAPI *platform, const char *root)
{
    AssetRegistry *reg = PushStruct(persistent, AssetRegistry);
    reg->blob_arena = persistent; // blobs share the persistent arena for now
    reg->platform = platform;
    strncpy(reg->root, root, sizeof(reg->root) - 1);
    reg->capacity = 4096;
    reg->entries = PushArray(persistent, AssetEntry, reg->capacity);
    return reg;
}

ENGINE_API AssetEntry *assets_register(AssetRegistry *reg, AssetGuid guid, AssetType type, const char *blob_relative_path)
{
    ASSERT(reg->count < reg->capacity);
    AssetEntry *e = &reg->entries[reg->count++];
    memset(e, 0, sizeof(*e));
    e->guid = guid;
    e->type = type;
    e->state = ASSET_STATE_UNLOADED;
    snprintf(e->blob_path, sizeof(e->blob_path), "%s/%s", reg->root, blob_relative_path);
    return e;
}

ENGINE_API AssetEntry *assets_find(AssetRegistry *reg, AssetGuid guid)
{
    for (u32 i = 0; i < reg->count; ++i)
        if (assets_guid_equal(reg->entries[i].guid, guid))
            return &reg->entries[i];
    return 0;
}

ENGINE_API b32 assets_load_sync(AssetRegistry *reg, AssetGuid guid)
{
    AssetEntry *e = assets_find(reg, guid);
    if (!e)
        return false;
    PlatformFileContents f = reg->platform->file_read(e->blob_path, reg->blob_arena);
    if (!f.ok)
    {
        e->state = ASSET_STATE_FAILED;
        return false;
    }
    e->data = f.data;
    e->size = f.size;
    e->blob_modified_time = reg->platform->file_modified_time(e->blob_path);
    e->state = ASSET_STATE_LOADED;
    return true;
}

ENGINE_API void assets_poll_hot_reload(AssetRegistry *reg)
{
    for (u32 i = 0; i < reg->count; ++i)
    {
        AssetEntry *e = &reg->entries[i];
        if (e->state != ASSET_STATE_LOADED)
            continue;
        u64 t = reg->platform->file_modified_time(e->blob_path);
        if (t == 0 || t == e->blob_modified_time)
            continue;
        PlatformFileContents f = reg->platform->file_read(e->blob_path, reg->blob_arena);
        if (f.ok)
        {
            e->data = f.data;
            e->size = f.size;
            e->blob_modified_time = t;
        } // old blob leaks
    }
}