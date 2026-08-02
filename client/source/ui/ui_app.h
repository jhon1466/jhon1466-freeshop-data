#pragma once
#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>
#include <stddef.h>

// Tinfoil-ish dark theme.
#define COLOR_BG        ((SDL_Color){0x1b, 0x1f, 0x2a, 0xff})
#define COLOR_PANEL     ((SDL_Color){0x24, 0x29, 0x38, 0xff})
#define COLOR_ACCENT    ((SDL_Color){0x3a, 0x8f, 0xd8, 0xff})
// Marks an entry queued for batch install (see ui_queue.h) - deliberately a
// different hue from COLOR_ACCENT's selection highlight so the two are never
// confused for one another.
#define COLOR_QUEUED    ((SDL_Color){0x4a, 0xc4, 0x6a, 0xff})
#define COLOR_TEXT      ((SDL_Color){0xe8, 0xea, 0xf0, 0xff})
#define COLOR_TEXT_DIM  ((SDL_Color){0x8a, 0x90, 0xa0, 0xff})

extern SDL_Renderer *g_renderer;
extern TTF_Font *g_font_title; // 28pt
extern TTF_Font *g_font_body;  // 20pt
extern TTF_Font *g_font_small; // 16pt

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
void ui_draw_progress_bar(int x, int y, int w, int h, float pct, SDL_Color fill, SDL_Color track);

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
