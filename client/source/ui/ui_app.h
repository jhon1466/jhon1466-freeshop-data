#pragma once
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>
#include <stddef.h>

// pipensx's own dark theme (src/ui/theme.hpp, read directly from
// https://github.com/i3sey/pipensx before choosing these) - "Joy-Con Neon
// Blue" accent, its dark-mode Surface/Success/Warning/Error tones, and its
// dark-mode text levels. Every screen in this app already draws through
// these same macros, so retheming here restyles the whole interface at once
// instead of needing a per-screen pass.
#define COLOR_BG        ((SDL_Color){0x18, 0x18, 0x1b, 0xff}) // pipensx's darkest neutral surface (meter track)
#define COLOR_PANEL     ((SDL_Color){0x3a, 0x3a, 0x42, 0xff}) // pipensx "Surface" (dark) - cards/panels
#define COLOR_ACCENT    ((SDL_Color){0x00, 0xc3, 0xe3, 0xff}) // pipensx accent - "Joy-Con Neon Blue #00C3E3"
// Marks an entry queued for batch install (see ui_queue.h) - deliberately a
// different hue from COLOR_ACCENT's selection highlight so the two are never
// confused for one another. pipensx's dark-mode "Success".
#define COLOR_QUEUED    ((SDL_Color){0x60, 0xdc, 0x82, 0xff})
#define COLOR_TEXT      ((SDL_Color){0xf5, 0xf5, 0xfa, 0xff}) // pipensx dark-mode text "Primary"
#define COLOR_TEXT_DIM  ((SDL_Color){0xb9, 0xb9, 0xc3, 0xff}) // pipensx dark-mode text "Secondary"
// Battery-level fill colors for ui_draw_battery_icon - pipensx's dark-mode
// "Warning"/"Error".
#define COLOR_WARN      ((SDL_Color){0xe6, 0x96, 0x50, 0xff})
#define COLOR_DANGER    ((SDL_Color){0xff, 0x45, 0x54, 0xff})
// Progress-bar track color - pipensx's is a translucent gray
// (RGBA(128,128,128,70)) laid over whatever panel sits behind it; this app's
// simple SDL_RenderFillRect draws don't do alpha compositing, so this is
// that same translucent gray pre-blended against COLOR_PANEL (the
// backdrop every progress bar in this app sits on) instead.
#define COLOR_TRACK     ((SDL_Color){0x4d, 0x4d, 0x53, 0xff})

// ---- Borealis frame chrome ----
//
// pipensx is a Borealis app (https://github.com/xfangfang/borealis), and
// most of what makes it *look* like a Switch app isn't its palette - it's
// Borealis's own AppletFrame chrome and focus treatment. These are that
// framework's real registered style metrics/colors, read from its
// style.cpp/theme.cpp rather than eyeballed:
//   brls/applet_frame/header_height        88
//   brls/applet_frame/header_padding_sides 35
//   brls/applet_frame/footer_height        73
//   brls/applet_frame/footer_padding_sides 25
//   brls/highlight/stroke_width             5
//   brls/header/border      RGB(78,78,78)
//   brls/header/subtitle    RGB(163,163,163)
//   brls/highlight/color1   RGB(25,138,198)   (focus border pulses...
//   brls/highlight/color2   RGB(137,241,242)   ...between these two)
#define UI_FRAME_HEADER_H 88
#define UI_FRAME_HEADER_PAD_SIDES 35
#define UI_FRAME_FOOTER_H 73
#define UI_FRAME_FOOTER_PAD_SIDES 25
#define UI_FOCUS_STROKE_W 5

#define COLOR_SEPARATOR ((SDL_Color){0x4e, 0x4e, 0x4e, 0xff})
#define COLOR_SUBTITLE  ((SDL_Color){0xa3, 0xa3, 0xa3, 0xff})

extern SDL_Renderer *g_renderer;
extern TTF_Font *g_font_title; // 28pt
extern TTF_Font *g_font_body;  // 20pt
extern TTF_Font *g_font_small; // 16pt
// The console's "Nintendo Extended" shared font, holding the real
// controller button glyphs - NULL if this console didn't provide it (every
// caller then falls back to text, see ui_draw_button_hint).
extern TTF_Font *g_font_glyph;

typedef enum {
    UI_BTN_A = 0,
    UI_BTN_B,
    UI_BTN_X,
    UI_BTN_Y,
    UI_BTN_L,
    UI_BTN_R,
    UI_BTN_ZL,
    UI_BTN_ZR,
    UI_BTN_PLUS,
    UI_BTN_MINUS,
    UI_BTN_DPAD,
    UI_BTN_STICK_L,
    UI_BTN_UP_DOWN,
    UI_BTN_COUNT,
} UiButton;

// The UTF-8 encoding of `button`'s glyph in the console's own extended
// font, or NULL when this console's font didn't provide one (see the
// candidate-codepoint probing in ui_app.c). Callers that just want to draw
// a hint should use ui_draw_button_hint instead, which handles the
// fallback for them.
const char *ui_button_glyph(UiButton button);

// Draws one footer hint - the console's own button glyph (or, failing that,
// a bordered text chip) followed by `label` in dim text. Returns the x
// coordinate just past it, so a caller chains several across a line by
// feeding each return value into the next call's `x`.
int ui_draw_button_hint(int x, int y, UiButton button, const char *label);

// Initializes pl (system shared font) + SDL2 + SDL2_ttf and opens
// g_font_title/body/small from the console's own font, no bundled font
// file needed. Returns false with a reason in err_buf on any failure -
// caller should fall back to a plain console message in that case, since
// this graphics stack may not be usable to report its own failure.
bool ui_app_init(char *err_buf, size_t err_buf_size);
void ui_app_shutdown(void);

void ui_draw_text(TTF_Font *font, int x, int y, SDL_Color color, const char *text);

// Draws text right-aligned so its right edge lands at `right_x`.
void ui_draw_text_right(TTF_Font *font, int right_x, int y, SDL_Color color, const char *text);
void ui_draw_rect(int x, int y, int w, int h, SDL_Color color);

// Same as ui_draw_rect but with rounded corners (radius clamped to half the
// shorter side) - filled via horizontal scanline spans from a circle
// equation, since there's no stroke/rounded-rect primitive in SDL2 itself
// and no SDL_gfx or similar linked in to add one.
void ui_draw_rounded_rect(int x, int y, int w, int h, int radius, SDL_Color color);

// Paints over the four corners of a `w`x`h` rect at (x, y) with `bg_color`,
// using the same scanline-inset circle math as ui_draw_rounded_rect but
// filling the excluded region instead of the included one. Call this right
// after drawing a square texture (there's no cheap real alpha-masked
// texture clipping in plain SDL2) with `bg_color` matching whatever was
// drawn immediately behind that texture, and its square corners read as
// rounded instead.
void ui_mask_rounded_corners(int x, int y, int w, int h, int radius, SDL_Color bg_color);

// Rounded-rect *outline* (a ring `thickness` px wide just inside the given
// bounds) rather than a filled shape - the missing counterpart to
// ui_draw_rounded_rect, needed because Borealis draws focus as a stroke
// around a widget, never as a fill behind it.
void ui_draw_rounded_rect_outline(int x, int y, int w, int h, int radius, int thickness,
                                   SDL_Color color);

// THE Borealis focus treatment: a UI_FOCUS_STROKE_W-thick rounded border
// drawn *around* the focused widget, its color pulsing between
// brls/highlight/color1 and color2. This is what makes a Switch UI read as
// a Switch UI, and it's why selection here should never be a solid accent
// fill behind the item (which is what this app did everywhere before) - the
// item keeps its own normal colors and just gains a glowing frame.
void ui_draw_focus_border(int x, int y, int w, int h, int radius);

// Borealis AppletFrame chrome. Header: title (and optional subtitle) in the
// top UI_FRAME_HEADER_H px, with a separator rule under it. Footer: a
// separator rule with UI_FRAME_FOOTER_H px below it for button hints.
// `screen_w` lets callers that draw at a non-1280 width stay correct.
void ui_draw_frame_header(int screen_w, const char *title, const char *subtitle);
// Draws the footer's separator rule and returns the Y button hints should
// be drawn at, vertically centered in the footer band.
int ui_draw_frame_footer(int screen_w, int screen_h);

void ui_draw_progress_bar(int x, int y, int w, int h, float pct, SDL_Color fill, SDL_Color track);

// Draws a small battery gauge (outline + terminal nub, filled to `percent`)
// at (x, y), `w` x `h` pixels - the real hardware chip, not the word
// "battery". Fill color signals level (green/yellow/red) or "charging"
// (accent blue) regardless of level. `ok` false (no psm reading) draws just
// the empty outline. Returns the width actually drawn (== w, provided for
// symmetry with ui_draw_wifi_icon's variable width).
int ui_draw_battery_icon(int x, int y, int w, int h, int percent, bool charging, bool ok);

// Draws a 4-bar signal-strength icon bottom-aligned within `h` pixels
// starting at (x, y) - `strength` (0-3, nifm's own scale) bars lit for
// WiFi, all 4 lit for a wired connection (signal strength isn't
// meaningful over Ethernet), none lit if `ok` is false. Returns the total
// width drawn, so a caller laying out a right-aligned status row can place
// whatever comes next.
int ui_draw_wifi_icon(int x, int y, int h, int strength, bool is_wifi, bool ok);

// Word-wraps `text` to at most max_width pixels and draws each line
// line_height apart. Returns the y coordinate just below the last line.
int ui_draw_text_wrapped(TTF_Font *font, int x, int y, int max_width, int line_height,
                          SDL_Color color, const char *text);

// Truncates `text` to fit within max_w pixels, appending "..." if cut.
void ui_truncate_to_width(TTF_Font *font, const char *text, int max_w, char *out, size_t out_size);

// Human-readable byte count, picking KB/MB/GB by magnitude. Raw byte
// counts are unreadable at the sizes this app deals in - a game's progress
// shown as "2463583744 / 4281265152 bytes" says far less at a glance than
// "2.29 / 3.99 GB", and the digits churn every frame.
void ui_format_bytes(int64_t bytes, char *out, size_t out_size);

// Full-screen modal message (supports '\n'), blocks until A or + is pressed.
void ui_app_show_message(const char *msg);

// Same as ui_app_show_message but blocks until A/+ (returns true) or B
// (returns false) is pressed - for a yes/no confirmation.
bool ui_app_show_confirm(const char *msg);
