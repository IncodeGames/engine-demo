#pragma once
#include "shared/types.h"
#include "memory/arena.h"

typedef struct Physics Physics;
typedef struct BodyHandle
{
    u32 id;
} BodyHandle;

typedef enum BodyType
{
    BODY_STATIC,
    BODY_DYNAMIC,
    BODY_KINEMATIC
} BodyType;

typedef struct BodyDesc
{
    BodyType type;
    f32 position[3];
    f32 half_extents[3]; // box placeholder
    f32 mass;
} BodyDesc;

typedef struct PhysicsAPI
{
    Physics *(*create)(Arena *persistent);
    b32 (*init)(Physics *p);
    void (*shutdown)(Physics *p);
    void (*step)(Physics *p, f32 dt);
    BodyHandle (*create_body)(Physics *p, const BodyDesc *desc);
    void (*destroy_body)(Physics *p, BodyHandle body);
    void (*get_body_transform)(Physics *p, BodyHandle body, f32 out_position[3], f32 out_rotation[4]);
} PhysicsAPI;

void physics_stub_get_api(PhysicsAPI *out);