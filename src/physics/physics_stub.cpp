#include "physics/physics.h"

// TODO(BPL) Replace stub with Jolt integration
struct Physics
{
    u32 next_id;
};

static Physics *physics_create(Arena *persistent)
{
    Physics *p = PushStruct(persistent, Physics);
    p->next_id = 1;
    return p;
}

static b32 physics_init(Physics *p)
{
    (void)p;
    return true;
}

static void physics_shutdown(Physics *p) { (void)p; }
static void physics_step(Physics *p, f32 dt)
{
    (void)p;
    (void)dt;
}

static BodyHandle physics_create_body(Physics *p, const BodyDesc *d)
{
    (void)d;
    BodyHandle h = {p->next_id++};
    return h;
}

static void physics_destroy_body(Physics *p, BodyHandle b)
{
    (void)p;
    (void)b;
}

static void physics_get_body_transform(Physics *p, BodyHandle b, f32 pos[3], f32 rot[4])
{
    (void)p;
    (void)b;
    pos[0] = pos[1] = pos[2] = 0.0f;
    rot[0] = rot[1] = rot[2] = 0.0f;
    rot[3] = 1.0f;
}

void physics_stub_get_api(PhysicsAPI *out)
{
    out->create = physics_create;
    out->init = physics_init;
    out->shutdown = physics_shutdown;
    out->step = physics_step;
    out->create_body = physics_create_body;
    out->destroy_body = physics_destroy_body;
    out->get_body_transform = physics_get_body_transform;
}
