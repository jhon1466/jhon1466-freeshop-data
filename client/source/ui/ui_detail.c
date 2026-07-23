#include "ui_detail.h"
#include "ui_app.h"
#include "ui_icons.h"

#include <switch.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 1280
#define DETAIL_ICON_SIZE 160
#define LEFT_EDGE 90

#define DLC_SECTION_Y 390
#define DLC_LIST_TOP (DLC_SECTION_Y + 34)
#define DLC_ROW_HEIGHT 34
#define DLC_VISIBLE_ROWS 4

typedef enum {
    FOCUS_MAIN = 0,
    FOCUS_DLC_LIST,
} DetailFocus;

static const char *dlc_tag_label(const AppEntry *e) {
    return (strcmp(e->content_type, "update") == 0) ? "[UPD]" : "[DLC]";
}

static bool needs_dbi(const AppEntry *e) {
    return e->file_type == APP_FILE_TYPE_NSP || e->file_type == APP_FILE_TYPE_XCI;
}

UiDetailAction ui_show_detail(const AppEntry *entry, const AppEntry *dlc_entries, int dlc_count,
                              const AppEntry **out_target) {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    // Prime the baseline so a button still held from the previous screen
    // (e.g. A held to select this entry) isn't misread as newly pressed here.
    padUpdate(&pad);

    char header[300];
    snprintf(header, sizeof(header), "por %s   -   v%s   -   %.2f MB", entry->author, entry->version,
             entry->file_size / (1024.0 * 1024.0));

    DetailFocus focus = FOCUS_MAIN;
    int dlc_selected = 0;
    int dlc_scroll = 0;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (focus == FOCUS_MAIN) {
            if (kDown & HidNpadButton_A) {
                *out_target = entry;
                return UI_DETAIL_INSTALL;
            }
            if (kDown & HidNpadButton_B) {
                return UI_DETAIL_BACK;
            }
            if (dlc_count > 0 && (kDown & HidNpadButton_Y)) {
                focus = FOCUS_DLC_LIST;
            }
        } else {
            if (kDown & HidNpadButton_Down) {
                if (dlc_selected < dlc_count - 1) dlc_selected++;
            }
            if (kDown & HidNpadButton_Up) {
                if (dlc_selected > 0) dlc_selected--;
            }
            if (kDown & HidNpadButton_A) {
                *out_target = &dlc_entries[dlc_selected];
                return UI_DETAIL_INSTALL;
            }
            if (kDown & HidNpadButton_B) {
                focus = FOCUS_MAIN;
            }
        }

        if (dlc_selected < dlc_scroll) dlc_scroll = dlc_selected;
        if (dlc_selected >= dlc_scroll + DLC_VISIBLE_ROWS) dlc_scroll = dlc_selected - DLC_VISIBLE_ROWS + 1;

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        ui_draw_rect(60, 60, SCREEN_W - 120, 560, COLOR_PANEL);

        int text_x = LEFT_EDGE + DETAIL_ICON_SIZE + 24;
        ui_draw_rect(LEFT_EDGE, 90, DETAIL_ICON_SIZE, DETAIL_ICON_SIZE, COLOR_BG);
        SDL_Texture *icon = ui_icons_get(entry);
        if (icon) {
            SDL_Rect dst = { LEFT_EDGE, 90, DETAIL_ICON_SIZE, DETAIL_ICON_SIZE };
            SDL_RenderCopy(g_renderer, icon, NULL, &dst);
        }

        ui_draw_text(g_font_title, text_x, 90, COLOR_TEXT, entry->title);
        ui_draw_text(g_font_body, text_x, 140, COLOR_TEXT_DIM, header);

        int y = LEFT_EDGE + DETAIL_ICON_SIZE + 30;
        y = ui_draw_text_wrapped(g_font_body, LEFT_EDGE, y, SCREEN_W - 220, 28, COLOR_TEXT, entry->description);

        if (dlc_count == 0 && entry->long_description[0] != '\0') {
            y += 16;
            ui_draw_text_wrapped(g_font_small, LEFT_EDGE, y, SCREEN_W - 220, 24, COLOR_TEXT_DIM,
                                  entry->long_description);
        }

        if (dlc_count > 0) {
            char section_header[48];
            snprintf(section_header, sizeof(section_header), "DLC y actualizaciones (%d)", dlc_count);
            ui_draw_text(g_font_body, LEFT_EDGE, DLC_SECTION_Y, COLOR_TEXT, section_header);

            for (int i = dlc_scroll; i < dlc_count && i < dlc_scroll + DLC_VISIBLE_ROWS; i++) {
                int row_index = i - dlc_scroll;
                int row_y = DLC_LIST_TOP + row_index * DLC_ROW_HEIGHT;
                bool is_selected = (focus == FOCUS_DLC_LIST && i == dlc_selected);

                if (is_selected) {
                    ui_draw_rect(LEFT_EDGE - 10, row_y - 6, SCREEN_W - 220, DLC_ROW_HEIGHT - 4, COLOR_ACCENT);
                }
                SDL_Color row_color = is_selected ? COLOR_BG : COLOR_TEXT;

                char row_text[160];
                snprintf(row_text, sizeof(row_text), "%s %s", dlc_tag_label(&dlc_entries[i]), dlc_entries[i].title);
                ui_draw_text(g_font_body, LEFT_EDGE, row_y, row_color, row_text);
            }
        }

        char hint[160];
        if (focus == FOCUS_DLC_LIST) {
            snprintf(hint, sizeof(hint), "Arriba/Abajo: elegir    A: instalar seleccionado    B: volver al juego");
        } else if (dlc_count > 0) {
            snprintf(hint, sizeof(hint), "A: instalar    Y: ver DLC/actualizaciones (%d)    B: volver", dlc_count);
        } else {
            snprintf(hint, sizeof(hint), "%s", needs_dbi(entry) ? "A: instalar vía DBI    B: volver" : "A: instalar    B: volver");
        }
        ui_draw_text(g_font_small, LEFT_EDGE, 680, COLOR_TEXT_DIM, hint);

        SDL_RenderPresent(g_renderer);
    }

    return UI_DETAIL_BACK;
}
