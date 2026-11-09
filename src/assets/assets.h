#pragma once
#include "shared/types.h"
#include "memory/arena.h"
#include "platform/platform.h"

typedef struct AssetGuid
{
    u64 hi;
    u64 lo;
} AssetGuid;

typedef enum AssetType
{
    ASSET_UNKNOWN,
    ASSET_TEXTURE,
    ASSET_MESH,
    ASSET_SHADER,
    ASSET_AUDIO_CLIP
} AssetType;

typedef enum AssetState
{
    ASSET_STATE_UNLOADED,
    ASSET_STATE_LOADED,
    ASSET_STATE_FAILED
} AssetState;

typedef struct AssetEntry
{
    AssetGuid guid;
    AssetType type;
    AssetState state;
    char blob_path[260];
    u64 blob_modified_time;
    void *data;
    usize size;
} AssetEntry;

typedef struct AssetRegistry
{
    Arena *blob_arena;
    PlatformAPI *platform;
    char root[240];
    AssetEntry *entries;
    u32 count;
    u32 capacity;
} AssetRegistry;

ENGINE_API b32 assets_guid_equal(AssetGuid a, AssetGuid b);
ENGINE_API AssetRegistry *assets_create(Arena *persistent, PlatformAPI *platform, const char *root);
ENGINE_API AssetEntry *assets_register(AssetRegistry *reg, AssetGuid guid, AssetType type, const char *blob_relative_path);
ENGINE_API AssetEntry *assets_find(AssetRegistry *reg, AssetGuid guid);
ENGINE_API b32 assets_load_sync(AssetRegistry *reg, AssetGuid guid);
ENGINE_API void assets_poll_hot_reload(AssetRegistry *reg);