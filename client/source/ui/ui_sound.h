#pragma once
#include <stdbool.h>

// Short UI feedback tones, synthesized at startup rather than loaded from
// files - a handful of sine sweeps costs a few KB of RAM and nothing on
// disk, versus shipping audio assets in romfs for sounds this brief. Same
// approach the NetherSX2 launcher uses.
typedef enum {
    UI_SOUND_NAVIGATE = 0, // selection moved
    UI_SOUND_CONFIRM,      // A / accepted
    UI_SOUND_BACK,         // B / dismissed
    UI_SOUND_COUNT,
} UiSound;

// Opens an audio device and generates the tones. Best-effort: a failure
// here just means ui_sound_play() does nothing, never a startup error -
// audio is decoration, and the app is perfectly usable silent.
void ui_sound_init(void);
void ui_sound_shutdown(void);

// No-op when audio failed to initialize or the user turned sounds off.
void ui_sound_play(UiSound sound);

// Reflects the persisted preference (see ui_prefs.h). Applied immediately.
void ui_sound_set_enabled(bool enabled);
bool ui_sound_enabled(void);
