#include "ecs/ecs.h"
#include "shared/assert.h"
#include <string.h>

ENGINE_API World *ecs_world_create(Arena *arena, u32 max_entities)
{
    ASSERT(max_entities <= ENTITY_INDEX_MASK &&
           "max_entities exceeds the 24-bit entity index space");
    World *w = PushStruct(arena, World);
    w->arena = arena;
    w->max_entities = max_entities;
    w->slots = PushArray(arena, EntitySlot, max_entities);
    w->free_indices = PushArray(arena, u32, max_entities);
    w->next_index = 1; // NOTE(BPL): index 0 reserved so entity 0 is never a valid handle
    ecs_register_archetype(w, 0, 0, 1024);
    return w;
}

ENGINE_API ComponentId ecs_register_component(World *w, const char *name, u32 size, u32 align)
{
    ASSERT(w->component_type_count < ECS_MAX_COMPONENT_TYPES);
    ComponentId id = (ComponentId)w->component_type_count++;
    w->component_types[id].name = name;
    w->component_types[id].size = size;
    w->component_types[id].align = align ? align : 16;
    return id;
}

ENGINE_API ArchetypeId ecs_register_archetype(World *w, const ComponentId *ids, u32 id_count, u32 capacity)
{
    ASSERT(w->archetype_count < ECS_MAX_ARCHETYPES);
    ASSERT(id_count <= ECS_MAX_COMPONENTS_PER_ARCHETYPE);
    ArchetypeId aid = (ArchetypeId)w->archetype_count++;
    Archetype *a = &w->archetypes[aid];
    memset(a, 0, sizeof(*a));
    for (u32 i = 0; i < ECS_MAX_COMPONENT_TYPES; ++i)
    {
        a->column_of[i] = -1;
    }
    a->component_count = id_count;
    a->capacity = capacity;
    a->entities = PushArray(w->arena, Entity, capacity);
    for (u32 i = 0; i < id_count; ++i)
    {
        ComponentId c = ids[i];
        ASSERT(c < w->component_type_count);
        mask_set(&a->signature, c);
        a->components[i] = c;
        a->column_of[c] = (i16)i;
        a->columns[i] = arena_push_zero(w->arena,
                                        (usize)w->component_types[c].size * capacity,
                                        w->component_types[c].align);
    }
    return aid;
}

ENGINE_API ArchetypeId ecs_find_archetype(World *w, const ComponentMask *sig)
{
    for (u32 i = 0; i < w->archetype_count; ++i)
    {
        if (mask_equal(&w->archetypes[i].signature, sig))
        {
            return (ArchetypeId)i;
        }
    }
    return ARCHETYPE_INVALID;
}

ENGINE_API Entity ecs_entity_create(World *w, ArchetypeId archetype)
{
    ASSERT(archetype < w->archetype_count);
    u32 idx;
    if (w->free_count > 0)
    {
        idx = w->free_indices[--w->free_count];
    }
    else
    {
        ASSERT(w->next_index < w->max_entities);
        idx = w->next_index++;
    }
    EntitySlot *slot = &w->slots[idx];
    slot->alive = true;
    slot->archetype = archetype;
    Archetype *a = &w->archetypes[archetype];
    ASSERT(a->count < a->capacity);
    u32 row = a->count++;
    slot->row = row;
    Entity e = entity_make(idx, slot->generation);
    a->entities[row] = e;
    for (u32 i = 0; i < a->component_count; ++i)
    {
        u32 sz = w->component_types[a->components[i]].size;
        memset((u8 *)a->columns[i] + (usize)row * sz, 0, sz);
    }
    return e;
}

ENGINE_API u32 ecs_entity_create_bulk(World *w, ArchetypeId archetype, u32 count)
{
    ASSERT(archetype < w->archetype_count);
    Archetype *a = &w->archetypes[archetype];
    ASSERT(a->count + count <= a->capacity);
    u32 first_row = a->count;
    for (u32 i = 0; i < count; ++i)
    {
        u32 idx;
        if (w->free_count > 0)
            idx = w->free_indices[--w->free_count];
        else
        {
            ASSERT(w->next_index < w->max_entities);
            idx = w->next_index++;
        }
        EntitySlot *slot = &w->slots[idx];
        slot->alive = true;
        slot->archetype = archetype;
        slot->row = first_row + i;
        a->entities[first_row + i] = entity_make(idx, slot->generation);
    }
    for (u32 c = 0; c < a->component_count; ++c)
    {
        u32 sz = w->component_types[a->components[c]].size;
        memset((u8 *)a->columns[c] + (usize)first_row * sz, 0, (usize)count * sz);
    }
    a->count += count;
    return first_row;
}

static void ecs_swap_remove_row(World *w, Archetype *a, u32 row)
{
    u32 last = a->count - 1;
    if (row != last)
    {
        for (u32 i = 0; i < a->component_count; ++i)
        {
            u32 sz = w->component_types[a->components[i]].size;
            memcpy((u8 *)a->columns[i] + (usize)row * sz,
                   (u8 *)a->columns[i] + (usize)last * sz, sz);
        }
        Entity moved = a->entities[last];
        a->entities[row] = moved;
        w->slots[entity_index(moved)].row = row;
    }
    a->count--;
}

ENGINE_API b32 ecs_entity_alive(World *w, Entity e)
{
    if (e == ENTITY_INVALID)
    {
        return false;
    }
    u32 idx = entity_index(e);
    if (idx == 0 || idx >= w->max_entities)
    {
        return false;
    }
    EntitySlot *s = &w->slots[idx];
    return s->alive && s->generation == (u8)entity_generation(e);
}

ENGINE_API ArchetypeId ecs_entity_archetype(World *w, Entity e)
{
    if (!ecs_entity_alive(w, e))
    {
        return ARCHETYPE_INVALID;
    }
    return w->slots[entity_index(e)].archetype;
}

ENGINE_API void ecs_entity_destroy(World *w, Entity e)
{
    if (!ecs_entity_alive(w, e))
    {
        return;
    }
    u32 idx = entity_index(e);
    EntitySlot *s = &w->slots[idx];
    ecs_swap_remove_row(w, &w->archetypes[s->archetype], s->row);
    s->alive = false;
    s->generation++;
    w->free_indices[w->free_count++] = idx;
}

ENGINE_API void *ecs_get(World *w, Entity e, ComponentId c)
{
    if (!ecs_entity_alive(w, e))
    {
        return 0;
    }
    EntitySlot *s = &w->slots[entity_index(e)];
    Archetype *a = &w->archetypes[s->archetype];
    i16 col = a->column_of[c];
    if (col < 0)
    {
        return 0;
    }
    u32 sz = w->component_types[c].size;
    return (u8 *)a->columns[col] + (usize)s->row * sz;
}

ENGINE_API b32 ecs_has(World *w, Entity e, ComponentId c)
{
    if (!ecs_entity_alive(w, e))
    {
        return false;
    }
    return mask_test(&w->archetypes[w->slots[entity_index(e)].archetype].signature, c);
}

static void ecs_move_entity(World *w, Entity e, ArchetypeId dst_id)
{
    EntitySlot *s = &w->slots[entity_index(e)];
    ArchetypeId src_id = s->archetype;
    if (src_id == dst_id)
    {
        return;
    }

    Archetype *src = &w->archetypes[src_id];
    Archetype *dst = &w->archetypes[dst_id];
    ASSERT(dst->count < dst->capacity);
    u32 src_row = s->row;
    u32 dst_row = dst->count++;
    for (u32 i = 0; i < dst->component_count; ++i)
    {
        ComponentId c = dst->components[i];
        u32 sz = w->component_types[c].size;
        i16 src_col = src->column_of[c];
        u8 *dst_ptr = (u8 *)dst->columns[i] + (usize)dst_row * sz;
        if (src_col >= 0)
        {
            memcpy(dst_ptr, (u8 *)src->columns[src_col] + (usize)src_row * sz, sz);
        }
        else
        {
            memset(dst_ptr, 0, sz);
        }
    }
    dst->entities[dst_row] = e;
    s->archetype = dst_id;
    s->row = dst_row;
    ecs_swap_remove_row(w, src, src_row);
}

ENGINE_API void *ecs_add(World *w, Entity e, ComponentId c)
{
    ASSERT(ecs_entity_alive(w, e));
    EntitySlot *s = &w->slots[entity_index(e)];
    Archetype *src = &w->archetypes[s->archetype];
    if (mask_test(&src->signature, c))
    {
        return ecs_get(w, e, c);
    }
    ComponentMask target = src->signature;
    mask_set(&target, c);
    ArchetypeId dst = ecs_find_archetype(w, &target);
    ASSERT(dst != ARCHETYPE_INVALID && "destination archetype was not pre-declared at startup");
    ecs_move_entity(w, e, dst);
    return ecs_get(w, e, c);
}

ENGINE_API void ecs_remove(World *w, Entity e, ComponentId c)
{
    ASSERT(ecs_entity_alive(w, e));
    EntitySlot *s = &w->slots[entity_index(e)];
    Archetype *src = &w->archetypes[s->archetype];
    if (!mask_test(&src->signature, c))
    {
        return;
    }
    ComponentMask target = src->signature;
    mask_clear(&target, c);
    ArchetypeId dst = ecs_find_archetype(w, &target);
    ASSERT(dst != ARCHETYPE_INVALID && "destination archetype was not pre-declared at startup");
    ecs_move_entity(w, e, dst);
}

ENGINE_API void ecs_query(World *w, const ComponentId *inc, u32 inc_n,
                          const ComponentId *exc, u32 exc_n, EcsQueryResult *out)
{
    ComponentMask include = {0}, exclude = {0};
    for (u32 i = 0; i < inc_n; ++i)
    {
        mask_set(&include, inc[i]);
    }

    for (u32 i = 0; i < exc_n; ++i)
    {
        mask_set(&exclude, exc[i]);
    }
    out->world = w;
    out->count = 0;
    for (u32 i = 0; i < w->archetype_count; ++i)
    {
        Archetype *a = &w->archetypes[i];
        if (a->count == 0)
        {
            continue;
        }
        if (!mask_contains_all(&a->signature, &include))
        {
            continue;
        }
        if (exc_n && mask_intersects(&a->signature, &exclude))
        {
            continue;
        }
        out->archetypes[out->count++] = (ArchetypeId)i;
    }
}

ENGINE_API void *ecs_column(World *w, ArchetypeId archetype, ComponentId c)
{
    Archetype *a = &w->archetypes[archetype];
    i16 col = a->column_of[c];
    return (col < 0) ? 0 : a->columns[col];
}

ENGINE_API u32 ecs_archetype_count(World *w, ArchetypeId archetype) { return w->archetypes[archetype].count; }
ENGINE_API Entity *ecs_archetype_entities(World *w, ArchetypeId archetype) { return w->archetypes[archetype].entities; }