#pragma once
#include "shared/types.h"
struct EngineContext;
struct ThreadContext;
struct RenderList;
#define GAME_API_VERSION 2

// Implemented by the game DLL. For hotreload to work
// the game ***MUST NOT STORE ANYTHING IN GLOBAL MEMORY***

typedef struct GameAPI
{
    u32 version;         // must equal GAME_API_VERSION
    u64 engine_abi_hash; // engine_abi_hash() as compiled into the game; mismatch => host refuses the module
    u64 schema_hash;     // component/state layout hash; mismatch => host performs a full reset
    void *(*init)(struct EngineContext *engine);
    void (*reload)(struct EngineContext *engine, void *game_state);
    void (*update)(struct EngineContext *engine, struct ThreadContext *thread, void *game_state);
    void (*build_render_list)(struct EngineContext *engine, struct ThreadContext *thread, void *game_state, struct RenderList *out);
    void (*shutdown)(struct EngineContext *engine, void *game_state);
} GameAPI;

typedef void (*GetGameAPIFn)(GameAPI *out); // The game DLL exports:  GAME_API void GetGameAPI(GameAPI* out);