#include "ui_ftp.h"
#include "ui_app.h"
#include "ui_sound.h"
#include "../i18n.h"
#include "../ftp/ftp_server.h"

#include <switch.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 1280
#define SCREEN_H 720
#define LEFT_EDGE 60
#define RIGHT_EDGE (SCREEN_W - 60)
#define CONTENT_W (RIGHT_EDGE - LEFT_EDGE)
#define HEADER_Y 46
#define FOOTER_Y (SCREEN_H - 46)

// Same throttle interval as ui_mtp.c's own progress callback - see that
// comment for why.
#define FTP_WORK_INTERVAL_NS 150000000ULL

// Same red ui_queue.c/ui_mtp.c each define for their own failed rows -
// matches COLOR_DANGER in ui_app.h (pipensx's dark-mode "Error").
#define COLOR_ERROR ((SDL_Color){0xff, 0x45, 0x54, 0xff})

#define QUEUE_ROW_H 36

static void draw_text_fitted(TTF_Font *font, int x, int y, int max_w, SDL_Color color, const char *text) {
    char buf[320];
    ui_truncate_to_width(font, text, max_w, buf, sizeof(buf));
    ui_draw_text(font, x, y, color, buf);
}

typedef struct {
    PadState *pad;
    u64 last_render_tick;
    const FtpState *state;
} FtpProgressCtx;

// Draws the panel every "something is happening" state shares - see
// ui_mtp.c's draw_busy_panel, which this mirrors exactly for visual
// consistency between the two transfer methods.
static void draw_busy_panel(const char *title, const char *filename, const char *detail, float pct) {
    SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(g_renderer);

    int y = 232;
    ui_draw_text(g_font_body, LEFT_EDGE, y, COLOR_TEXT_DIM, title);
    y += 42;

    if (filename && filename[0] != '\0') {
        draw_text_fitted(g_font_title, LEFT_EDGE, y, CONTENT_W, COLOR_TEXT, filename);
        y += 56;
    }

    if (detail && detail[0] != '\0') {
        ui_draw_text(g_font_body, LEFT_EDGE, y, COLOR_TEXT_DIM, detail);
    }
    y += 46;

    if (pct >= 0.0f) {
        ui_draw_progress_bar(LEFT_EDGE, y, CONTENT_W, 7, pct, COLOR_ACCENT, COLOR_TRACK);
    }
}

// Passed as ftp_step's InstallProgressCallback - fires during a file
// transfer and, for an installable upload, the commit phase right after
// (see ftp_server.c's handle_stor) - the only rendering that happens while
// either is in progress.
static bool ftp_progress_cb(long total, long now, void *userdata) {
    FtpProgressCtx *ctx = (FtpProgressCtx *)userdata;
    const FtpState *state = ctx ? ctx->state : NULL;
    bool committing = state && state->status == FTP_STATUS_INSTALLING;

    u64 now_tick = armGetSystemTick();
    if (!committing && ctx && armTicksToNs(now_tick - ctx->last_render_tick) < FTP_WORK_INTERVAL_NS) return true;
    if (ctx) ctx->last_render_tick = now_tick;

    bool cancel = false;
    if (!committing && ctx && ctx->pad) {
        padUpdate(ctx->pad);
        cancel = (padGetButtons(ctx->pad) & HidNpadButton_B) != 0;
    }

    const char *filename = state ? state->current_file : NULL;

    if (committing) {
        draw_busy_panel(tr(STR_FTP_INSTALLING_NOW), filename, NULL, -1.0f);
        SDL_RenderPresent(g_renderer);
        return true;
    }

    char now_str[32];
    ui_format_bytes(now, now_str, sizeof(now_str));
    char detail[96];
    float pct = -1.0f;

    if (total > 0) {
        pct = (float)now / (float)total;
        char total_str[32];
        ui_format_bytes(total, total_str, sizeof(total_str));
        snprintf(detail, sizeof(detail), "%s / %s   (%d%%)", now_str, total_str, (int)(pct * 100));
    } else {
        // Uploads (STOR) never have a known total - FTP has no equivalent
        // of declaring a size upfront (see ftp_server.c's handle_stor) - so
        // this is the common case for that direction, not just a fallback.
        snprintf(detail, sizeof(detail), "%s", now_str);
    }

    draw_busy_panel(tr(STR_FTP_TRANSFERRING), filename, detail, pct);

    if (state && state->history_count > 0) {
        char count_line[64];
        snprintf(count_line, sizeof(count_line), tr(STR_FTP_SESSION_COUNT_TEMPLATE), state->history_count);
        ui_draw_text(g_font_small, LEFT_EDGE, 400, COLOR_TEXT_DIM, count_line);
    }

    ui_draw_button_hint(LEFT_EDGE, 470, UI_BTN_B, tr(STR_SAVES_HINT_CANCEL));

    SDL_RenderPresent(g_renderer);
    return !cancel;
}

// One row of the session's history - same visual language as ui_mtp.c's
// draw_history_row (and, one level further back, ui_queue.c's download
// rows), just with two extra outcomes (Uploaded/Downloaded) FTP can produce
// that MTP never does.
static void draw_history_row(int y, const FtpHistoryItem *item) {
    const char *label;
    SDL_Color color;
    switch (item->status) {
        case FTP_HISTORY_INSTALLED:   label = tr(STR_QUEUE_STATUS_INSTALLED);  color = COLOR_QUEUED;    break;
        case FTP_HISTORY_UPLOADED:    label = tr(STR_FTP_STATUS_UPLOADED);     color = COLOR_TEXT;      break;
        case FTP_HISTORY_DOWNLOADED:  label = tr(STR_FTP_STATUS_DOWNLOADED);   color = COLOR_TEXT;      break;
        default:                      label = tr(STR_QUEUE_STATUS_ERROR);      color = COLOR_ERROR;     break;
    }

    int label_w = 0, label_h = 0;
    TTF_SizeUTF8(g_font_small, label, &label_w, &label_h);
    draw_text_fitted(g_font_small, LEFT_EDGE, y, CONTENT_W - label_w - 24, COLOR_TEXT, item->filename);
    ui_draw_text_right(g_font_small, RIGHT_EDGE, y, color, label);
}

// The idle screen: the address to connect to, what state the link is in,
// and a running list of this session's activity - see ui_mtp.c's draw_idle,
// which this mirrors closely on purpose (the two screens are meant to feel
// like the same feature over two different transports).
static void draw_idle(const FtpState *state) {
    SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(g_renderer);

    ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, tr(STR_FTP_TITLE));
    ui_draw_rect(LEFT_EDGE, HEADER_Y + 54, CONTENT_W, 1, COLOR_PANEL);

    bool ready = state->status == FTP_STATUS_LISTENING || state->status == FTP_STATUS_CONNECTED;
    char status_buf[96];
    const char *status_text;
    const char *status_help;
    switch (state->status) {
        case FTP_STATUS_WAITING_FOR_NETWORK:
            status_text = tr(STR_FTP_WAITING_NETWORK);
            status_help = tr(STR_FTP_WAITING_NETWORK_HELP);
            break;
        case FTP_STATUS_CONNECTED:
            snprintf(status_buf, sizeof(status_buf), tr(STR_FTP_CONNECTED_TEMPLATE), state->client_ip);
            status_text = status_buf;
            status_help = tr(STR_FTP_CONNECTED_HELP);
            break;
        default:
            snprintf(status_buf, sizeof(status_buf), tr(STR_FTP_LISTENING_TEMPLATE), state->local_ip);
            status_text = status_buf;
            status_help = tr(STR_FTP_LISTENING_HELP);
            break;
    }

    int y = 128;
    SDL_Color dot = ready ? COLOR_QUEUED : COLOR_TEXT_DIM;
    ui_draw_rounded_rect(LEFT_EDGE, y + 8, 14, 14, 7, dot);
    draw_text_fitted(g_font_body, LEFT_EDGE + 28, y, CONTENT_W - 28, COLOR_TEXT, status_text);
    y += 40;

    y = ui_draw_text_wrapped(g_font_small, LEFT_EDGE + 28, y, CONTENT_W - 28, 22, COLOR_TEXT_DIM, status_help);

    if (ready) {
        y += 4;
        ui_draw_text(g_font_small, LEFT_EDGE + 28, y, COLOR_TEXT_DIM, tr(STR_FTP_FORMATS));
        y += 26;
    }

    y += 22;
    ui_draw_rect(LEFT_EDGE, y, CONTENT_W, 1, COLOR_PANEL);
    y += 22;

    char queue_title[64];
    if (state->history_count > 0) {
        snprintf(queue_title, sizeof(queue_title), "%s (%d)", tr(STR_FTP_QUEUE_TITLE), state->history_count);
    } else {
        snprintf(queue_title, sizeof(queue_title), "%s", tr(STR_FTP_QUEUE_TITLE));
    }
    ui_draw_text(g_font_small, LEFT_EDGE, y, COLOR_TEXT_DIM, queue_title);
    y += 30;

    if (state->history_count == 0) {
        ui_draw_text(g_font_small, LEFT_EDGE, y, COLOR_TEXT_DIM, tr(STR_FTP_QUEUE_EMPTY));
    } else {
        int max_rows = (FOOTER_Y - 20 - y) / QUEUE_ROW_H;
        if (max_rows < 1) max_rows = 1;
        int shown = state->history_count < max_rows ? state->history_count : max_rows;
        for (int i = 0; i < shown; i++) {
            draw_history_row(y, &state->history[state->history_count - 1 - i]);
            y += QUEUE_ROW_H;
        }
    }

    ui_draw_rect(LEFT_EDGE, FOOTER_Y - 12, CONTENT_W, 1, COLOR_PANEL);
    ui_draw_button_hint(LEFT_EDGE, FOOTER_Y, UI_BTN_B, tr(STR_FTP_HINT_EXIT));

    SDL_RenderPresent(g_renderer);
}

void ui_show_ftp(void) {
    char err[200];
    if (!ftp_start(err, sizeof(err))) {
        char msg[260];
        snprintf(msg, sizeof(msg), tr(STR_FTP_START_FAILED_TEMPLATE), err);
        ui_app_show_message(msg);
        return;
    }

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    padUpdate(&pad);

    FtpState state;
    memset(&state, 0, sizeof(state));
    state.status = FTP_STATUS_WAITING_FOR_NETWORK;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        if (kDown & (HidNpadButton_B | HidNpadButton_Plus)) {
            ui_sound_play(UI_SOUND_BACK);
            break;
        }

        // Only actually blocks (redrawing itself via ftp_progress_cb) while
        // a file is being transferred/installed - otherwise a short poll,
        // so this screen's own render below still runs every frame the
        // rest of the time.
        FtpProgressCtx ctx = { .pad = &pad, .last_render_tick = 0, .state = &state };
        ftp_step(&state, ftp_progress_cb, &ctx);

        draw_idle(&state);
    }

    ftp_stop();
}
