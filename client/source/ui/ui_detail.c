#include "ui_detail.h"
#include "ui_app.h"
#include "ui_icons.h"
#include "ui_queue.h"
#include "../i18n.h"

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

// The title sits to the right of the icon and must stop short of the panel's
// right edge (the panel spans 60..SCREEN_W-60).
#define DETAIL_TITLE_X (LEFT_EDGE + DETAIL_ICON_SIZE + 24)
#define DETAIL_TITLE_MAX_W (SCREEN_W - 60 - DETAIL_TITLE_X - 20)
#define DETAIL_TITLE_MAX_LINES 2
#define DETAIL_TITLE_LINE_MAX 160
#define DETAIL_TITLE_LINE_H 36

typedef enum {
    FOCUS_MAIN = 0,
    FOCUS_DLC_LIST,
} DetailFocus;

// Truncates `text` to fit within max_w pixels, appending "..." if cut.
static void truncate_to_width(TTF_Font *font, const char *text, int max_w, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", text);
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

// Breaks `text` into at most DETAIL_TITLE_MAX_LINES lines of at most max_w
// pixels each, ellipsising the last one if anything is still left over.
// Returns how many lines were filled (always >= 1).
//
// Breaks mid-word when it has to rather than only at spaces: titles coming
// from a raw-folder source are filenames, which are frequently one long
// unbroken token ("Terraria_[v4.5.4][0100...].nsz") that no amount of
// word-wrapping would ever fit.
static int wrap_title(TTF_Font *font, const char *text, int max_w,
                       char lines[DETAIL_TITLE_MAX_LINES][DETAIL_TITLE_LINE_MAX]) {
    const char *cursor = text;
    int count = 0;

    while (*cursor != '\0' && count < DETAIL_TITLE_MAX_LINES) {
        size_t remaining = strlen(cursor);
        bool is_last_line = (count == DETAIL_TITLE_MAX_LINES - 1);

        // Longest prefix of `cursor` that still fits in max_w.
        size_t fit = 0;
        for (size_t n = 1; n <= remaining && n < DETAIL_TITLE_LINE_MAX - 4; n++) {
            char probe[DETAIL_TITLE_LINE_MAX];
            snprintf(probe, sizeof(probe), "%.*s", (int)n, cursor);
            int w = 0, h = 0;
            TTF_SizeUTF8(font, probe, &w, &h);
            if (w > max_w) break;
            fit = n;
        }
        if (fit == 0) fit = 1; // always consume something, or this loops forever

        if (fit >= remaining) {
            snprintf(lines[count], DETAIL_TITLE_LINE_MAX, "%s", cursor);
            count++;
            break;
        }

        if (is_last_line) {
            // More text than lines left - cut back a little to leave room
            // for the ellipsis rather than overflowing past max_w.
            size_t cut = fit > 3 ? fit - 3 : 1;
            snprintf(lines[count], DETAIL_TITLE_LINE_MAX, "%.*s...", (int)cut, cursor);
            count++;
            break;
        }

        // Prefer a space break, but only one near the end of the line -
        // breaking at a very early space would leave a badly ragged line.
        size_t brk = fit;
        for (size_t n = fit; n > fit / 2; n--) {
            if (cursor[n] == ' ') { brk = n; break; }
        }

        snprintf(lines[count], DETAIL_TITLE_LINE_MAX, "%.*s", (int)brk, cursor);
        count++;
        cursor += brk;
        while (*cursor == ' ') cursor++;
    }

    if (count == 0) {
        lines[0][0] = '\0';
        count = 1;
    }
    return count;
}

static const char *dlc_tag_label(const AppEntry *e) {
    return (strcmp(e->content_type, "update") == 0) ? "[UPD]" : "[DLC]";
}

// NSP, XCI, and NSZ all install natively now (see install_nsp_native.h,
// install_xci_native.h, ncz.h) - DBI is only offered as a manual fallback
// (X) for these, not the primary path.
static bool has_native_install(const AppEntry *e) {
    return e->file_type == APP_FILE_TYPE_NSP || e->file_type == APP_FILE_TYPE_XCI ||
           e->file_type == APP_FILE_TYPE_NSZ;
}

UiDetailAction ui_show_detail(const AppEntry *entry, const AppEntry *dlc_entries, int dlc_count,
                              const AppEntry **out_target) {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    // Prime the baseline so a button still held from the previous screen
    // (e.g. A held to select this entry) isn't misread as newly pressed here.
    padUpdate(&pad);

    // file_size can legitimately be unknown (0) for a raw-directory source
    // whose server never reported one - see catalog.c/try_fetch_raw_directory.
    char size_str[32];
    if (entry->file_size <= 0) {
        snprintf(size_str, sizeof(size_str), "%s", tr(STR_DETAIL_UNKNOWN_SIZE));
    } else if (entry->file_size >= 1024L * 1024L * 1024L) {
        snprintf(size_str, sizeof(size_str), "%.2f GB", entry->file_size / (1024.0 * 1024.0 * 1024.0));
    } else {
        snprintf(size_str, sizeof(size_str), "%.2f MB", entry->file_size / (1024.0 * 1024.0));
    }

    char header[300];
    snprintf(header, sizeof(header), tr(STR_DETAIL_HEADER_TEMPLATE), entry->author, entry->version, size_str);

    // Wrapped once here, not per frame - the title never changes while this
    // screen is up, and wrap_title measures character by character.
    char title_lines[DETAIL_TITLE_MAX_LINES][DETAIL_TITLE_LINE_MAX];
    int title_line_count = wrap_title(g_font_title, entry->title, DETAIL_TITLE_MAX_W, title_lines);
    // Everything stacked under the title shifts down by whatever extra
    // lines it took, so a two-line title doesn't collide with the header.
    int title_extra_y = (title_line_count - 1) * DETAIL_TITLE_LINE_H;

    DetailFocus focus = FOCUS_MAIN;
    int dlc_selected = 0;
    int dlc_scroll = 0;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        // Whichever entry the current focus acts on (base or the selected
        // DLC/update row) - what A installs and what + queues.
        const AppEntry *focused = (focus == FOCUS_DLC_LIST) ? &dlc_entries[dlc_selected] : entry;

        if (focus == FOCUS_MAIN) {
            if (kDown & HidNpadButton_A) {
                *out_target = entry;
                return UI_DETAIL_INSTALL;
            }
            if (kDown & HidNpadButton_B) {
                return UI_DETAIL_BACK;
            }
            if (has_native_install(entry) && (kDown & HidNpadButton_X)) {
                *out_target = entry;
                return UI_DETAIL_INSTALL_DBI;
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

        // + toggles the focused entry in/out of the download queue - stays
        // on this screen (with an "En cola" indicator, see below) so several
        // titles/DLCs can be queued without leaving, then installed together
        // from the queue screen. Managed here directly rather than via a
        // returned action - the queue is shared state (ui_queue.h), nothing
        // for the caller to route.
        if (kDown & HidNpadButton_Plus) {
            if (ui_queue_contains(focused->id)) {
                ui_queue_remove(focused->id);
            } else {
                ui_queue_add(focused->id);
            }
        }

        if (dlc_selected < dlc_scroll) dlc_scroll = dlc_selected;
        if (dlc_selected >= dlc_scroll + DLC_VISIBLE_ROWS) dlc_scroll = dlc_selected - DLC_VISIBLE_ROWS + 1;

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        ui_draw_rect(60, 60, SCREEN_W - 120, 560, COLOR_PANEL);

        int text_x = DETAIL_TITLE_X;
        ui_draw_rect(LEFT_EDGE, 90, DETAIL_ICON_SIZE, DETAIL_ICON_SIZE, COLOR_BG);
        SDL_Texture *icon = ui_icons_get(entry);
        if (icon) {
            SDL_Rect dst = { LEFT_EDGE, 90, DETAIL_ICON_SIZE, DETAIL_ICON_SIZE };
            SDL_RenderCopy(g_renderer, icon, NULL, &dst);
        }

        for (int i = 0; i < title_line_count; i++) {
            ui_draw_text(g_font_title, text_x, 90 + i * DETAIL_TITLE_LINE_H, COLOR_TEXT, title_lines[i]);
        }
        ui_draw_text(g_font_body, text_x, 140 + title_extra_y, COLOR_TEXT_DIM, header);

        // "En cola" indicator for whichever entry + would currently toggle.
        if (ui_queue_contains(focused->id)) {
            ui_draw_rect(text_x, 172 + title_extra_y, 90, 26, COLOR_QUEUED);
            ui_draw_text(g_font_small, text_x + 8, 175 + title_extra_y, COLOR_BG, tr(STR_DETAIL_QUEUED_BADGE));
        }

        // Decompressing an NSZ needs far more memory than any other install
        // this app does (see ncz.c) - enough that it only reliably fits in
        // applet mode. Said up front rather than only after a failed
        // attempt, since the fix is to relaunch, which means losing whatever
        // else was queued up.
        if (entry->file_type == APP_FILE_TYPE_NSZ) {
            ui_draw_text(g_font_small, text_x, 206 + title_extra_y, COLOR_TEXT_DIM,
                         tr(STR_DETAIL_NSZ_APPLET_HINT));
        }

        int y = LEFT_EDGE + DETAIL_ICON_SIZE + 30;
        y = ui_draw_text_wrapped(g_font_body, LEFT_EDGE, y, SCREEN_W - 220, 28, COLOR_TEXT, entry->description);

        if (dlc_count == 0 && entry->long_description[0] != '\0') {
            y += 16;
            ui_draw_text_wrapped(g_font_small, LEFT_EDGE, y, SCREEN_W - 220, 24, COLOR_TEXT_DIM,
                                  entry->long_description);
        }

        if (dlc_count > 0) {
            char section_header[48];
            snprintf(section_header, sizeof(section_header), tr(STR_DETAIL_DLC_SECTION_TEMPLATE), dlc_count);
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

                // Same overflow risk as the main title - clip to the row's
                // own highlight rect rather than running off the panel.
                char row_fitted[160];
                truncate_to_width(g_font_body, row_text, SCREEN_W - 240, row_fitted, sizeof(row_fitted));
                ui_draw_text(g_font_body, LEFT_EDGE, row_y, row_color, row_fitted);
            }
        }

        char hint[160];
        if (focus == FOCUS_DLC_LIST) {
            snprintf(hint, sizeof(hint), "%s", tr(STR_DETAIL_HINT_DLC_FOCUS));
        } else if (has_native_install(entry)) {
            if (dlc_count > 0) {
                snprintf(hint, sizeof(hint), tr(STR_DETAIL_HINT_NATIVE_WITH_DLC_TEMPLATE), dlc_count);
            } else {
                snprintf(hint, sizeof(hint), "%s", tr(STR_DETAIL_HINT_NATIVE));
            }
        } else if (dlc_count > 0) {
            snprintf(hint, sizeof(hint), tr(STR_DETAIL_HINT_DLC_TEMPLATE), dlc_count);
        } else {
            snprintf(hint, sizeof(hint), "%s", tr(STR_DETAIL_HINT_PLAIN));
        }
        ui_draw_text(g_font_small, LEFT_EDGE, 664, COLOR_TEXT_DIM, hint);

        // Queue hint on its own line so appending it above wouldn't push the
        // longest hint variant off the right edge (ui_draw_text doesn't wrap).
        ui_draw_text(g_font_small, LEFT_EDGE, 688, COLOR_TEXT_DIM,
                     ui_queue_contains(focused->id) ? tr(STR_DETAIL_QUEUE_REMOVE_HINT) : tr(STR_DETAIL_QUEUE_ADD_HINT));

        SDL_RenderPresent(g_renderer);
    }

    return UI_DETAIL_BACK;
}
