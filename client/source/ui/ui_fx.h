#pragma once
#include <stdbool.h>

// Ambient background effects: soft drifting light blobs plus a sparse
// particle field, drawn behind everything else on the main screens.
// Entirely procedural (a single generated radial-gradient texture plus
// sin/fmod motion) - no image assets, and cheap enough to run every frame
// at 1280x720.

// Creates the shared glow texture. Requires g_renderer, so call after
// ui_app_init(). Best-effort - if it fails, ui_fx_draw_background() simply
// draws nothing and screens keep their flat background.
void ui_fx_init(void);
void ui_fx_shutdown(void);

// Draws the animated backdrop over the already-cleared background. No-op
// when effects are disabled. Callers draw their own content on top as
// usual.
void ui_fx_draw_background(void);

// Reflects the persisted preference (see ui_prefs.h). When off,
// ui_fx_draw_background() does nothing and animated easing elsewhere snaps
// instead of interpolating (see ui_fx_ease).
void ui_fx_set_enabled(bool enabled);
bool ui_fx_enabled(void);

// Moves `current` a fraction of the way toward `target` - the standard
// "ease toward" used for selection highlights and the grid's zoom pop.
// Returns `target` outright when animations are off, so every caller gets
// the instant-snap behavior from one place rather than each testing the
// flag itself.
float ui_fx_ease(float current, float target, float rate);
