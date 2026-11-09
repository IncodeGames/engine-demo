#include "audio/audio.h"

// TODO(BPL): Replace stub with FMOD backend.

static Audio *audio_create(Arena *persistent)
{
    Audio *a = PushStruct(persistent, Audio);
    a->next_id = 1;
    return a;
}

static b32 audio_init(Audio *a)
{
    (void)a;
    return true;
}

static void audio_update(Audio *a) { (void)a; }
static void audio_shutdown(Audio *a) { (void)a; }

static SoundHandle audio_load_sound(Audio *a, const char *path)
{
    (void)path;
    SoundHandle h = {a->next_id++};
    return h;
}

static VoiceHandle audio_play(Audio *a, SoundHandle s, f32 v)
{
    (void)s;
    (void)v;
    VoiceHandle h = {a->next_id++};
    return h;
}

static void audio_stop(Audio *a, VoiceHandle v)
{
    (void)a;
    (void)v;
}

void audio_stub_get_api(AudioAPI *out)
{
    out->create = audio_create;
    out->init = audio_init;
    out->shutdown = audio_shutdown;
    out->update = audio_update;
    out->load_sound = audio_load_sound;
    out->play = audio_play;
    out->stop = audio_stop;
}