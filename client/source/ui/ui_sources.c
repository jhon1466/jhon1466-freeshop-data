#include "ui_sources.h"
#include "ui_app.h"

#include <switch.h>
#include <stdio.h>

#define SCREEN_W 1280
#define SCREEN_H 720
#define LEFT_EDGE 20
#define RIGHT_EDGE (SCREEN_W - 20)
#define HEADER_Y 40
#define LIST_TOP 140
#define ROW_HEIGHT 56
#define FOOTER_Y (SCREEN_H - 46)

// Blocks until the keyboard closes. Returns true and fills `out` only if the
// user confirmed non-empty text - false (out left as "") on cancel or error,
// so callers can tell "no input" apart from "confirmed empty".
static bool prompt_text(const char *header, const char *guide, const char *initial,
                         char *out, size_t out_size) {
    out[0] = '\0';

    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;

    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetGuideText(&kbd, guide);
    if (initial) swkbdConfigSetInitialText(&kbd, initial);
    swkbdConfigSetStringLenMax(&kbd, (u32)(out_size - 1));

    Result rc = swkbdShow(&kbd, out, out_size);
    swkbdClose(&kbd);

    return R_SUCCEEDED(rc) && out[0] != '\0';
}

// URL is required (canceling aborts the whole add); name is optional and
// falls back to the URL itself if left blank or canceled.
static bool add_source(SourceList *list) {
    if (list->count >= SOURCES_MAX) return false;

    char url[SOURCE_URL_MAX];
    if (!prompt_text("Nueva fuente - dirección del servidor", "Ej: http://192.168.1.10:8080",
                      "http://", url, sizeof(url))) {
        return false;
    }

    char name[SOURCE_NAME_MAX];
    prompt_text("Nueva fuente - nombre (opcional)", "Cómo se muestra en la lista", NULL,
                name, sizeof(name));
    // Precision-bounded so a URL longer than SOURCE_NAME_MAX truncates
    // instead of (harmlessly, but noisily to the compiler) risking it.
    if (name[0] == '\0') snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1, url);

    CatalogSource *dst = &list->items[list->count];
    snprintf(dst->base_url, sizeof(dst->base_url), "%s", url);
    snprintf(dst->name, sizeof(dst->name), "%s", name);
    dst->enabled = true;
    dst->hidden = false; // sources users add are always visible/manageable
    list->count++;
    return true;
}

// Hidden sources (the operator's own bootstrap default - see sources.h)
// never appear here and can't be selected/toggled/deleted from this screen;
// they still count toward the merged catalog fetch in main.c. Rebuilt every
// frame from `list` (cheap - SOURCES_MAX is 8).
static int build_visible_index(const SourceList *list, int *out_visible) {
    int n = 0;
    for (int i = 0; i < list->count; i++) {
        if (!list->items[i].hidden) out_visible[n++] = i;
    }
    return n;
}

bool ui_show_sources(SourceList *list) {
    bool changed = false;
    int selected = 0; // index into the visible[] array, not list->items directly

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    // Prime the baseline - see ui_list.c for why a fresh PadState's first
    // padGetButtonsDown() can misread a button still held from the caller.
    padUpdate(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        int visible[SOURCES_MAX];
        int visible_count = build_visible_index(list, visible);
        if (selected >= visible_count) selected = visible_count - 1;
        if (selected < 0) selected = 0;

        if (kDown & HidNpadButton_Down) {
            if (selected < visible_count - 1) selected++;
        }
        if (kDown & HidNpadButton_Up) {
            if (selected > 0) selected--;
        }
        if ((kDown & HidNpadButton_A) && visible_count > 0) {
            CatalogSource *src = &list->items[visible[selected]];
            src->enabled = !src->enabled;
            changed = true;
        }
        if (kDown & HidNpadButton_Y) {
            if (add_source(list)) {
                // The new entry is always visible and appended at the raw
                // array's end, so it's also last in visible[] next frame.
                visible_count = build_visible_index(list, visible);
                selected = visible_count - 1;
                changed = true;
            }
            // swkbd's applet takeover means whatever was physically held
            // when it closed would otherwise look "freshly pressed" here.
            padUpdate(&pad);
        }
        if ((kDown & HidNpadButton_X) && visible_count > 0) {
            int real_index = visible[selected];
            for (int i = real_index; i < list->count - 1; i++) {
                list->items[i] = list->items[i + 1];
            }
            list->count--;
            visible_count = build_visible_index(list, visible);
            if (selected >= visible_count) selected = visible_count - 1;
            changed = true;
        }
        if (kDown & HidNpadButton_B) {
            return changed;
        }

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, "Fuentes");

        if (visible_count == 0) {
            ui_draw_text(g_font_body, LEFT_EDGE, LIST_TOP, COLOR_TEXT_DIM, "(sin fuentes configuradas)");
        }

        for (int row = 0; row < visible_count; row++) {
            const CatalogSource *item = &list->items[visible[row]];
            int row_y = LIST_TOP + row * ROW_HEIGHT;
            bool is_selected = (row == selected);

            if (is_selected) {
                ui_draw_rect(LEFT_EDGE, row_y - 8, RIGHT_EDGE - LEFT_EDGE, ROW_HEIGHT - 6, COLOR_ACCENT);
            } else if (row % 2 == 1) {
                ui_draw_rect(LEFT_EDGE, row_y - 8, RIGHT_EDGE - LEFT_EDGE, ROW_HEIGHT - 6, COLOR_PANEL);
            }

            SDL_Color text_color = is_selected ? COLOR_BG : COLOR_TEXT;
            SDL_Color dim_color = is_selected ? COLOR_BG : COLOR_TEXT_DIM;
            const char *state = item->enabled ? "[activa]" : "[inactiva]";

            ui_draw_text(g_font_body, LEFT_EDGE + 20, row_y, text_color, item->name);
            ui_draw_text(g_font_small, LEFT_EDGE + 20, row_y + 26, dim_color, item->base_url);
            ui_draw_text_right(g_font_body, RIGHT_EDGE - 20, row_y + 4, text_color, state);
        }

        ui_draw_rect(LEFT_EDGE, FOOTER_Y - 10, RIGHT_EDGE - LEFT_EDGE, 1, COLOR_PANEL);
        ui_draw_text(g_font_small, LEFT_EDGE, FOOTER_Y, COLOR_TEXT_DIM,
                     "A: activar/desactivar    Y: agregar    X: eliminar    B: volver");

        SDL_RenderPresent(g_renderer);
    }

    return changed;
}
