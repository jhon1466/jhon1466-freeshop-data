#include "ui_app.h"

#include <stdio.h>
#include <string.h>

SDL_Renderer *g_renderer = NULL;
TTF_Font *g_font_title = NULL;
TTF_Font *g_font_body = NULL;
TTF_Font *g_font_small = NULL;

static SDL_Window *s_window = NULL;
static PlFontData s_font_data;
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

    return true;
}

void ui_app_shutdown(void) {
    if (g_font_title) { TTF_CloseFont(g_font_title); g_font_title = NULL; }
    if (g_font_body) { TTF_CloseFont(g_font_body); g_font_body = NULL; }
    if (g_font_small) { TTF_CloseFont(g_font_small); g_font_small = NULL; }
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

void ui_draw_progress_bar(int x, int y, int w, int h, float pct, SDL_Color fill, SDL_Color track) {
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    ui_draw_rect(x, y, w, h, track);
    int fill_w = (int)(w * pct);
    if (fill_w > 0) ui_draw_rect(x, y, fill_w, h, fill);
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
        ui_draw_text(g_font_small, 80, y + 20, COLOR_TEXT_DIM, "Presiona A o + para continuar");

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
        ui_draw_text(g_font_small, 80, y + 20, COLOR_TEXT_DIM, "A o +: sí    B: no");

        SDL_RenderPresent(g_renderer);
    }

    return false;
}
