#include "ui_saves.h"
#include "ui_app.h"
#include "ui_icons.h"
#include "ui_sound.h"
#include "../i18n.h"
#include "../saves/save_scan.h"
#include "../saves/save_backup.h"

#include <switch.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 1280
#define SCREEN_H 720
#define LEFT_EDGE 20
#define RIGHT_EDGE (SCREEN_W - 20)
#define HEADER_Y 40
#define LIST_TOP 140
#define ROW_HEIGHT 64
#define FOOTER_Y (SCREEN_H - 46)
#define ICON_SIZE 48

// Same throttle interval as main.c's install_progress_cb, same reason: this
// callback can fire far more often than a redraw is actually useful, and a
// full render (uncached TTF rasterization + a vsync-blocking
// SDL_RenderPresent) on every single call would slow the copy itself down
// far more than it's worth.
#define SAVE_WORK_INTERVAL_NS 150000000ULL // ~6-7 times/sec

typedef struct {
    PadState *pad;
    const char *verb;  // "Haciendo backup..." / "Restaurando..."
    const char *title; // game name
    u64 last_render_tick;
} SaveProgressCtx;

// Passed as the SaveBackupProgressCallback to save_backup_create/restore -
// without this, a large save's copy blocked the screen with literally
// nothing drawn until it finished, which reads as a hang rather than work
// in progress.
static bool save_progress_cb(long total, long now, void *userdata) {
    SaveProgressCtx *ctx = (SaveProgressCtx *)userdata;

    u64 now_tick = armGetSystemTick();
    if (ctx && armTicksToNs(now_tick - ctx->last_render_tick) < SAVE_WORK_INTERVAL_NS) return true;
    if (ctx) ctx->last_render_tick = now_tick;

    bool cancel = false;
    if (ctx && ctx->pad) {
        padUpdate(ctx->pad);
        cancel = (padGetButtons(ctx->pad) & HidNpadButton_B) != 0;
    }

    SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(g_renderer);

    ui_draw_text(g_font_title, 90, 250, COLOR_TEXT, ctx ? ctx->verb : "");
    if (ctx && ctx->title && ctx->title[0]) {
        char name_line[160];
        ui_truncate_to_width(g_font_body, ctx->title, 1100, name_line, sizeof(name_line));
        ui_draw_text(g_font_body, 90, 300, COLOR_TEXT, name_line);
    }

    float pct = 0.0f;
    char now_str[32];
    ui_format_bytes(now, now_str, sizeof(now_str));
    char line[96];
    if (total > 0) {
        pct = (float)now / (float)total;
        char total_str[32];
        ui_format_bytes(total, total_str, sizeof(total_str));
        snprintf(line, sizeof(line), "%d%%   (%s / %s)", (int)(pct * 100), now_str, total_str);
    } else {
        snprintf(line, sizeof(line), "%s copiados", now_str);
    }
    ui_draw_text(g_font_body, 90, 340, COLOR_TEXT_DIM, line);
    ui_draw_progress_bar(90, 380, 1100, 7, pct, COLOR_ACCENT, COLOR_TRACK);

    int fx = 90;
    ui_draw_button_hint(fx, 430, UI_BTN_B, tr(STR_SAVES_HINT_CANCEL));

    SDL_RenderPresent(g_renderer);
    return !cancel;
}

// Runs `save_backup_create`/`save_backup_restore` (whichever `op` wraps)
// behind the progress screen above - a fresh PadState of its own (same
// reasoning as main.c's install_pad: this shouldn't disturb the calling
// screen's own button-edge-detection state), rebaselined into `caller_pad`
// afterwards since the progress screen's own renders/padUpdates leave it
// stale otherwise.
typedef bool (*SaveBackupOp)(void *op_ctx, SaveBackupProgressCallback cb, void *cb_userdata,
                              char *err_buf, size_t err_buf_size);

static bool run_with_progress(PadState *caller_pad, const char *verb, const char *title,
                               SaveBackupOp op, void *op_ctx, char *err_buf, size_t err_buf_size) {
    PadState progress_pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&progress_pad);
    padUpdate(&progress_pad);

    SaveProgressCtx ctx = { .pad = &progress_pad, .verb = verb, .title = title, .last_render_tick = 0 };
    bool ok = op(op_ctx, save_progress_cb, &ctx, err_buf, err_buf_size);

    padUpdate(caller_pad);
    return ok;
}

typedef struct {
    const SaveEntry *entry;
} BackupOpCtx;

static bool backup_op(void *op_ctx, SaveBackupProgressCallback cb, void *cb_userdata,
                       char *err_buf, size_t err_buf_size) {
    BackupOpCtx *ctx = (BackupOpCtx *)op_ctx;
    return save_backup_create(ctx->entry, cb, cb_userdata, err_buf, err_buf_size);
}

typedef struct {
    const SaveEntry *entry;
    const char *folder_name;
} RestoreOpCtx;

static bool restore_op(void *op_ctx, SaveBackupProgressCallback cb, void *cb_userdata,
                        char *err_buf, size_t err_buf_size) {
    RestoreOpCtx *ctx = (RestoreOpCtx *)op_ctx;
    return save_backup_restore(ctx->entry, ctx->folder_name, cb, cb_userdata, err_buf, err_buf_size);
}

// How many rows fit between LIST_TOP and the footer divider - anything past
// this scrolls rather than drawing off the bottom of the screen (which is
// exactly what it used to do: every entry was drawn at
// LIST_TOP + row * ROW_HEIGHT with no cap, so a console with more saves
// than fit just had the rest rendered underneath the footer and off-screen,
// invisible but still selectable).
#define VISIBLE_ROWS ((FOOTER_Y - 20 - LIST_TOP) / ROW_HEIGHT)

// Backups screen: a header panel above the list leaves it less room.
#define BACKUP_LIST_TOP 240
#define BACKUP_ROW_HEIGHT 52
#define BACKUP_VISIBLE_ROWS ((FOOTER_Y - 20 - BACKUP_LIST_TOP) / BACKUP_ROW_HEIGHT)

// Icon textures decoded from nsGetApplicationControlData, keyed by
// application id - scoped to one visit of the saves screen (freed when it
// closes), same lifetime pattern as ui_about.c's one-off QR texture. Not
// the same cache ui_icons.c uses for catalog entries: those are keyed by a
// string catalog id and fetched over HTTP; these are keyed by a u64 title
// id and fetched via a local IPC call - different enough to not share.
#define SAVES_ICON_CACHE_MAX 32
typedef struct {
    u64 application_id;
    SDL_Texture *texture; // NULL if the fetch/decode failed - still cached as "tried"
} SavesIconCacheEntry;

static SavesIconCacheEntry s_icon_cache[SAVES_ICON_CACHE_MAX];
static int s_icon_cache_count = 0;

static SDL_Texture *get_icon_texture(u64 application_id) {
    for (int i = 0; i < s_icon_cache_count; i++) {
        if (s_icon_cache[i].application_id == application_id) return s_icon_cache[i].texture;
    }
    if (s_icon_cache_count >= SAVES_ICON_CACHE_MAX) return NULL; // cache full for this visit - later rows just show no icon

    static unsigned char s_jpeg_buf[0x20000];
    size_t jpeg_len = 0;
    SDL_Texture *tex = NULL;
    if (saves_fetch_icon_jpeg(application_id, s_jpeg_buf, sizeof(s_jpeg_buf), &jpeg_len)) {
        tex = ui_icons_decode_bytes(s_jpeg_buf, jpeg_len);
    }

    SavesIconCacheEntry *slot = &s_icon_cache[s_icon_cache_count++];
    slot->application_id = application_id;
    slot->texture = tex;
    return tex;
}

static void clear_icon_cache(void) {
    for (int i = 0; i < s_icon_cache_count; i++) {
        if (s_icon_cache[i].texture) SDL_DestroyTexture(s_icon_cache[i].texture);
    }
    s_icon_cache_count = 0;
}

// folder_name is always "YYYYMMDD-HHMMSS" (see save_backup_create) - this
// codebase is the only writer, so anything else is shown as-is rather than
// risking a bad read past a shorter/malformed string.
static void format_backup_label(const char *folder_name, char *out, size_t out_size) {
    if (strlen(folder_name) != 15 || folder_name[8] != '-') {
        snprintf(out, out_size, "%s", folder_name);
        return;
    }
    char y[5], mo[3], d[3], h[3], mi[3], s[3];
    memcpy(y, folder_name, 4);      y[4] = '\0';
    memcpy(mo, folder_name + 4, 2); mo[2] = '\0';
    memcpy(d, folder_name + 6, 2);  d[2] = '\0';
    memcpy(h, folder_name + 9, 2);  h[2] = '\0';
    memcpy(mi, folder_name + 11, 2); mi[2] = '\0';
    memcpy(s, folder_name + 13, 2); s[2] = '\0';
    snprintf(out, out_size, "%s/%s/%s   %s:%s:%s", d, mo, y, h, mi, s);
}

// Keeps `selected` inside the window of rows currently drawn, scrolling by
// the minimum needed - the standard follow-the-cursor behavior ui_list.c
// uses too.
static void follow_scroll(int selected, int visible_rows, int *scroll_offset) {
    if (selected < *scroll_offset) *scroll_offset = selected;
    if (selected >= *scroll_offset + visible_rows) *scroll_offset = selected - visible_rows + 1;
    if (*scroll_offset < 0) *scroll_offset = 0;
}

// Thin vertical scrollbar on the right edge of a list, drawn only when
// there's more than fits - both a "there is more below" cue (which the
// screen had no way of showing at all before) and a position readout.
static void draw_scrollbar(int top, int visible_rows, int row_h, int count, int scroll_offset) {
    if (count <= visible_rows) return;

    int track_h = visible_rows * row_h;
    int track_x = RIGHT_EDGE - 6;
    ui_draw_rounded_rect(track_x, top, 4, track_h, 2, COLOR_PANEL);

    int thumb_h = track_h * visible_rows / count;
    if (thumb_h < 24) thumb_h = 24;
    int max_scroll = count - visible_rows;
    int thumb_y = top + (track_h - thumb_h) * scroll_offset / max_scroll;
    ui_draw_rounded_rect(track_x, thumb_y, 4, thumb_h, 2, COLOR_ACCENT);
}

// ---- Backups screen for one save: A restores (confirm first - it
// overwrites the live save), X deletes a backup (confirm first), Y makes a
// new backup right now, B goes back to the save list. ----
static void show_backups_screen(const SaveEntry *entry) {
    SaveBackupEntry backups[SAVE_BACKUP_MAX];
    int count = save_backup_list(entry, backups, SAVE_BACKUP_MAX);
    int selected = 0;
    int scroll_offset = 0;

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    padUpdate(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (selected >= count) selected = count - 1;
        if (selected < 0) selected = 0;

        if ((kDown & HidNpadButton_Down) && selected < count - 1) {
            selected++;
            ui_sound_play(UI_SOUND_NAVIGATE);
        }
        if ((kDown & HidNpadButton_Up) && selected > 0) {
            selected--;
            ui_sound_play(UI_SOUND_NAVIGATE);
        }
        follow_scroll(selected, BACKUP_VISIBLE_ROWS, &scroll_offset);

        if (kDown & HidNpadButton_Y) {
            char msg[300];
            snprintf(msg, sizeof(msg), tr(STR_SAVES_BACKUP_NOW_CONFIRM_TEMPLATE), entry->name);
            if (ui_app_show_confirm(msg)) {
                char err[128];
                BackupOpCtx op_ctx = { .entry = entry };
                if (run_with_progress(&pad, tr(STR_SAVES_BACKING_UP), entry->name, backup_op, &op_ctx,
                                       err, sizeof(err))) {
                    ui_app_show_message(tr(STR_SAVES_BACKUP_DONE));
                } else {
                    char fail[220];
                    snprintf(fail, sizeof(fail), tr(STR_SAVES_BACKUP_FAILED_TEMPLATE), err);
                    ui_app_show_message(fail);
                }
                count = save_backup_list(entry, backups, SAVE_BACKUP_MAX);
                selected = 0;
                scroll_offset = 0;
            }
            padUpdate(&pad); // the confirm/message dialogs take over the applet - rebaseline held buttons
        }

        if ((kDown & HidNpadButton_A) && count > 0) {
            char label[64];
            format_backup_label(backups[selected].folder_name, label, sizeof(label));
            char msg[300];
            snprintf(msg, sizeof(msg), tr(STR_SAVES_RESTORE_CONFIRM_TEMPLATE), label);
            if (ui_app_show_confirm(msg)) {
                char err[128];
                RestoreOpCtx op_ctx = { .entry = entry, .folder_name = backups[selected].folder_name };
                if (run_with_progress(&pad, tr(STR_SAVES_RESTORING), entry->name, restore_op, &op_ctx,
                                       err, sizeof(err))) {
                    ui_app_show_message(tr(STR_SAVES_RESTORE_DONE));
                } else {
                    char fail[220];
                    snprintf(fail, sizeof(fail), tr(STR_SAVES_RESTORE_FAILED_TEMPLATE), err);
                    ui_app_show_message(fail);
                }
            }
            padUpdate(&pad);
        }

        if ((kDown & HidNpadButton_X) && count > 0) {
            char label[64];
            format_backup_label(backups[selected].folder_name, label, sizeof(label));
            char msg[200];
            snprintf(msg, sizeof(msg), tr(STR_SAVES_DELETE_CONFIRM_TEMPLATE), label);
            if (ui_app_show_confirm(msg)) {
                save_backup_delete(entry, backups[selected].folder_name);
                count = save_backup_list(entry, backups, SAVE_BACKUP_MAX);
                if (selected >= count) selected = count - 1;
                follow_scroll(selected < 0 ? 0 : selected, BACKUP_VISIBLE_ROWS, &scroll_offset);
            }
            padUpdate(&pad);
        }

        if (kDown & HidNpadButton_B) {
            ui_sound_play(UI_SOUND_BACK);
            return;
        }

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, tr(STR_SAVES_BACKUPS_TITLE));

        // Header panel: the game this screen is about, shown the way the
        // save list shows it (icon + title + profile) so it's unambiguous
        // which title a restore is about to overwrite, plus a running
        // count/total of what's backed up.
        ui_draw_rounded_rect(LEFT_EDGE, 100, RIGHT_EDGE - LEFT_EDGE, 112, 8, COLOR_PANEL);

        SDL_Texture *icon = get_icon_texture(entry->application_id);
        int text_x = LEFT_EDGE + 24;
        if (icon) {
            SDL_Rect dst = { LEFT_EDGE + 20, 118, 76, 76 };
            SDL_RenderCopy(g_renderer, icon, NULL, &dst);
            text_x = LEFT_EDGE + 20 + 76 + 20;
        }

        char title_line[96];
        ui_truncate_to_width(g_font_title, entry->name, RIGHT_EDGE - text_x - 260, title_line, sizeof(title_line));
        ui_draw_text(g_font_title, text_x, 122, COLOR_TEXT, title_line);

        if (entry->nickname[0] != '\0') {
            ui_draw_text(g_font_small, text_x, 158, COLOR_TEXT_DIM, entry->nickname);
        }

        char save_size[32];
        ui_format_bytes(entry->size, save_size, sizeof(save_size));
        char size_line[64];
        snprintf(size_line, sizeof(size_line), tr(STR_SAVES_LIVE_SIZE_TEMPLATE), save_size);
        ui_draw_text(g_font_small, text_x, 180, COLOR_TEXT_DIM, size_line);

        char count_line[48];
        snprintf(count_line, sizeof(count_line), tr(STR_SAVES_BACKUP_COUNT_TEMPLATE), count);
        ui_draw_text_right(g_font_body, RIGHT_EDGE - 24, 130, COLOR_ACCENT, count_line);

        if (count == 0) {
            ui_draw_text(g_font_body, LEFT_EDGE + 24, BACKUP_LIST_TOP + 20, COLOR_TEXT_DIM,
                         tr(STR_SAVES_NO_BACKUPS));
        }

        for (int row = scroll_offset; row < count && row < scroll_offset + BACKUP_VISIBLE_ROWS; row++) {
            int row_y = BACKUP_LIST_TOP + (row - scroll_offset) * BACKUP_ROW_HEIGHT;
            bool is_selected = (row == selected);

            if (row % 2 == 1) {
                ui_draw_rounded_rect(LEFT_EDGE, row_y, RIGHT_EDGE - LEFT_EDGE - 14, BACKUP_ROW_HEIGHT - 6,
                                      6, COLOR_PANEL);
            }

            // Normal colors regardless of focus - the Borealis-style focus
            // border drawn after this row's content marks the selection.
            SDL_Color text_color = COLOR_TEXT;
            SDL_Color dim_color = COLOR_TEXT_DIM;

            char label[64];
            format_backup_label(backups[row].folder_name, label, sizeof(label));
            ui_draw_text(g_font_body, LEFT_EDGE + 20, row_y + 11, text_color, label);

            // The newest backup is always row 0 (save_backup_list sorts
            // newest-first) - worth calling out, since "which one do I
            // restore" is otherwise a date-comparison exercise.
            if (row == 0) {
                ui_draw_text(g_font_small, LEFT_EDGE + 300, row_y + 15, dim_color, tr(STR_SAVES_MOST_RECENT));
            }

            char size_str[32];
            ui_format_bytes(backups[row].total_size, size_str, sizeof(size_str));
            ui_draw_text_right(g_font_body, RIGHT_EDGE - 34, row_y + 11, dim_color, size_str);

            if (is_selected) {
                ui_draw_focus_border(LEFT_EDGE, row_y, RIGHT_EDGE - LEFT_EDGE - 14,
                                     BACKUP_ROW_HEIGHT - 6, 6);
            }
        }

        draw_scrollbar(BACKUP_LIST_TOP, BACKUP_VISIBLE_ROWS, BACKUP_ROW_HEIGHT, count, scroll_offset);

        ui_draw_rect(LEFT_EDGE, FOOTER_Y - 10, RIGHT_EDGE - LEFT_EDGE, 1, COLOR_SEPARATOR);
        int fx = LEFT_EDGE;
        fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_UP_DOWN, tr(STR_SAVES_HINT_NAVIGATE));
        fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_A, tr(STR_SAVES_HINT_RESTORE));
        fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_X, tr(STR_SAVES_HINT_DELETE));
        fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_Y, tr(STR_SAVES_HINT_BACKUP_NOW));
        ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_B, tr(STR_SAVES_HINT_BACK));

        SDL_RenderPresent(g_renderer);
    }
}

void ui_show_saves(void) {
    // SAVES_MAX entries at ~0x230 bytes each (~140KB total) - static so it
    // doesn't sit on the stack, same reasoning as ncm_cleanup.c's
    // meta_keys[MAX_META_KEYS].
    static SaveEntry entries[SAVES_MAX];

    // One-frame "scanning" splash before the blocking scan - resolving up
    // to SAVES_MAX titles' control data over IPC isn't instant, and a
    // frozen frame with no feedback reads as a hang.
    SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(g_renderer);
    ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, tr(STR_SAVES_TITLE));
    ui_draw_text(g_font_body, LEFT_EDGE, LIST_TOP, COLOR_TEXT_DIM, tr(STR_SAVES_SCANNING));
    SDL_RenderPresent(g_renderer);

    char scan_err[160];
    int count = saves_scan(entries, SAVES_MAX, scan_err, sizeof(scan_err));
    bool scan_failed = (count == 0 && scan_err[0] != '\0');

    int selected = 0;
    int scroll_offset = 0;

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    padUpdate(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (selected >= count) selected = count - 1;
        if (selected < 0) selected = 0;

        if ((kDown & HidNpadButton_Down) && selected < count - 1) {
            selected++;
            ui_sound_play(UI_SOUND_NAVIGATE);
        }
        if ((kDown & HidNpadButton_Up) && selected > 0) {
            selected--;
            ui_sound_play(UI_SOUND_NAVIGATE);
        }
        follow_scroll(selected, VISIBLE_ROWS, &scroll_offset);

        if ((kDown & HidNpadButton_A) && count > 0) {
            ui_sound_play(UI_SOUND_CONFIRM);
            show_backups_screen(&entries[selected]);
            padUpdate(&pad);
        }

        if ((kDown & HidNpadButton_Y) && count > 0) {
            char msg[300];
            snprintf(msg, sizeof(msg), tr(STR_SAVES_BACKUP_NOW_CONFIRM_TEMPLATE), entries[selected].name);
            if (ui_app_show_confirm(msg)) {
                char err[128];
                BackupOpCtx op_ctx = { .entry = &entries[selected] };
                if (run_with_progress(&pad, tr(STR_SAVES_BACKING_UP), entries[selected].name, backup_op,
                                       &op_ctx, err, sizeof(err))) {
                    ui_app_show_message(tr(STR_SAVES_BACKUP_DONE));
                } else {
                    char fail[220];
                    snprintf(fail, sizeof(fail), tr(STR_SAVES_BACKUP_FAILED_TEMPLATE), err);
                    ui_app_show_message(fail);
                }
            }
            padUpdate(&pad);
        }

        if (kDown & (HidNpadButton_B | HidNpadButton_Plus)) {
            ui_sound_play(UI_SOUND_BACK);
            break;
        }

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, tr(STR_SAVES_TITLE));
        if (count > 0) {
            char subtitle[64];
            snprintf(subtitle, sizeof(subtitle), tr(STR_SAVES_COUNT_TEMPLATE), count);
            ui_draw_text_right(g_font_body, RIGHT_EDGE, HEADER_Y + 6, COLOR_TEXT_DIM, subtitle);
        }

        if (count == 0) {
            ui_draw_text(g_font_body, LEFT_EDGE, LIST_TOP, COLOR_TEXT_DIM,
                         scan_failed ? scan_err : tr(STR_SAVES_NONE_FOUND));
        }

        for (int row = scroll_offset; row < count && row < scroll_offset + VISIBLE_ROWS; row++) {
            int row_y = LIST_TOP + (row - scroll_offset) * ROW_HEIGHT;
            bool is_selected = (row == selected);

            if (row % 2 == 1) {
                ui_draw_rounded_rect(LEFT_EDGE, row_y - 8, RIGHT_EDGE - LEFT_EDGE - 14, ROW_HEIGHT - 6,
                                      8, COLOR_PANEL);
            }

            // Normal colors regardless of focus - the Borealis-style focus
            // border drawn after this row's content marks the selection.
            SDL_Color text_color = COLOR_TEXT;
            SDL_Color dim_color = COLOR_TEXT_DIM;

            SDL_Texture *icon = get_icon_texture(entries[row].application_id);
            int text_x = LEFT_EDGE + 20;
            if (icon) {
                SDL_Rect dst = { LEFT_EDGE + 12, row_y - 4, ICON_SIZE, ICON_SIZE };
                SDL_RenderCopy(g_renderer, icon, NULL, &dst);
                text_x = LEFT_EDGE + 12 + ICON_SIZE + 16;
            }

            char title_line[96];
            ui_truncate_to_width(g_font_body, entries[row].name, 560, title_line, sizeof(title_line));
            ui_draw_text(g_font_body, text_x, row_y, text_color, title_line);

            if (entries[row].nickname[0] != '\0') {
                ui_draw_text(g_font_small, text_x, row_y + 26, dim_color, entries[row].nickname);
            }

            char size_str[32];
            ui_format_bytes(entries[row].size, size_str, sizeof(size_str));
            ui_draw_text_right(g_font_body, RIGHT_EDGE - 34, row_y + 4, dim_color, size_str);

            if (is_selected) {
                ui_draw_focus_border(LEFT_EDGE, row_y - 8, RIGHT_EDGE - LEFT_EDGE - 14, ROW_HEIGHT - 6, 8);
            }
        }

        draw_scrollbar(LIST_TOP - 8, VISIBLE_ROWS, ROW_HEIGHT, count, scroll_offset);

        ui_draw_rect(LEFT_EDGE, FOOTER_Y - 10, RIGHT_EDGE - LEFT_EDGE, 1, COLOR_SEPARATOR);
        int fx = LEFT_EDGE;
        fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_UP_DOWN, tr(STR_SAVES_HINT_NAVIGATE));
        fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_A, tr(STR_SAVES_HINT_VIEW_BACKUPS));
        fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_Y, tr(STR_SAVES_HINT_BACKUP_NOW));
        ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_B, tr(STR_SAVES_HINT_BACK));

        SDL_RenderPresent(g_renderer);
    }

    clear_icon_cache();
}
