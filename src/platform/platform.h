#pragma once
#include "shared/types.h"
#include "memory/arena.h"

typedef struct PlatformProcess
{
    void *process;
    void *output;
} PlatformProcess;

typedef struct PlatformThread
{
    void *handle;
} PlatformThread;

typedef struct PlatformSemaphore
{
    void *handle;
} PlatformSemaphore;

typedef struct PlatformBarrier
{
    alignas(16) u8 opaque[64];
} PlatformBarrier;

typedef void (*PlatformThreadFunc)(void *user);

typedef enum PlatformMouseButton
{
    PLATFORM_MOUSE_LEFT,
    PLATFORM_MOUSE_RIGHT,
    PLATFORM_MOUSE_MIDDLE,
    PLATFORM_MOUSE_BUTTON_COUNT
} PlatformMouseButton;

// Indexed by platform virtual-key code. On Windows these match VK_*.
enum
{
    KEY_BACKSPACE = 0x08,
    KEY_TAB = 0x09,
    KEY_RETURN = 0x0D,
    KEY_SHIFT = 0x10,
    KEY_CONTROL = 0x11,
    KEY_ALT = 0x12,
    KEY_ESCAPE = 0x1B,
    KEY_SPACE = 0x20,
    KEY_LEFT = 0x25,
    KEY_UP = 0x26,
    KEY_RIGHT = 0x27,
    KEY_DOWN = 0x28,
    KEY_0 = 0x30,
    KEY_1,
    KEY_2,
    KEY_3,
    KEY_4,
    KEY_5,
    KEY_6,
    KEY_7,
    KEY_8,
    KEY_9,
    KEY_A = 0x41,
    KEY_B,
    KEY_C,
    KEY_D,
    KEY_E,
    KEY_F,
    KEY_G,
    KEY_H,
    KEY_I,
    KEY_J,
    KEY_K,
    KEY_L,
    KEY_M,
    KEY_N,
    KEY_O,
    KEY_P,
    KEY_Q,
    KEY_R,
    KEY_S,
    KEY_T,
    KEY_U,
    KEY_V,
    KEY_W,
    KEY_X,
    KEY_Y,
    KEY_Z,
    KEY_F1 = 0x70,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12
};

typedef struct PlatformInput
{
    u8 keys[256];
    u8 mouse[PLATFORM_MOUSE_BUTTON_COUNT];
    i32 mouse_x, mouse_y;
    i32 mouse_dx, mouse_dy;
    f32 mouse_wheel;
} PlatformInput;

typedef enum PlatformInputEventType
{
    PLATFORM_INPUT_KEY,
    PLATFORM_INPUT_MOUSE_BUTTON,
    PLATFORM_INPUT_MOUSE_MOVE,
    PLATFORM_INPUT_MOUSE_WHEEL,
    PLATFORM_INPUT_TEXT
} PlatformInputEventType;

typedef struct PlatformInputEvent
{
    u32 type;
    union
    {
        struct
        {
            u32 key;
            b32 down;
        } key;
        struct
        {
            u32 button;
            b32 down;
        } mouse_button;
        struct
        {
            i32 x, y, dx, dy;
        } mouse_move;
        struct
        {
            f32 delta;
        } wheel;
        struct
        {
            u32 codepoint;
        } text; // UTF-16 code unit on Windows
        //(TODO(BPL): full codepoints) - 2026-04-29
    };
} PlatformInputEvent;

typedef struct PlatformFileContents
{
    void *data;
    usize size;
    b32 ok;
} PlatformFileContents;

typedef void (*PlatformDirVisitFn)(const char *path, u64 modified_time, void *user);

typedef struct PlatformAPI
{
    b32 (*process_start)(PlatformProcess *out, const char *command_line, const char *working_dir);
    u32 (*process_read)(PlatformProcess *proc, char *buffer, u32 capacity);
    b32 (*process_exited)(PlatformProcess *proc, i32 *out_exit_code);
    void (*process_release)(PlatformProcess *proc);

    // virtual memory
    void *(*mem_reserve)(usize size);
    b32 (*mem_commit)(void *ptr, usize size);
    void (*mem_release)(void *ptr, usize size);

    // file i/o
    PlatformFileContents (*file_read)(const char *path, Arena *arena);
    b32 (*file_write)(const char *path, const void *data, usize size);
    b32 (*file_exists)(const char *path);
    u64 (*file_modified_time)(const char *path);
    b32 (*file_copy)(const char *src, const char *dst, b32 overwrite);

    void (*dir_visit_files)(const char *dir, PlatformDirVisitFn visit, void *user);

    // dynamic libraries
    void *(*library_load)(const char *path);
    void (*library_unload)(void *handle);
    void *(*library_symbol)(void *handle, const char *name);

    // threads & synchronization
    u32 (*logical_core_count)(void);
    PlatformThread (*thread_start)(PlatformThreadFunc fn, void *user);
    void (*thread_join)(PlatformThread thread);
    void (*barrier_init)(PlatformBarrier *barrier, u32 thread_count);
    void (*barrier_wait)(PlatformBarrier *barrier);
    void (*barrier_destroy)(PlatformBarrier *barrier);
    PlatformSemaphore (*semaphore_create)(u32 initial, u32 max);
    void (*semaphore_wait)(PlatformSemaphore sem);
    b32 (*semaphore_wait_timeout)(PlatformSemaphore sem, u32 timeout_ms);
    b32 (*semaphore_signal)(PlatformSemaphore sem, u32 count);
    void (*semaphore_destroy)(PlatformSemaphore sem);

    // timing
    u64 (*time_ticks)(void);
    u64 (*time_frequency)(void);
    void (*sleep_ms)(u32 ms);

    // window & input (THREAD 0 ONLY)
    b32 (*window_open)(const char *title, u32 width, u32 height);
    b32 (*window_pump)(void);
    void (*window_size)(u32 *out_w, u32 *out_h);
    void *(*window_handle)(void);
    void (*input_snapshot)(PlatformInput *out);
    u32 (*input_poll_events)(PlatformInputEvent *out_events, u32 max_events); // drains the queue

    // diagnostics
    void (*log)(const char *message);
    void (*debug_break)(void);
} PlatformAPI;