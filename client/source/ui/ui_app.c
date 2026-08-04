#include "ui_app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

SDL_Renderer *g_renderer = NULL;
TTF_Font *g_font_title = NULL;
TTF_Font *g_font_body = NULL;
TTF_Font *g_font_small = NULL;

TTF_Font *g_font_glyph = NULL;

static SDL_Window *s_window = NULL;
static PlFontData s_font_data;
static PlFontData s_ext_font_data;
static bool s_ext_font_ok = false;
static bool s_pl_initialized = false;
static bool s_sdl_initialized = false;
static bool s_ttf_initialized = false;

// SDL_RWFromConstMem's close callback only frees the small RWops wrapper
// struct itself, never the underlying buffer - so freesrc=1 here is safe
// and does NOT free s_font_data.address (which is owned by the pl service
// for the lifetime of the process; there's no matching "free" call for it).
static TTF_Font *open_font_at_size(int point_size) {
    SDL_RWops *rw = SDL_RWFromConstMem(s_font_data.address, (int)s_font_data.size);
    if (!rw) return NULL;
    return TTF_OpenFontRW(rw, /*freesrc=*/1, point_size);
}

static TTF_Font *open_ext_font_at_size(int point_size) {
    if (!s_ext_font_ok) return NULL;
    SDL_RWops *rw = SDL_RWFromConstMem(s_ext_font_data.address, (int)s_ext_font_data.size);
    if (!rw) return NULL;
    return TTF_OpenFontRW(rw, /*freesrc=*/1, point_size);
}

// ---- Console button glyphs (PlSharedFontType_NintendoExt) ----
//
// The system's "Nintendo Extended" shared font carries the real controller
// glyphs (the round A/B/X/Y marks, the ZL/ZR shoulder shapes, +/-, the
// D-Pad, the sticks) as private-use codepoints - the same ones the console's
// own UI and every polished homebrew menu draw. Nothing about which
// codepoint is which is declared in libnx, and this font only exists on a
// real console (it comes from the pl service at runtime, not from a file in
// the toolchain), so the exact values couldn't be verified while building:
// each button below therefore lists a couple of candidate codepoints, and
// resolve_glyphs() keeps whichever one the font on *this* console actually
// provides (TTF_GlyphIsProvided32). Anything left unresolved falls back to
// the plain ASCII text in kFallback, so a wrong guess degrades to a readable
// "ZL" rather than a tofu box.
#define GLYPH_CANDIDATES_MAX 3

static const u32 kGlyphCandidates[UI_BTN_COUNT][GLYPH_CANDIDATES_MAX] = {
    [UI_BTN_A]       = { 0xE0E0, 0xE0A0, 0 },
    [UI_BTN_B]       = { 0xE0E1, 0xE0A1, 0 },
    [UI_BTN_X]       = { 0xE0E2, 0xE0A2, 0 },
    [UI_BTN_Y]       = { 0xE0E3, 0xE0A3, 0 },
    [UI_BTN_L]       = { 0xE0E4, 0xE0A4, 0 },
    [UI_BTN_R]       = { 0xE0E5, 0xE0A5, 0 },
    [UI_BTN_ZL]      = { 0xE0E6, 0xE0A6, 0 },
    [UI_BTN_ZR]      = { 0xE0E7, 0xE0A7, 0 },
    [UI_BTN_PLUS]    = { 0xE0EF, 0xE0B5, 0 },
    [UI_BTN_MINUS]   = { 0xE0F0, 0xE0B6, 0 },
    [UI_BTN_DPAD]    = { 0xE0AF, 0xE0B0, 0 },
    [UI_BTN_STICK_L] = { 0xE101, 0xE0C1, 0 },
    [UI_BTN_UP_DOWN] = { 0xE0EC, 0xE0B2, 0 },
};

static const char *const kFallback[UI_BTN_COUNT] = {
    [UI_BTN_A] = "A", [UI_BTN_B] = "B", [UI_BTN_X] = "X", [UI_BTN_Y] = "Y",
    [UI_BTN_L] = "L", [UI_BTN_R] = "R", [UI_BTN_ZL] = "ZL", [UI_BTN_ZR] = "ZR",
    [UI_BTN_PLUS] = "+", [UI_BTN_MINUS] = "-", [UI_BTN_DPAD] = "D-Pad",
    [UI_BTN_STICK_L] = "Stick L", [UI_BTN_UP_DOWN] = "Up/Down",
};

// Resolved once at init: the UTF-8 encoding of whichever candidate the font
// provides, or "" when none did (meaning: use kFallback and draw the boxed
// chip instead).
static char s_glyphs[UI_BTN_COUNT][5];

// Named apart from libnx's own encode_utf8() (switch/runtime/util/utf.h),
// which has a different signature and doesn't NUL-terminate.
static void encode_glyph_utf8(u32 cp, char out[5]) {
    // Every codepoint used here is in the U+E000..U+FFFF private-use range,
    // i.e. always the 3-byte form - no need for a general encoder.
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    out[3] = '\0';
}

static void resolve_glyphs(void) {
    memset(s_glyphs, 0, sizeof(s_glyphs));
    if (!g_font_glyph) return;

    for (int b = 0; b < UI_BTN_COUNT; b++) {
        for (int c = 0; c < GLYPH_CANDIDATES_MAX; c++) {
            u32 cp = kGlyphCandidates[b][c];
            if (cp == 0) break;
            if (TTF_GlyphIsProvided32(g_font_glyph, cp)) {
                encode_glyph_utf8(cp, s_glyphs[b]);
                break;
            }
        }
    }
}

const char *ui_button_glyph(UiButton button) {
    if (button < 0 || button >= UI_BTN_COUNT) return NULL;
    return s_glyphs[button][0] ? s_glyphs[button] : NULL;
}

bool ui_app_init(char *err_buf, size_t err_buf_size) {
    Result rc = plInitialize(PlServiceType_User);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "plInitialize falló: 0x%x", rc);
        return false;
    }
    s_pl_initialized = true;

    rc = plGetSharedFontByType(&s_font_data, PlSharedFontType_Standard);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "plGetSharedFontByType falló: 0x%x", rc);
        return false;
    }

    // Not fatal if missing - every button hint just falls back to its boxed
    // text chip (see ui_draw_button_hint), which is what the app drew before
    // the glyphs existed at all.
    s_ext_font_ok = R_SUCCEEDED(plGetSharedFontByType(&s_ext_font_data, PlSharedFontType_NintendoExt));

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "SDL_Init falló: %s", SDL_GetError());
        return false;
    }
    s_sdl_initialized = true;

    if (TTF_Init() != 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "TTF_Init falló: %s", TTF_GetError());
        return false;
    }
    s_ttf_initialized = true;

    s_window = SDL_CreateWindow(NULL, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 1280, 720, SDL_WINDOW_FULLSCREEN);
    if (!s_window) {
        if (err_buf) snprintf(err_buf, err_buf_size, "SDL_CreateWindow falló: %s", SDL_GetError());
        return false;
    }

    g_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_renderer) {
        if (err_buf) snprintf(err_buf, err_buf_size, "SDL_CreateRenderer falló: %s", SDL_GetError());
        return false;
    }

    g_font_title = open_font_at_size(28);
    g_font_body = open_font_at_size(20);
    g_font_small = open_font_at_size(16);
    if (!g_font_title || !g_font_body || !g_font_small) {
        if (err_buf) snprintf(err_buf, err_buf_size, "TTF_OpenFontRW falló: %s", TTF_GetError());
        return false;
    }

    // A couple of points larger than g_font_small it sits beside: these
    // glyphs are drawn inside their own round/rounded outline, so at a
    // matched point size the actual mark inside reads noticeably smaller
    // than the label next to it.
    g_font_glyph = open_ext_font_at_size(20);
    resolve_glyphs();

    return true;
}

void ui_app_shutdown(void) {
    if (g_font_title) { TTF_CloseFont(g_font_title); g_font_title = NULL; }
    if (g_font_body) { TTF_CloseFont(g_font_body); g_font_body = NULL; }
    if (g_font_small) { TTF_CloseFont(g_font_small); g_font_small = NULL; }
    if (g_font_glyph) { TTF_CloseFont(g_font_glyph); g_font_glyph = NULL; }
    if (g_renderer) { SDL_DestroyRenderer(g_renderer); g_renderer = NULL; }
    if (s_window) { SDL_DestroyWindow(s_window); s_window = NULL; }
    if (s_ttf_initialized) { TTF_Quit(); s_ttf_initialized = false; }
    if (s_sdl_initialized) { SDL_Quit(); s_sdl_initialized = false; }
    if (s_pl_initialized) { plExit(); s_pl_initialized = false; }
}

void ui_draw_text(TTF_Font *font, int x, int y, SDL_Color color, const char *text) {
    if (!font || !text || !text[0]) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_renderer, surf);
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_FreeSurface(surf);
    if (!tex) return;
    SDL_RenderCopy(g_renderer, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
}

void ui_draw_text_right(TTF_Font *font, int right_x, int y, SDL_Color color, const char *text) {
    if (!font || !text || !text[0]) return;
    int w = 0, h = 0;
    TTF_SizeUTF8(font, text, &w, &h);
    ui_draw_text(font, right_x - w, y, color, text);
}

void ui_draw_rect(int x, int y, int w, int h, SDL_Color color) {
    SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderFillRect(g_renderer, &rect);
}

void ui_draw_rounded_rect(int x, int y, int w, int h, int radius, SDL_Color color) {
    if (w <= 0 || h <= 0) return;
    if (radius <= 0) { ui_draw_rect(x, y, w, h, color); return; }
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);

    // The full-width band between the top/bottom corners, plus - scanned
    // one row at a time so each row's corners can stop short of the
    // straight edges by however much the circle demands at that height -
    // the top/bottom strips where the rounding actually happens. No SDL_gfx
    // or similar linked in for a native filled-circle/rounded-rect, so this
    // is the same "fill scanline spans from a circle equation" approach
    // ui_list.c's sidebar icons use for the handful of straight-line glyphs
    // that needed one, just for a whole quarter circle instead of a few
    // short segments.
    SDL_Rect mid = { x, y + radius, w, h - 2 * radius };
    SDL_RenderFillRect(g_renderer, &mid);

    for (int i = 0; i < radius; i++) {
        int dy = radius - i;
        int dx = (int)(sqrtf((float)(radius * radius - dy * dy)) + 0.5f);
        int inset = radius - dx;

        SDL_Rect top_row = { x + inset, y + i, w - 2 * inset, 1 };
        SDL_RenderFillRect(g_renderer, &top_row);
        SDL_Rect bottom_row = { x + inset, y + h - 1 - i, w - 2 * inset, 1 };
        SDL_RenderFillRect(g_renderer, &bottom_row);
    }
}

void ui_mask_rounded_corners(int x, int y, int w, int h, int radius, SDL_Color bg_color) {
    if (radius <= 0 || w <= 0 || h <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    SDL_SetRenderDrawColor(g_renderer, bg_color.r, bg_color.g, bg_color.b, bg_color.a);
    for (int i = 0; i < radius; i++) {
        int dy = radius - i;
        int dx = (int)(sqrtf((float)(radius * radius - dy * dy)) + 0.5f);
        int inset = radius - dx;
        if (inset <= 0) continue;

        SDL_Rect top_left = { x, y + i, inset, 1 };
        SDL_RenderFillRect(g_renderer, &top_left);
        SDL_Rect top_right = { x + w - inset, y + i, inset, 1 };
        SDL_RenderFillRect(g_renderer, &top_right);
        SDL_Rect bottom_left = { x, y + h - 1 - i, inset, 1 };
        SDL_RenderFillRect(g_renderer, &bottom_left);
        SDL_Rect bottom_right = { x + w - inset, y + h - 1 - i, inset, 1 };
        SDL_RenderFillRect(g_renderer, &bottom_right);
    }
}

// Horizontal inset of a rounded rect's edge at row `dy` (0-based from the
// shape's top) - 0 along the straight sides, growing into the corner arcs.
// Same circle equation ui_draw_rounded_rect scans with, factored out so the
// outline below can run it twice (outer edge and inner edge) per row.
static int rounded_row_inset(int h, int radius, int dy) {
    if (radius <= 0) return 0;
    int d;
    if (dy < radius) {
        d = radius - dy;
    } else if (dy >= h - radius) {
        d = dy - (h - radius) + 1;
    } else {
        return 0;
    }
    if (d > radius) d = radius;
    int dx = (int)(sqrtf((float)(radius * radius - d * d)) + 0.5f);
    return radius - dx;
}

void ui_draw_rounded_rect_outline(int x, int y, int w, int h, int radius, int thickness,
                                   SDL_Color color) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (thickness * 2 > w) thickness = w / 2;
    if (thickness * 2 > h) thickness = h / 2;
    if (thickness <= 0) return;

    int inner_w = w - 2 * thickness;
    int inner_h = h - 2 * thickness;
    int inner_radius = radius - thickness;
    if (inner_radius < 0) inner_radius = 0;

    SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);

    for (int dy = 0; dy < h; dy++) {
        int outer_inset = rounded_row_inset(h, radius, dy);
        int ox0 = x + outer_inset;
        int ox1 = x + w - outer_inset;

        int inner_dy = dy - thickness;
        if (inner_dy < 0 || inner_dy >= inner_h || inner_w <= 0) {
            // Above/below the hollow middle - this row is solid ring.
            SDL_Rect row = { ox0, y + dy, ox1 - ox0, 1 };
            SDL_RenderFillRect(g_renderer, &row);
            continue;
        }

        int inner_inset = rounded_row_inset(inner_h, inner_radius, inner_dy);
        int ix0 = x + thickness + inner_inset;
        int ix1 = x + thickness + inner_w - inner_inset;
        if (ix0 > ox0) {
            SDL_Rect left = { ox0, y + dy, ix0 - ox0, 1 };
            SDL_RenderFillRect(g_renderer, &left);
        }
        if (ox1 > ix1) {
            SDL_Rect right = { ix1, y + dy, ox1 - ix1, 1 };
            SDL_RenderFillRect(g_renderer, &right);
        }
    }
}

void ui_draw_focus_border(int x, int y, int w, int h, int radius) {
    // Borealis animates its highlight between two cyans rather than holding
    // one flat color - that slow pulse is a large part of why a focused
    // widget reads as "live" there. ~1.7 cycles/sec, matching how brisk the
    // real one feels.
    float t = armTicksToNs(armGetSystemTick()) / 1e9f;
    float k = (sinf(t * 3.4f) + 1.0f) * 0.5f;

    SDL_Color c1 = { 25, 138, 198, 0xff };  // brls/highlight/color1
    SDL_Color c2 = { 137, 241, 242, 0xff }; // brls/highlight/color2
    SDL_Color pulse = {
        (Uint8)(c1.r + (c2.r - c1.r) * k),
        (Uint8)(c1.g + (c2.g - c1.g) * k),
        (Uint8)(c1.b + (c2.b - c1.b) * k),
        0xff,
    };

    // Two rings: a dimmer one just outside the bright stroke, standing in
    // for the soft outer glow Borealis gets from a real shadow/feather
    // (which needs alpha compositing this renderer isn't set up for).
    ui_draw_rounded_rect_outline(x - 2, y - 2, w + 4, h + 4, radius + 2, 2, c1);
    ui_draw_rounded_rect_outline(x, y, w, h, radius, UI_FOCUS_STROKE_W, pulse);
}

void ui_draw_frame_header(int screen_w, const char *title, const char *subtitle) {
    if (title) {
        ui_draw_text(g_font_title, UI_FRAME_HEADER_PAD_SIDES, 28, COLOR_TEXT, title);
    }
    if (subtitle && subtitle[0]) {
        int title_w = 0, title_h = 0;
        if (title) TTF_SizeUTF8(g_font_title, title, &title_w, &title_h);
        ui_draw_text(g_font_body, UI_FRAME_HEADER_PAD_SIDES + title_w + 16, 36,
                     COLOR_SUBTITLE, subtitle);
    }
    ui_draw_rect(UI_FRAME_HEADER_PAD_SIDES, UI_FRAME_HEADER_H,
                 screen_w - UI_FRAME_HEADER_PAD_SIDES * 2, 1, COLOR_SEPARATOR);
}

int ui_draw_frame_footer(int screen_w, int screen_h) {
    int line_y = screen_h - UI_FRAME_FOOTER_H;
    ui_draw_rect(UI_FRAME_FOOTER_PAD_SIDES, line_y,
                 screen_w - UI_FRAME_FOOTER_PAD_SIDES * 2, 1, COLOR_SEPARATOR);
    // Hints sit centered in the band below the rule; ui_draw_button_hint
    // draws downward from the Y it's given, so bias up by roughly half a
    // hint's height.
    return line_y + UI_FRAME_FOOTER_H / 2 - 12;
}

void ui_draw_progress_bar(int x, int y, int w, int h, float pct, SDL_Color fill, SDL_Color track) {
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    int radius = h / 2;
    ui_draw_rounded_rect(x, y, w, h, radius, track);
    int fill_w = (int)(w * pct);
    // Below ~2*radius wide, ui_draw_rounded_rect's own clamp already falls
    // back to a plain rect, so an early fill (pct still near 0) doesn't
    // draw a rounded cap wider than the fill itself.
    if (fill_w > 0) ui_draw_rounded_rect(x, y, fill_w, h, radius, fill);
}

int ui_draw_battery_icon(int x, int y, int w, int h, int percent, bool charging, bool ok) {
    int nub_w = 3;
    int nub_h = h > 6 ? h / 2 : h;
    int body_w = w - nub_w;

    SDL_SetRenderDrawColor(g_renderer, COLOR_TEXT_DIM.r, COLOR_TEXT_DIM.g, COLOR_TEXT_DIM.b, COLOR_TEXT_DIM.a);
    SDL_Rect body = { x, y, body_w, h };
    SDL_RenderDrawRect(g_renderer, &body);
    SDL_Rect nub = { x + body_w, y + (h - nub_h) / 2, nub_w, nub_h };
    SDL_RenderFillRect(g_renderer, &nub);

    if (ok) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;

        SDL_Color fill_color;
        if (charging) fill_color = COLOR_ACCENT;
        else if (percent <= 15) fill_color = COLOR_DANGER;
        else if (percent <= 30) fill_color = COLOR_WARN;
        else fill_color = COLOR_QUEUED;

        int pad = 2;
        int inner_w = body_w - pad * 2;
        int inner_h = h - pad * 2;
        int fill_w = inner_w * percent / 100;
        if (fill_w > 0 && inner_h > 0) ui_draw_rect(x + pad, y + pad, fill_w, inner_h, fill_color);
    }
    return w;
}

int ui_draw_wifi_icon(int x, int y, int h, int strength, bool is_wifi, bool ok) {
    const int bars = 4;
    const int bar_w = 3, gap = 2;
    int lit = 0;
    if (ok) {
        if (!is_wifi) {
            lit = bars; // signal strength isn't meaningful over a wired connection
        } else {
            lit = strength + 1; // nifm's strength is 0-3; even the weakest connected reading lights one bar
            if (lit > bars) lit = bars;
            if (lit < 1) lit = 1;
        }
    }

    for (int i = 0; i < bars; i++) {
        int bar_h = h * (i + 1) / bars;
        int bx = x + i * (bar_w + gap);
        int by = y + (h - bar_h);
        SDL_Color c = (i < lit) ? COLOR_TEXT : COLOR_TEXT_DIM;
        ui_draw_rect(bx, by, bar_w, bar_h, c);
    }
    return bars * bar_w + (bars - 1) * gap;
}

int ui_draw_button_hint(int x, int y, UiButton button, const char *label) {
    const char *glyph = ui_button_glyph(button);
    int key_w = 0, key_h = 0;
    int label_w = 0, label_h = 0;
    if (label) TTF_SizeUTF8(g_font_small, label, &label_w, &label_h);

    int key_x = x;
    if (glyph) {
        TTF_SizeUTF8(g_font_glyph, glyph, &key_w, &key_h);
    } else {
        // No console glyph resolved - fall back to a bordered text chip,
        // sized around the button's ASCII name.
        const char *text = kFallback[button];
        int text_w, text_h;
        TTF_SizeUTF8(g_font_small, text, &text_w, &text_h);
        key_w = text_w + 12;
        key_h = text_h + 6;
        ui_draw_rect(key_x, y, key_w, key_h, COLOR_ACCENT);
        ui_draw_rect(key_x + 2, y + 2, key_w - 4, key_h - 4, COLOR_PANEL);
        ui_draw_text(g_font_small, key_x + 6, y + 3, COLOR_TEXT, text);
    }

    // Vertically center the shorter of the two against the taller, so the
    // glyph and its label read as one unit rather than one riding high.
    int row_h = key_h > label_h ? key_h : label_h;
    if (glyph) {
        ui_draw_text(g_font_glyph, key_x, y + (row_h - key_h) / 2, COLOR_TEXT, glyph);
    }
    // A NULL label means "this button is part of a pair" (ZL then ZR, say) -
    // return just past the glyph with only a hair of space, so the next
    // call's glyph sits right beside this one and the label after it reads
    // as belonging to both.
    if (!label) return key_x + key_w + 4;

    int label_x = key_x + key_w + 6;
    ui_draw_text(g_font_small, label_x, y + (row_h - label_h) / 2, COLOR_TEXT_DIM, label);
    return label_x + label_w + 20;
}

void ui_truncate_to_width(TTF_Font *font, const char *text, int max_w, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", text);
    if (!font) return;

    int w = 0, h = 0;
    TTF_SizeUTF8(font, out, &w, &h);
    if (w <= max_w) return;

    size_t len = strlen(out);
    while (len > 1) {
        len--;
        snprintf(out, out_size, "%.*s...", (int)len, text);
        TTF_SizeUTF8(font, out, &w, &h);
        if (w <= max_w) return;
    }
}

void ui_format_bytes(int64_t bytes, char *out, size_t out_size) {
    if (bytes < 0) bytes = 0;

    double kb = bytes / 1024.0;
    double mb = kb / 1024.0;
    double gb = mb / 1024.0;

    // Two decimals for GB, one for MB: at GB scale a single decimal only
    // moves once every ~100MB, which reads as a stalled transfer.
    if (gb >= 1.0) snprintf(out, out_size, "%.2f GB", gb);
    else if (mb >= 1.0) snprintf(out, out_size, "%.1f MB", mb);
    else snprintf(out, out_size, "%.0f KB", kb);
}

int ui_draw_text_wrapped(TTF_Font *font, int x, int y, int max_width, int line_height,
                          SDL_Color color, const char *text) {
    if (!font || !text) return y;

    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", text);

    char line[300] = "";
    char *saveptr = NULL;
    char *word = strtok_r(buf, " ", &saveptr);

    while (word) {
        char candidate[300];
        if (line[0] == '\0') {
            snprintf(candidate, sizeof(candidate), "%s", word);
        } else {
            snprintf(candidate, sizeof(candidate), "%s %s", line, word);
        }

        int w = 0, h = 0;
        TTF_SizeUTF8(font, candidate, &w, &h);

        if (w > max_width && line[0] != '\0') {
            ui_draw_text(font, x, y, color, line);
            y += line_height;
            snprintf(line, sizeof(line), "%s", word);
        } else {
            snprintf(line, sizeof(line), "%s", candidate);
        }

        word = strtok_r(NULL, " ", &saveptr);
    }

    if (line[0] != '\0') {
        ui_draw_text(font, x, y, color, line);
        y += line_height;
    }

    return y;
}

// Screen is 1280 wide (see e.g. ui_list.c's SCREEN_W) - 80px margin on each
// side to match the x=80 these dialogs already draw at.
#define UI_APP_MSG_MAX_WIDTH (1280 - 80 * 2)
#define UI_APP_MSG_LINE_HEIGHT 30

// Splits `msg` on '\n' (each resulting paragraph word-wrapped to
// UI_APP_MSG_MAX_WIDTH via ui_draw_text_wrapped - unlike the plain
// ui_draw_text these dialogs used to call per line, which doesn't wrap and
// silently ran text off the right edge of the screen for anything long
// without its own line breaks, e.g. a GitHub release's one-paragraph body
// text) and draws them starting at `y`, preserving blank lines (an empty
// "line" between two '\n's, from something like "...\n\n...") as vertical
// spacing rather than collapsing them the way strtok_r would. Returns the y
// position just after the last line drawn.
static int draw_wrapped_message(int x, int y, SDL_Color color, const char *msg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", msg);

    char *cursor = buf;
    while (*cursor) {
        char *newline = strchr(cursor, '\n');
        if (newline) *newline = '\0';

        if (*cursor) {
            y = ui_draw_text_wrapped(g_font_body, x, y, UI_APP_MSG_MAX_WIDTH, UI_APP_MSG_LINE_HEIGHT, color, cursor);
        } else {
            y += UI_APP_MSG_LINE_HEIGHT;
        }

        if (!newline) break;
        cursor = newline + 1;
    }
    return y;
}

void ui_app_show_message(const char *msg) {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    // Prime the baseline so a button still held from the previous screen
    // isn't misread as newly pressed here.
    padUpdate(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        if (kDown & (HidNpadButton_A | HidNpadButton_Plus)) break;

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        int y = draw_wrapped_message(80, 300, COLOR_TEXT, msg);
        ui_draw_button_hint(80, y + 20, UI_BTN_A, "continuar");

        SDL_RenderPresent(g_renderer);
    }
}

bool ui_app_show_confirm(const char *msg) {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    padUpdate(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        if (kDown & (HidNpadButton_A | HidNpadButton_Plus)) return true;
        if (kDown & HidNpadButton_B) return false;

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        int y = draw_wrapped_message(80, 300, COLOR_TEXT, msg);
        int hx = ui_draw_button_hint(80, y + 20, UI_BTN_A, "sí");
        ui_draw_button_hint(hx, y + 20, UI_BTN_B, "no");

        SDL_RenderPresent(g_renderer);
    }

    return false;
}
