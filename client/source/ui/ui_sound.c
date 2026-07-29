#include "ui_sound.h"

#include <SDL2/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 48000
#define CHANNELS 2

typedef struct {
    int16_t *samples; // interleaved stereo
    size_t count;     // total int16 values (frames * CHANNELS)
} Tone;

static SDL_AudioDeviceID s_device = 0;
static bool s_enabled = true;
static Tone s_tones[UI_SOUND_COUNT];

// A sine sweep from start_hz to end_hz over `ms`, with a fast attack and a
// squared decay so it reads as a soft "blip" rather than a click - a raw
// sine cut off abruptly pops audibly at both ends.
static Tone make_tone(float start_hz, float end_hz, int ms, float volume) {
    Tone tone = { NULL, 0 };

    int frames = SAMPLE_RATE * ms / 1000;
    if (frames <= 1) return tone;

    tone.count = (size_t)frames * CHANNELS;
    tone.samples = (int16_t *)malloc(tone.count * sizeof(int16_t));
    if (!tone.samples) {
        tone.count = 0;
        return tone;
    }

    double phase = 0.0;
    for (int frame = 0; frame < frames; frame++) {
        float progress = (float)frame / (float)(frames - 1);
        float frequency = start_hz + (end_hz - start_hz) * progress;
        phase += 6.283185307179586 * frequency / SAMPLE_RATE;

        float attack = progress * 10.0f;
        if (attack > 1.0f) attack = 1.0f;
        float release = 1.0f - progress;
        float envelope = attack * release * release;

        int16_t value = (int16_t)(sin(phase) * envelope * volume * 32767.0f);
        tone.samples[(size_t)frame * CHANNELS] = value;
        tone.samples[(size_t)frame * CHANNELS + 1] = value;
    }
    return tone;
}

void ui_sound_init(void) {
    if (s_device) return;

    // SDL_INIT_VIDEO is already up (see ui_app_init) - this only adds the
    // audio subsystem, and failing it is not fatal to anything.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;

    SDL_AudioSpec want;
    memset(&want, 0, sizeof(want));
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = CHANNELS;
    want.samples = 512;

    s_device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!s_device) return;

    // Rising = forward/accept, falling = backward/dismiss, and a short high
    // tick for movement, so the three are distinguishable without being
    // intrusive at the volume they play back at.
    s_tones[UI_SOUND_NAVIGATE] = make_tone(920.0f, 1040.0f, 18, 0.10f);
    s_tones[UI_SOUND_CONFIRM] = make_tone(620.0f, 980.0f, 42, 0.16f);
    s_tones[UI_SOUND_BACK] = make_tone(760.0f, 420.0f, 48, 0.14f);

    SDL_PauseAudioDevice(s_device, 0);
}

void ui_sound_shutdown(void) {
    if (s_device) {
        SDL_ClearQueuedAudio(s_device);
        SDL_CloseAudioDevice(s_device);
        s_device = 0;
    }
    for (int i = 0; i < UI_SOUND_COUNT; i++) {
        free(s_tones[i].samples);
        s_tones[i].samples = NULL;
        s_tones[i].count = 0;
    }
}

void ui_sound_play(UiSound sound) {
    if (!s_device || !s_enabled) return;
    if (sound < 0 || sound >= UI_SOUND_COUNT) return;

    const Tone *tone = &s_tones[sound];
    if (!tone->samples) return;

    // Held D-Pad auto-repeat can queue tones faster than they play back,
    // which would drift further and further behind the actual input.
    // Dropping the backlog once it exceeds ~half a second keeps the sound
    // tied to what the user is doing right now.
    if (SDL_GetQueuedAudioSize(s_device) > SAMPLE_RATE * sizeof(int16_t)) {
        SDL_ClearQueuedAudio(s_device);
    }
    SDL_QueueAudio(s_device, tone->samples, (Uint32)(tone->count * sizeof(int16_t)));
}

void ui_sound_set_enabled(bool enabled) {
    s_enabled = enabled;
    if (!enabled && s_device) SDL_ClearQueuedAudio(s_device);
}

bool ui_sound_enabled(void) {
    return s_enabled;
}
