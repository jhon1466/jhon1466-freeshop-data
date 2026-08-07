#include "ui_torrent_select.h"
#include "ui_app.h"
#include "ui_nav.h"
#include "ui_sound.h"

#include <switch.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 1280
#define SCREEN_H 720
#define LEFT_EDGE UI_FRAME_FOOTER_PAD_SIDES
#define RIGHT_EDGE (SCREEN_W - UI_FRAME_FOOTER_PAD_SIDES)

#define CONTENT_TOP (UI_FRAME_HEADER_H + 24)
#define ROW_H 44
#define VISIBLE_ROWS ((SCREEN_H - UI_FRAME_FOOTER_H - 16 - CONTENT_TOP) / ROW_H)
#define ROW_NAME_X (LEFT_EDGE + 44)
#define ROW_NAME_MAX_W 820
#define ROW_SIZE_X (RIGHT_EDGE - 130)

#define CHECKBOX_SIZE 22

static void truncate_to_width(TTF_Font *font, const char *text, int max_w, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", text);
    int w = 0, h = 0;
    TTF_SizeUTF8(font, out, &w, &h);
    if (w <= max_w) return;

    size_t full_len = strlen(text);
    size_t lo = 0, hi = full_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        while (mid > lo && ((unsigned char)text[mid] & 0xC0) == 0x80) mid--;
        if (mid == lo) break;
        snprintf(out, out_size, "%.*s...", (int)mid, text);
        TTF_SizeUTF8(font, out, &w, &h);
        if (w <= max_w) lo = mid;
        else hi = mid - 1;
    }
    snprintf(out, out_size, "%.*s...", (int)lo, text);
}

// A hollow square, filled solid when `checked` - same drawing primitives
// (ui_draw_rect only ever fills, no stroke) draw_queue_badge/similar
// checkbox-shaped indicators elsewhere in this app already use.
static void draw_checkbox(int x, int y, bool checked) {
    if (checked) {
        ui_draw_rect(x, y, CHECKBOX_SIZE, CHECKBOX_SIZE, COLOR_ACCENT);
        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        int x1 = x + CHECKBOX_SIZE * 2 / 10, y1 = y + CHECKBOX_SIZE * 5 / 10;
        int x2 = x + CHECKBOX_SIZE * 4 / 10, y2 = y + CHECKBOX_SIZE * 7 / 10;
        int x3 = x + CHECKBOX_SIZE * 8 / 10, y3 = y + CHECKBOX_SIZE * 3 / 10;
        for (int off = -1; off <= 1; off++) {
            SDL_RenderDrawLine(g_renderer, x1, y1 + off, x2, y2 + off);
            SDL_RenderDrawLine(g_renderer, x2, y2 + off, x3, y3 + off);
        }
    } else {
        ui_draw_rect(x, y, CHECKBOX_SIZE, CHECKBOX_SIZE, COLOR_TEXT_DIM);
        ui_draw_rect(x + 2, y + 2, CHECKBOX_SIZE - 4, CHECKBOX_SIZE - 4, COLOR_BG);
    }
}

UiTorrentSelectResult ui_show_torrent_select(const TorrentFileEntry *files, int file_count,
                                             int *out_selected_indices, int *out_selected_count) {
    // Every file starts checked - the prior, only behavior (install
    // everything) stays the default; this screen is purely opt-out.
    static bool checked[TORRENT_PREVIEW_MAX_FILES];
    for (int i = 0; i < file_count && i < TORRENT_PREVIEW_MAX_FILES; i++) checked[i] = true;

    int selected = 0;
    int scroll_offset = 0;

    NavRepeatState nav_up = {0}, nav_down = {0};

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    padUpdate(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        u64 kHeld = padGetButtons(&pad);
        HidAnalogStickState stick = padGetStickPos(&pad, 0);
        bool held_up = (kHeld & HidNpadButton_Up) || stick.y > NAV_STICK_DEADZONE;
        bool held_down = (kHeld & HidNpadButton_Down) || stick.y < -NAV_STICK_DEADZONE;
        u64 now_tick = armGetSystemTick();

        if (nav_repeat_step(&nav_down, held_down, now_tick) && selected < file_count - 1) {
            selected++;
            ui_sound_play(UI_SOUND_NAVIGATE);
        }
        if (nav_repeat_step(&nav_up, held_up, now_tick) && selected > 0) {
            selected--;
            ui_sound_play(UI_SOUND_NAVIGATE);
        }
        if (selected < scroll_offset) scroll_offset = selected;
        if (selected >= scroll_offset + VISIBLE_ROWS) scroll_offset = selected - VISIBLE_ROWS + 1;

        if ((kDown & HidNpadButton_A) && file_count > 0) {
            checked[selected] = !checked[selected];
            ui_sound_play(UI_SOUND_CONFIRM);
        }
        if (kDown & HidNpadButton_X) {
            for (int i = 0; i < file_count; i++) checked[i] = true;
            ui_sound_play(UI_SOUND_NAVIGATE);
        }
        if (kDown & HidNpadButton_Y) {
            for (int i = 0; i < file_count; i++) checked[i] = false;
            ui_sound_play(UI_SOUND_NAVIGATE);
        }

        int count_selected = 0;
        int64_t total_selected = 0;
        for (int i = 0; i < file_count; i++) {
            if (checked[i]) {
                count_selected++;
                total_selected += files[i].size;
            }
        }

        if ((kDown & HidNpadButton_R) && count_selected > 0) {
            ui_sound_play(UI_SOUND_CONFIRM);
            int n = 0;
            for (int i = 0; i < file_count; i++) {
                if (checked[i]) out_selected_indices[n++] = files[i].file_index;
            }
            *out_selected_count = n;
            return UI_TORRENT_SELECT_OK;
        }
        if (kDown & HidNpadButton_B) {
            ui_sound_play(UI_SOUND_BACK);
            return UI_TORRENT_SELECT_CANCELED;
        }

        // ---- render ----
        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        char subtitle[64];
        char total_str[32];
        ui_format_bytes(total_selected, total_str, sizeof(total_str));
        snprintf(subtitle, sizeof(subtitle), "%d/%d archivos - %s", count_selected, file_count, total_str);
        ui_draw_frame_header(SCREEN_W, "Elegir archivos", subtitle);

        if (file_count == 0) {
            ui_draw_text(g_font_body, LEFT_EDGE, CONTENT_TOP, COLOR_TEXT_DIM, "El torrent no tiene archivos.");
        }

        for (int vi = scroll_offset; vi < file_count && vi < scroll_offset + VISIBLE_ROWS; vi++) {
            int row_index = vi - scroll_offset;
            int row_y = CONTENT_TOP + row_index * ROW_H;

            if (row_index % 2 == 1) {
                ui_draw_rect(LEFT_EDGE, row_y - 6, RIGHT_EDGE - LEFT_EDGE, ROW_H - 4, COLOR_PANEL);
            }

            draw_checkbox(LEFT_EDGE + 8, row_y - 1, checked[vi]);

            char name[160];
            truncate_to_width(g_font_body, files[vi].name, ROW_NAME_MAX_W, name, sizeof(name));
            SDL_Color text_color = checked[vi] ? COLOR_TEXT : COLOR_TEXT_DIM;
            ui_draw_text(g_font_body, ROW_NAME_X, row_y, text_color, name);

            char size_str[32];
            ui_format_bytes(files[vi].size, size_str, sizeof(size_str));
            ui_draw_text_right(g_font_small, RIGHT_EDGE, row_y + 2, COLOR_TEXT_DIM, size_str);

            if (vi == selected) {
                ui_draw_focus_border(LEFT_EDGE, row_y - 6, RIGHT_EDGE - LEFT_EDGE, ROW_H - 4, 6);
            }
        }

        ui_draw_rect(LEFT_EDGE, SCREEN_H - UI_FRAME_FOOTER_H, RIGHT_EDGE - LEFT_EDGE, 1, COLOR_SEPARATOR);
        int fy = SCREEN_H - UI_FRAME_FOOTER_H + UI_FRAME_FOOTER_H / 2 - 12;
        int fx = LEFT_EDGE;
        fx = ui_draw_button_hint(fx, fy, UI_BTN_UP_DOWN, "Navegar");
        fx = ui_draw_button_hint(fx, fy, UI_BTN_A, "Marcar");
        fx = ui_draw_button_hint(fx, fy, UI_BTN_X, "Todos");
        fx = ui_draw_button_hint(fx, fy, UI_BTN_Y, "Ninguno");
        fx = ui_draw_button_hint(fx, fy, UI_BTN_R, "Instalar seleccionados");
        ui_draw_button_hint(fx, fy, UI_BTN_B, "Cancelar");

        SDL_RenderPresent(g_renderer);
    }

    return UI_TORRENT_SELECT_CANCELED;
}
