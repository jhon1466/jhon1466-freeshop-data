#include "ui_fx.h"
#include "ui_app.h"

#include <switch.h>
#include <math.h>
#include <stdlib.h>

#define SCREEN_W 1280
#define SCREEN_H 720

// Resolution of the generated radial-gradient sprite. Everything is drawn
// by scaling this one texture way up, so it only needs enough resolution to
// avoid visible banding once blurred across a few hundred pixels - 128 is
// well past that, and costs 64KB.
#define GLOW_TEX_SIZE 128

#define PARTICLE_COUNT 30

static SDL_Texture *s_glow = NULL;
static bool s_enabled = true;

void ui_fx_init(void) {
    if (s_glow || !g_renderer) return;

    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, GLOW_TEX_SIZE, GLOW_TEX_SIZE, 32,
                                                           SDL_PIXELFORMAT_RGBA8888);
    if (!surface) return;

    SDL_LockSurface(surface);
    for (int y = 0; y < GLOW_TEX_SIZE; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch);
        for (int x = 0; x < GLOW_TEX_SIZE; x++) {
            float dx = (x - (GLOW_TEX_SIZE - 1) * 0.5f) / (GLOW_TEX_SIZE * 0.5f);
            float dy = (y - (GLOW_TEX_SIZE - 1) * 0.5f) / (GLOW_TEX_SIZE * 0.5f);
            float distance = sqrtf(dx * dx + dy * dy);
            // Squared falloff: a linear one leaves a visible hard edge at
            // the circle's rim once the sprite is scaled up this far.
            float strength = distance >= 1.0f ? 0.0f : 1.0f - distance;
            Uint8 alpha = (Uint8)(255.0f * strength * strength);
            row[x] = SDL_MapRGBA(surface->format, 255, 255, 255, alpha);
        }
    }
    SDL_UnlockSurface(surface);

    s_glow = SDL_CreateTextureFromSurface(g_renderer, surface);
    SDL_FreeSurface(surface);
    if (s_glow) SDL_SetTextureBlendMode(s_glow, SDL_BLENDMODE_BLEND);
}

void ui_fx_shutdown(void) {
    if (s_glow) {
        SDL_DestroyTexture(s_glow);
        s_glow = NULL;
    }
}

void ui_fx_set_enabled(bool enabled) {
    s_enabled = enabled;
}

bool ui_fx_enabled(void) {
    return s_enabled;
}

float ui_fx_ease(float current, float target, float rate) {
    if (!s_enabled) return target;
    return current + (target - current) * rate;
}

// One soft light blob, positioned and sized in screen fractions.
static void draw_glow(float x, float y, float radius, Uint8 r, Uint8 g, Uint8 b, Uint8 alpha) {
    int diameter = (int)(SCREEN_H * radius);
    SDL_Rect dst = { (int)(SCREEN_W * x) - diameter / 2, (int)(SCREEN_H * y) - diameter / 2,
                     diameter, diameter };
    SDL_SetTextureColorMod(s_glow, r, g, b);
    SDL_SetTextureAlphaMod(s_glow, alpha);
    SDL_RenderCopy(g_renderer, s_glow, NULL, &dst);
}

// Sparse drifting dots. Each one's path is a pure function of its index and
// the clock - no per-particle state to store or update, and it loops
// seamlessly because the horizontal travel wraps via fmodf.
static void draw_particles(float time) {
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        float travel = fmodf(i * 0.371f + time * 0.011f * (0.65f + (i % 5) * 0.11f), 1.12f) - 0.06f;
        float y = fmodf(i * 0.217f + 0.11f * sinf(time * 0.29f + i * 1.73f), 1.0f);
        float pulse = 0.45f + 0.55f * sinf(time * (0.9f + (i % 4) * 0.17f) + i);

        SDL_Color c = { 0xb6, 0xe0, 0xff, (Uint8)(88 * (0.55f + 0.45f * pulse)) };
        int size = (i % 9 == 0) ? 3 : 2;
        ui_draw_rect((int)(travel * SCREEN_W), (int)(y * SCREEN_H), size, size, c);
    }
}

void ui_fx_draw_background(void) {
    if (!s_enabled || !s_glow) return;

    float time = armTicksToNs(armGetSystemTick()) / 1e9f;

    // Four blobs on slow, mutually prime-ish cycles so the composition
    // never visibly repeats. Colors follow the app's accent (blue) with a
    // violet and a teal for depth, all at low alpha - this sits behind
    // text and must never compete with it for attention.
    draw_glow(0.10f + 0.13f * sinf(time * 0.43f), 0.20f + 0.11f * cosf(time * 0.37f), 0.90f, 45, 140, 255, 96);
    draw_glow(0.84f + 0.12f * cosf(time * 0.34f), 0.34f + 0.10f * sinf(time * 0.41f), 0.78f, 154, 75, 255, 84);
    draw_glow(0.54f + 0.10f * sinf(time * 0.29f), 0.91f + 0.06f * cosf(time * 0.33f), 0.94f, 0, 210, 190, 70);
    draw_glow(0.42f + 0.08f * cosf(time * 0.25f), 0.48f + 0.09f * sinf(time * 0.31f), 0.58f, 64, 125, 255, 50);

    draw_particles(time);

    // Leave the texture's modulation neutral - it's shared, and anything
    // drawn with it later would otherwise inherit this call's tint.
    SDL_SetTextureColorMod(s_glow, 255, 255, 255);
    SDL_SetTextureAlphaMod(s_glow, 255);
}
