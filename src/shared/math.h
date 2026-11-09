#pragma once
#include "shared/types.h"

typedef union v2
{
    struct
    {
        f32 x, y;
    };
    f32 e[2];
} v2;

typedef union v3
{
    struct
    {
        f32 x, y, z;
    };
    f32 e[3];
} v3;

typedef union v4
{
    struct
    {
        f32 x, y, z, w;
    };
    f32 e[4];
} v4;

typedef struct mat4
{
    f32 m[16];
} mat4; // column-major

static FORCE_INLINE v3 v3_add(v3 a, v3 b)
{
    v3 r = {a.x + b.x, a.y + b.y, a.z + b.z};
    return r;
}

static FORCE_INLINE v3 v3_scale(v3 a, f32 s)
{
    v3 r = {a.x * s, a.y * s, a.z * s};
    return r;
}

static FORCE_INLINE mat4 mat4_identity(void)
{
    mat4 r = {0};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

static FORCE_INLINE mat4 mat4_mul(mat4 a, mat4 b)
{
    mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            f32 s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}
