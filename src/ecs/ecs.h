#pragma once
#include "shared/types.h"
#include "memory/arena.h"

typedef u32 Entity;
typedef u8 ComponentId;
typedef u16 ArchetypeId;

#define ECS_MAX_COMPONENT_TYPES 255
#define ECS_MAX_ARCHETYPES 1024
#define ECS_MAX_COMPONENTS_PER_ARCHETYPE 64
#define ENTITY_INDEX_BITS 24u
#define ENTITY_INDEX_MASK ((1u << ENTITY_INDEX_BITS) - 1u)
#define ENTITY_INVALID ((Entity)0xFFFFFFFFu)
#define COMPONENT_INVALID ((ComponentId)0xFFu)
#define ARCHETYPE_INVALID ((ArchetypeId)0xFFFFu)

static FORCE_INLINE u32 entity_index(Entity e) { return e & ENTITY_INDEX_MASK; }
static FORCE_INLINE u32 entity_generation(Entity e) { return e >> ENTITY_INDEX_BITS; }
static FORCE_INLINE Entity entity_make(u32 idx, u32 gen)
{
    return (Entity)((gen << ENTITY_INDEX_BITS) | (idx & ENTITY_INDEX_MASK));
}

typedef struct ComponentMask
{
    u64 bits[4];
} ComponentMask;

static FORCE_INLINE void mask_set(ComponentMask *m, ComponentId id) { m->bits[id >> 6] |= (1ull << (id & 63)); }
static FORCE_INLINE void mask_clear(ComponentMask *m, ComponentId id) { m->bits[id >> 6] &= ~(1ull << (id & 63)); }
static FORCE_INLINE b32 mask_test(const ComponentMask *m, ComponentId id)
{
    return (b32)((m->bits[id >> 6] >> (id & 63)) & 1ull);
}

static FORCE_INLINE b32 mask_contains_all(const ComponentMask *m, const ComponentMask *req)
{
    for (int i = 0; i < 4; ++i)
        if ((m->bits[i] & req->bits[i]) != req->bits[i])
            return false;
    return true;
}

static FORCE_INLINE b32 mask_intersects(const ComponentMask *a, const ComponentMask *b)
{
    for (int i = 0; i < 4; ++i)
        if (a->bits[i] & b->bits[i])
            return true;
    return false;
}

static FORCE_INLINE b32 mask_equal(const ComponentMask *a, const ComponentMask *b)
{
    for (int i = 0; i < 4; ++i)
        if (a->bits[i] != b->bits[i])
            return false;
    return true;
}

typedef struct ComponentType
{
    const char *name;
    u32 size;
    u32 align;
} ComponentType;

typedef struct Archetype
{
    ComponentMask signature;
    ComponentId components[ECS_MAX_COMPONENTS_PER_ARCHETYPE];
    void *columns[ECS_MAX_COMPONENTS_PER_ARCHETYPE]; // parallel SoA arrays
    i16 column_of[ECS_MAX_COMPONENT_TYPES];          // component id -> column index, or -1
    u32 component_count;
    Entity *entities; // row -> entity
    u32 count;
    u32 capacity;
} Archetype;

typedef struct EntitySlot
{
    ArchetypeId archetype;
    u32 row;
    u8 generation;
    b32 alive;
} EntitySlot;

typedef struct World
{
    Arena *arena;
    ComponentType component_types[ECS_MAX_COMPONENT_TYPES];
    u32 component_type_count;
    Archetype archetypes[ECS_MAX_ARCHETYPES];
    u32 archetype_count;
    EntitySlot *slots;
    u32 *free_indices;
    u32 free_count;
    u32 next_index;
    u32 max_entities;
} World;

typedef struct EcsQueryResult
{
    World *world;
    ArchetypeId archetypes[ECS_MAX_ARCHETYPES];
    u32 count;
} EcsQueryResult;

// lifecycle
ENGINE_API World *ecs_world_create(Arena *arena, u32 max_entities);

// registration (startup only -- archetypes are pre-declared)
ENGINE_API ComponentId ecs_register_component(World *w, const char *name, u32 size, u32 align);
ENGINE_API ArchetypeId ecs_register_archetype(World *w, const ComponentId *ids, u32 id_count, u32 capacity);
ENGINE_API ArchetypeId ecs_find_archetype(World *w, const ComponentMask *signature);

// entities
ENGINE_API Entity ecs_entity_create(World *w, ArchetypeId archetype);
ENGINE_API u32 ecs_entity_create_bulk(World *w, ArchetypeId archetype, u32 count);
ENGINE_API void ecs_entity_destroy(World *w, Entity e);
ENGINE_API b32 ecs_entity_alive(World *w, Entity e);
ENGINE_API ArchetypeId ecs_entity_archetype(World *w, Entity e);

// components
ENGINE_API void *ecs_get(World *w, Entity e, ComponentId c);
ENGINE_API b32 ecs_has(World *w, Entity e, ComponentId c);
ENGINE_API void *ecs_add(World *w, Entity e, ComponentId c);
ENGINE_API void ecs_remove(World *w, Entity e, ComponentId c);

// iteration
ENGINE_API void ecs_query(World *w, const ComponentId *include, u32 include_count,
                          const ComponentId *exclude, u32 exclude_count, EcsQueryResult *out);
ENGINE_API void *ecs_column(World *w, ArchetypeId archetype, ComponentId c);
ENGINE_API u32 ecs_archetype_count(World *w, ArchetypeId archetype);
ENGINE_API Entity *ecs_archetype_entities(World *w, ArchetypeId archetype);