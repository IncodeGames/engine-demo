#pragma once
#include "shared/types.h"
#include "memory/arena.h"
#include "platform/platform.h"

struct EngineContext;

#define INPUT_MAX_EVENTS_PER_FRAME 4096
#define INPUT_MAX_TEXT_PER_FRAME 64

typedef struct InputState
{
    u8 key_down[256];
    u8 key_pressed[256];  // down this frame
    u8 key_released[256]; // up this frame
    u8 mouse_down[PLATFORM_MOUSE_BUTTON_COUNT];
    u8 mouse_pressed[PLATFORM_MOUSE_BUTTON_COUNT];
    u8 mouse_released[PLATFORM_MOUSE_BUTTON_COUNT];
    i32 mouse_x, mouse_y, mouse_dx, mouse_dy;
    f32 mouse_wheel;

    u32 text[INPUT_MAX_TEXT_PER_FRAME];
    u32 text_count;

    PlatformInputEvent *events;
    u32 events_count;
} InputState;

void input_init(InputState *in, Arena *persistent);
void input_begin_frame(InputState *in, PlatformAPI *platform); // thread 0 only

ENGINE_API b32 input_key_down(struct EngineContext *e, u32 key);
ENGINE_API b32 input_key_pressed(struct EngineContext *e, u32 key);
ENGINE_API b32 input_key_released(struct EngineContext *e, u32 key);
ENGINE_API b32 input_mouse_down(struct EngineContext *e, PlatformMouseButton button);
ENGINE_API b32 input_mouse_pressed(struct EngineContext *e, PlatformMouseButton button);
ENGINE_API b32 input_mouse_released(struct EngineContext *e, PlatformMouseButton button);