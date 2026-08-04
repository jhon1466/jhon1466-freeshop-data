#include "ui_detail.h"
#include "ui_app.h"
#include "ui_icons.h"
#include "ui_queue.h"
#include "ui_sound.h"
#include "ui_fx.h"
#include "../i18n.h"

#include <switch.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 1280
#define SCREEN_H 720

// pipensx's own game detail screen (src/ui/detail/game_detail.hpp, read
// directly from https://github.com/i3sey/pipensx) is a two-column
// eShop-style layout: a fixed-width left column (cover, install button,
// secondary actions, status), and a right column (facts table, screenshots,
// description). This mirrors that structure with the facts we actually
// have on AppEntry - no Title ID/Publisher/Genre/Players/screenshots, since
// nothing in this catalog's schema carries them (see app_entry.h) - laid
// out statically instead of in a ScrollingFrame, matching how every other
// screen in this app already fits fixed content into the fixed 1280x720
// frame rather than scrolling.
#define PANEL_X 60
#define PANEL_Y 60
#define PANEL_W (SCREEN_W - 120)
#define PANEL_H 560

#define COL_PAD 40
#define LEFT_COL_X (PANEL_X + COL_PAD)
#define LEFT_COL_W 320
#define COL_GAP 32
#define RIGHT_COL_X (LEFT_COL_X + LEFT_COL_W + COL_GAP)
#define RIGHT_COL_W ((PANEL_X + PANEL_W - COL_PAD) - RIGHT_COL_X)
#define CONTENT_TOP (PANEL_Y + 24)
#define CONTENT_BOTTOM (PANEL_Y + PANEL_H - 24)

#define COVER_W LEFT_COL_W
#define COVER_H 200

#define INSTALL_BTN_H 64
#define INSTALL_BTN_GAP 16
#define SECONDARY_ROW_GAP 14
#define SECONDARY_ROW_H 30
#define STATUS_LINE_GAP 10

#define FACT_ROW_H 26
#define FACT_LABEL_W 120
#define FACT_COUNT 5

#define DLC_ROW_HEIGHT 30
#define DLC_VISIBLE_ROWS 4
// Header line + rows + a little breathing room above it - reserved from the
// bottom of the right column whenever there's a DLC/update list to show, so
// the description above it never has to guess how much room is left.
#define DLC_SECTION_RESERVED (30 + DLC_VISIBLE_ROWS * DLC_ROW_HEIGHT + 10)

#define DETAIL_TITLE_X RIGHT_COL_X
#define DETAIL_TITLE_MAX_W RIGHT_COL_W
#define DETAIL_TITLE_MAX_LINES 2
#define DETAIL_TITLE_LINE_MAX 160
#define DETAIL_TITLE_LINE_H 36

// Footer button-hint row sits below the panel, aligned to the panel's own
// left edge rather than the page's outer margin.
#define FOOTER_HINT_X PANEL_X

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
// (X) for these, not the primary path. Torrent-catalog entries (via_torrent
// - see sources.h) are excluded even when their file_type matches: the DBI
// hand-off (install_nsp_and_launch_dbi) fetches entry->download_url over
// HTTP, which is a magnet: URI here, not something DBI or that hand-off
// can use.
static bool has_native_install(const AppEntry *e) {
    return !e->via_torrent &&
           (e->file_type == APP_FILE_TYPE_NSP || e->file_type == APP_FILE_TYPE_XCI ||
            e->file_type == APP_FILE_TYPE_NSZ);
}

static const char *file_type_label(AppFileType type) {
    if (type == APP_FILE_TYPE_NSP) return "NSP";
    if (type == APP_FILE_TYPE_XCI) return "XCI";
    if (type == APP_FILE_TYPE_NSZ) return "NSZ";
    if (type == APP_FILE_TYPE_PORT) return "Port";
    return "NRO";
}

// One label/value row of the right column's facts table - dim label in a
// fixed-width first column, normal-weight value beside it, matching
// pipensx's own Facts Table row style (dim key, brighter value).
static void draw_fact_row(int x, int y, const char *label, const char *value) {
    ui_draw_text(g_font_small, x, y, COLOR_TEXT_DIM, label);
    ui_draw_text(g_font_small, x + FACT_LABEL_W, y, COLOR_TEXT, value);
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

    // Wrapped once here, not per frame - the title never changes while this
    // screen is up, and wrap_title measures character by character. Every Y
    // position below the title (facts table, description, DLC list) is
    // computed by chaining off facts_y/facts_bottom rather than a fixed
    // offset, so a two-line title just naturally pushes everything under it
    // down instead of needing a separate "extra height" correction applied
    // a second time somewhere else.
    char title_lines[DETAIL_TITLE_MAX_LINES][DETAIL_TITLE_LINE_MAX];
    int title_line_count = wrap_title(g_font_title, entry->title, DETAIL_TITLE_MAX_W, title_lines);

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
                ui_sound_play(UI_SOUND_CONFIRM);
                return UI_DETAIL_INSTALL;
            }
            if (kDown & HidNpadButton_B) {
                ui_sound_play(UI_SOUND_BACK);
                return UI_DETAIL_BACK;
            }
            if (has_native_install(entry) && (kDown & HidNpadButton_X)) {
                *out_target = entry;
                ui_sound_play(UI_SOUND_CONFIRM);
                return UI_DETAIL_INSTALL_DBI;
            }
            if (dlc_count > 0 && (kDown & HidNpadButton_Y)) {
                focus = FOCUS_DLC_LIST;
                ui_sound_play(UI_SOUND_NAVIGATE);
            }
        } else {
            int dlc_before = dlc_selected;
            if (kDown & HidNpadButton_Down) {
                if (dlc_selected < dlc_count - 1) dlc_selected++;
            }
            if (kDown & HidNpadButton_Up) {
                if (dlc_selected > 0) dlc_selected--;
            }
            if (dlc_selected != dlc_before) ui_sound_play(UI_SOUND_NAVIGATE);

            if (kDown & HidNpadButton_A) {
                *out_target = &dlc_entries[dlc_selected];
                ui_sound_play(UI_SOUND_CONFIRM);
                return UI_DETAIL_INSTALL;
            }
            if (kDown & HidNpadButton_B) {
                focus = FOCUS_MAIN;
                ui_sound_play(UI_SOUND_BACK);
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
                ui_sound_play(UI_SOUND_BACK);
            } else {
                ui_queue_add(focused->id);
                ui_sound_play(UI_SOUND_CONFIRM);
            }
        }

        if (dlc_selected < dlc_scroll) dlc_scroll = dlc_selected;
        if (dlc_selected >= dlc_scroll + DLC_VISIBLE_ROWS) dlc_scroll = dlc_selected - DLC_VISIBLE_ROWS + 1;

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);
        ui_fx_draw_background();

        ui_draw_rounded_rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 12, COLOR_PANEL);

        // ---- Left column: cover, install button, secondary actions, status ----

        ui_draw_rect(LEFT_COL_X, CONTENT_TOP, COVER_W, COVER_H, COLOR_BG);
        SDL_Texture *icon = ui_icons_get(entry);
        if (icon) {
            // Our covers are square; pipensx's cover plate is wide (320x200) -
            // fit the square inside it without stretching, same as
            // pipensx's own ImageScalingType::FIT, rather than distorting it
            // to fill the whole plate.
            int icon_size = COVER_H;
            int icon_x = LEFT_COL_X + (COVER_W - icon_size) / 2;
            SDL_Rect dst = { icon_x, CONTENT_TOP, icon_size, icon_size };
            SDL_RenderCopy(g_renderer, icon, NULL, &dst);
        }
        ui_mask_rounded_corners(LEFT_COL_X, CONTENT_TOP, COVER_W, COVER_H, 12, COLOR_PANEL);

        int install_btn_y = CONTENT_TOP + COVER_H + INSTALL_BTN_GAP;
        ui_draw_rounded_rect(LEFT_COL_X, install_btn_y, LEFT_COL_W, INSTALL_BTN_H, 8, COLOR_ACCENT);
        {
            const char *install_label = tr(STR_DETAIL_INSTALL_BUTTON);
            int w = 0, h = 0;
            TTF_SizeUTF8(g_font_body, install_label, &w, &h);
            ui_draw_text(g_font_body, LEFT_COL_X + (LEFT_COL_W - w) / 2,
                         install_btn_y + (INSTALL_BTN_H - h) / 2, COLOR_BG, install_label);
        }

        int secondary_y = install_btn_y + INSTALL_BTN_H + SECONDARY_ROW_GAP;
        {
            int hx = LEFT_COL_X;
            hx = ui_draw_button_hint(hx, secondary_y, UI_BTN_PLUS,
                                     ui_queue_contains(focused->id) ? tr(STR_DETAIL_QUEUE_REMOVE_HINT)
                                                                     : tr(STR_DETAIL_QUEUE_ADD_HINT));
            if (focus == FOCUS_MAIN && has_native_install(entry)) {
                ui_draw_button_hint(hx, secondary_y, UI_BTN_X, tr(STR_DETAIL_HINT_INSTALL_DBI));
            }
        }

        // "En cola" indicator for whichever entry + would currently toggle -
        // its own row so the secondary actions row above doesn't shift
        // depending on queue state.
        int status_y = secondary_y + SECONDARY_ROW_H + STATUS_LINE_GAP;
        if (ui_queue_contains(focused->id)) {
            ui_draw_rounded_rect(LEFT_COL_X, status_y, 90, 26, 6, COLOR_QUEUED);
            ui_draw_text(g_font_small, LEFT_COL_X + 8, status_y + 3, COLOR_BG, tr(STR_DETAIL_QUEUED_BADGE));
            status_y += 26 + 8;
        }

        // Decompressing an NSZ needs far more memory than any other install
        // this app does (see ncz.c) - enough that it only reliably fits in
        // applet mode. Said up front rather than only after a failed
        // attempt, since the fix is to relaunch, which means losing whatever
        // else was queued up.
        if (entry->file_type == APP_FILE_TYPE_NSZ) {
            ui_draw_text_wrapped(g_font_small, LEFT_COL_X, status_y, LEFT_COL_W, 20, COLOR_TEXT_DIM,
                                 tr(STR_DETAIL_NSZ_APPLET_HINT));
        }

        // ---- Right column: title, facts table, description, DLC list ----

        for (int i = 0; i < title_line_count; i++) {
            ui_draw_text(g_font_title, DETAIL_TITLE_X, CONTENT_TOP + i * DETAIL_TITLE_LINE_H,
                         COLOR_TEXT, title_lines[i]);
        }

        int facts_y = CONTENT_TOP + title_line_count * DETAIL_TITLE_LINE_H + 8;
        // Torrent-catalog entries carry a release year in `version`, not an
        // app version string (see catalog.c's copy_year_field) - "Versión:
        // v2019" reads as a typo, "Año: 2019" reads as what it is.
        draw_fact_row(RIGHT_COL_X, facts_y, tr(STR_DETAIL_FACT_AUTHOR), entry->author);
        draw_fact_row(RIGHT_COL_X, facts_y + FACT_ROW_H, tr(STR_LIST_COL_CATEGORY), entry->category);
        draw_fact_row(RIGHT_COL_X, facts_y + FACT_ROW_H * 2, tr(STR_LIST_COL_TYPE), file_type_label(entry->file_type));
        draw_fact_row(RIGHT_COL_X, facts_y + FACT_ROW_H * 3,
                     tr(entry->via_torrent ? STR_DETAIL_FACT_YEAR : STR_LIST_COL_VERSION), entry->version);
        draw_fact_row(RIGHT_COL_X, facts_y + FACT_ROW_H * 4, tr(STR_LIST_COL_SIZE), size_str);
        int facts_bottom = facts_y + FACT_ROW_H * FACT_COUNT;

        int desc_y = facts_bottom + 16;
        int desc_bottom = CONTENT_BOTTOM - (dlc_count > 0 ? DLC_SECTION_RESERVED : 0);

        int y = ui_draw_text_wrapped(g_font_body, RIGHT_COL_X, desc_y, RIGHT_COL_W, 26, COLOR_TEXT,
                                     entry->description);
        if (dlc_count == 0 && entry->long_description[0] != '\0' && y < desc_bottom) {
            y += 12;
            ui_draw_text_wrapped(g_font_small, RIGHT_COL_X, y, RIGHT_COL_W, 22, COLOR_TEXT_DIM,
                                 entry->long_description);
        }

        if (dlc_count > 0) {
            int dlc_section_y = CONTENT_BOTTOM - DLC_SECTION_RESERVED;
            char section_header[48];
            snprintf(section_header, sizeof(section_header), tr(STR_DETAIL_DLC_SECTION_TEMPLATE), dlc_count);
            ui_draw_text(g_font_body, RIGHT_COL_X, dlc_section_y, COLOR_TEXT, section_header);

            int dlc_list_top = dlc_section_y + 30;
            for (int i = dlc_scroll; i < dlc_count && i < dlc_scroll + DLC_VISIBLE_ROWS; i++) {
                int row_index = i - dlc_scroll;
                int row_y = dlc_list_top + row_index * DLC_ROW_HEIGHT;
                bool is_selected = (focus == FOCUS_DLC_LIST && i == dlc_selected);

                char row_text[160];
                snprintf(row_text, sizeof(row_text), "%s %s", dlc_tag_label(&dlc_entries[i]), dlc_entries[i].title);

                // Same overflow risk as the main title - clip to the row's
                // own highlight rect rather than running off the panel.
                char row_fitted[160];
                truncate_to_width(g_font_small, row_text, RIGHT_COL_W - 20, row_fitted, sizeof(row_fitted));
                // Normal color regardless of focus - the Borealis-style
                // focus border below marks the selection.
                ui_draw_text(g_font_small, RIGHT_COL_X, row_y, COLOR_TEXT, row_fitted);

                if (is_selected) {
                    ui_draw_focus_border(RIGHT_COL_X - 10, row_y - 4, RIGHT_COL_W + 10,
                                         DLC_ROW_HEIGHT - 4, 6);
                }
            }
        }

        int hint_x = FOOTER_HINT_X;
        if (focus == FOCUS_DLC_LIST) {
            hint_x = ui_draw_button_hint(hint_x, 664, UI_BTN_UP_DOWN, tr(STR_DETAIL_HINT_CHOOSE));
            hint_x = ui_draw_button_hint(hint_x, 664, UI_BTN_A, tr(STR_DETAIL_HINT_INSTALL_SELECTED));
            ui_draw_button_hint(hint_x, 664, UI_BTN_B, tr(STR_DETAIL_HINT_BACK_TO_GAME));
        } else {
            hint_x = ui_draw_button_hint(hint_x, 664, UI_BTN_A, tr(STR_DETAIL_HINT_INSTALL));
            if (dlc_count > 0) {
                char dlc_hint[48];
                snprintf(dlc_hint, sizeof(dlc_hint), tr(STR_DETAIL_HINT_DLC_TEMPLATE), dlc_count);
                hint_x = ui_draw_button_hint(hint_x, 664, UI_BTN_Y, dlc_hint);
            }
            ui_draw_button_hint(hint_x, 664, UI_BTN_B, tr(STR_ABOUT_HINT_BACK));
        }

        SDL_RenderPresent(g_renderer);
    }

    return UI_DETAIL_BACK;
}
