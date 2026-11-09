#include "engine/engine.h"
#include <string.h>

void input_init(InputState *in, Arena *persistent)
{
    memset(in, 0, sizeof(*in));
    in->events = PushArray(persistent, PlatformInputEvent, INPUT_MAX_EVENTS_PER_FRAME);
}

void input_begin_frame(InputState *in, PlatformAPI *p)
{
    memset(in->key_pressed, 0, sizeof(in->key_pressed));
    memset(in->key_released, 0, sizeof(in->key_released));
    memset(in->mouse_pressed, 0, sizeof(in->mouse_pressed));
    memset(in->mouse_released, 0, sizeof(in->mouse_released));
    in->text_count = 0;
    in->mouse_dx = 0;
    in->mouse_dy = 0;
    in->mouse_wheel = 0.0f;

    in->events_count = p->input_poll_events(in->events, INPUT_MAX_EVENTS_PER_FRAME);
    for (u32 i = 0; i < in->events_count; ++i)
    {
        PlatformInputEvent *ev = &in->events[i];
        switch (ev->type)
        {
        case PLATFORM_INPUT_KEY:
            if (ev->key.key < 256)
            {
                u32 k = ev->key.key;
                if (ev->key.down)
                {
                    if (!in->key_down[k])
                        in->key_pressed[k] = 1;
                    in->key_down[k] = 1;
                }
                else
                {
                    if (in->key_down[k])
                        in->key_released[k] = 1;
                    in->key_down[k] = 0;
                }
            }
            break;
        case PLATFORM_INPUT_MOUSE_BUTTON:
            if (ev->mouse_button.button < PLATFORM_MOUSE_BUTTON_COUNT)
            {
                u32 b = ev->mouse_button.button;
                if (ev->mouse_button.down)
                {
                    if (!in->mouse_down[b])
                        in->mouse_pressed[b] = 1;
                    in->mouse_down[b] = 1;
                }
                else
                {
                    if (in->mouse_down[b])
                        in->mouse_released[b] = 1;
                    in->mouse_down[b] = 0;
                }
            }
            break;
        case PLATFORM_INPUT_MOUSE_MOVE:
            in->mouse_x = ev->mouse_move.x;
            in->mouse_y = ev->mouse_move.y;
            in->mouse_dx += ev->mouse_move.dx;
            in->mouse_dy += ev->mouse_move.dy;
            break;
        case PLATFORM_INPUT_MOUSE_WHEEL:
            in->mouse_wheel += ev->wheel.delta;
            break;
        case PLATFORM_INPUT_TEXT:
            if (in->text_count < INPUT_MAX_TEXT_PER_FRAME)
                in->text[in->text_count++] = ev->text.codepoint;
            break;
        }
    }
}

ENGINE_API b32 input_key_down(EngineContext *e, u32 key) { return key < 256 ? e->input.key_down[key] : 0; }
ENGINE_API b32 input_key_pressed(EngineContext *e, u32 key) { return key < 256 ? e->input.key_pressed[key] : 0; }
ENGINE_API b32 input_key_released(EngineContext *e, u32 key) { return key < 256 ? e->input.key_released[key] : 0; }
ENGINE_API b32 input_mouse_down(EngineContext *e, PlatformMouseButton b) { return b < PLATFORM_MOUSE_BUTTON_COUNT ? e->input.mouse_down[b] : 0; }
ENGINE_API b32 input_mouse_pressed(EngineContext *e, PlatformMouseButton b) { return b < PLATFORM_MOUSE_BUTTON_COUNT ? e->input.mouse_pressed[b] : 0; }
ENGINE_API b32 input_mouse_released(EngineContext *e, PlatformMouseButton b) { return b < PLATFORM_MOUSE_BUTTON_COUNT ? e->input.mouse_released[b] : 0; }