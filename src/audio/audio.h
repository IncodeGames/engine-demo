#pragma once
#include "shared/types.h"
#include "memory/arena.h"

typedef struct Audio
{
    u32 next_id;
} Audio;

typedef struct SoundHandle
{
    u32 id;
} SoundHandle;

typedef struct VoiceHandle
{
    u32 id;
} VoiceHandle;

typedef struct AudioAPI
{
    Audio *(*create)(Arena *persistent);
    b32 (*init)(Audio *a);
    void (*shutdown)(Audio *a);
    void (*update)(Audio *a);
    SoundHandle (*load_sound)(Audio *a, const char *path);
    VoiceHandle (*play)(Audio *a, SoundHandle sound, f32 volume);
    void (*stop)(Audio *a, VoiceHandle voice);
} AudioAPI;

void audio_stub_get_api(AudioAPI *out);