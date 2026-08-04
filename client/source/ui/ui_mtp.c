#include "ui_mtp.h"
#include "ui_app.h"
#include "ui_sound.h"
#include "../i18n.h"
#include "../mtp/mtp_ptp.h"

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

// Same throttle interval as every other install-flavored progress callback
// in this app (main.c's install_progress_cb, ui_saves.c's
// save_progress_cb) - a full render on every single callback call would
// slow the USB transfer itself down far more than it's worth.
#define MTP_WORK_INTERVAL_NS 150000000ULL

// Same red ui_queue.c defines for its own failed-download rows - matches
// COLOR_DANGER in ui_app.h (pipensx's dark-mode "Error"), kept as its own
// macro rather than importing that name since this predates it.
#define COLOR_ERROR ((SDL_Color){0xff, 0x45, 0x54, 0xff})

// Draws `text` on one line, cut with an ellipsis if it doesn't fit `max_w` -
// file names here are routinely longer than the screen.
static void draw_text_fitted(TTF_Font *font, int x, int y, int max_w, SDL_Color color, const char *text) {
    char buf[320];
    ui_truncate_to_width(font, text, max_w, buf, sizeof(buf));
    ui_draw_text(font, x, y, color, buf);
}

typedef struct {
    PadState *pad;
    u64 last_render_tick;
    // The same MtpState mtp_step is filling in - read (never written) here
    // for the filename/status it has already recorded by the time this
    // fires, so the busy screen can name what it's working on.
    const MtpState *state;
} MtpProgressCtx;

// Draws the panel every "something is happening" state shares: a title, the
// file it applies to, and whatever detail line the caller wants under it.
// `pct` < 0 means "no progress bar" (unknown total, or a phase with nothing
// meaningful to measure).
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

// Passed as mtp_step's InstallProgressCallback - fires during both the USB
// file receive and, right after, the commit phase (see mtp_ptp.c's
// finish_recv), so this is the only rendering that happens while either is
// in progress.
static bool mtp_progress_cb(long total, long now, void *userdata) {
    MtpProgressCtx *ctx = (MtpProgressCtx *)userdata;
    const MtpState *state = ctx ? ctx->state : NULL;
    bool committing = state && state->status == MTP_STATUS_INSTALLING;

    u64 now_tick = armGetSystemTick();
    // The commit phase calls this exactly once, to put its message up before
    // blocking - throttling that one call away would leave the finished
    // transfer's own last frame on screen for the whole commit instead.
    if (!committing && ctx && armTicksToNs(now_tick - ctx->last_render_tick) < MTP_WORK_INTERVAL_NS) return true;
    if (ctx) ctx->last_render_tick = now_tick;

    bool cancel = false;
    if (!committing && ctx && ctx->pad) {
        padUpdate(ctx->pad);
        cancel = (padGetButtons(ctx->pad) & HidNpadButton_B) != 0;
    }

    const char *filename = state ? state->current_file : NULL;

    if (committing) {
        // Nothing to measure and nothing to cancel - the install is already
        // writing into the title database at this point.
        draw_busy_panel(tr(STR_MTP_INSTALLING_NOW), filename, NULL, -1.0f);
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
        // Total genuinely unknown (a >4GB object whose host never stated a
        // size - see mtp_ptp.c's receive_data_phase_unbounded). A bar stuck
        // at 0% would read as hung, so show only the count that is moving.
        snprintf(detail, sizeof(detail), "%s", now_str);
    }

    draw_busy_panel(tr(STR_MTP_RECEIVING), filename, detail, pct);

    // A host dragging several files at once sends them one at a time over
    // this same session (see mtp_ptp.h's own doc comment) - this is the
    // only sign of that during an active transfer that isn't the file name
    // itself changing, so a batch drag reads as "a queue running", not just
    // "a file, then another unrelated file".
    if (state && state->history_count > 0) {
        char count_line[64];
        snprintf(count_line, sizeof(count_line), tr(STR_MTP_SESSION_COUNT_TEMPLATE), state->history_count);
        ui_draw_text(g_font_small, LEFT_EDGE, 400, COLOR_TEXT_DIM, count_line);
    }

    ui_draw_button_hint(LEFT_EDGE, 470, UI_BTN_B, tr(STR_SAVES_HINT_CANCEL));

    SDL_RenderPresent(g_renderer);
    return !cancel;
}

#define QUEUE_ROW_H 36

// One row of the session's history - filename on the left (ellipsized),
// outcome on the right, in the same colour language ui_queue.c's download
// queue uses (COLOR_QUEUED for installed, COLOR_ERROR for failed) so a user
// who already knows that screen reads this one for free.
static void draw_history_row(int y, const MtpHistoryItem *item) {
    const char *label = item->status == MTP_HISTORY_INSTALLED ? tr(STR_QUEUE_STATUS_INSTALLED)
                                                                : tr(STR_QUEUE_STATUS_ERROR);
    SDL_Color color = item->status == MTP_HISTORY_INSTALLED ? COLOR_QUEUED : COLOR_ERROR;

    int label_w = 0, label_h = 0;
    TTF_SizeUTF8(g_font_small, label, &label_w, &label_h);
    draw_text_fitted(g_font_small, LEFT_EDGE, y, CONTENT_W - label_w - 24, COLOR_TEXT, item->filename);
    ui_draw_text_right(g_font_small, RIGHT_EDGE, y, color, label);
}

// The idle screen: what state the USB link is in, how to use it, and a
// running list of what this session has received - the "queue" a batch drag
// produces isn't known ahead of time (MTP never states how many objects are
// coming - see mtp_ptp.h), so unlike the download queue this only ever
// grows from the bottom as each transfer actually finishes, newest on top.
static void draw_idle(const MtpState *state) {
    SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(g_renderer);

    ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, tr(STR_MTP_TITLE));
    ui_draw_rect(LEFT_EDGE, HEADER_Y + 54, CONTENT_W, 1, COLOR_PANEL);

    // Status line, with a dot whose colour carries the same information at a
    // glance - dim while it's still coming up, green once it can receive.
    bool ready = state->status == MTP_STATUS_IDLE;
    const char *status_text;
    const char *status_help;
    switch (state->status) {
        case MTP_STATUS_WAITING_FOR_USB:
            status_text = tr(STR_MTP_WAITING_USB);
            status_help = tr(STR_MTP_WAITING_USB_HELP);
            break;
        case MTP_STATUS_WAITING_FOR_HOST:
            status_text = tr(STR_MTP_WAITING_HOST);
            status_help = tr(STR_MTP_WAITING_HOST_HELP);
            break;
        default:
            status_text = tr(STR_MTP_READY);
            status_help = tr(STR_MTP_HELP);
            break;
    }

    int y = 128;
    SDL_Color dot = ready ? COLOR_QUEUED : COLOR_TEXT_DIM;
    ui_draw_rounded_rect(LEFT_EDGE, y + 8, 14, 14, 7, dot);
    ui_draw_text(g_font_body, LEFT_EDGE + 28, y, COLOR_TEXT, status_text);
    y += 40;

    y = ui_draw_text_wrapped(g_font_small, LEFT_EDGE + 28, y, CONTENT_W - 28, 22, COLOR_TEXT_DIM, status_help);

    if (ready) {
        y += 4;
        ui_draw_text(g_font_small, LEFT_EDGE + 28, y, COLOR_TEXT_DIM, tr(STR_MTP_FORMATS));
        y += 26;
    }

    // Session history, filling whatever's left above the footer.
    y += 22;
    ui_draw_rect(LEFT_EDGE, y, CONTENT_W, 1, COLOR_PANEL);
    y += 22;

    char queue_title[64];
    if (state->history_count > 0) {
        snprintf(queue_title, sizeof(queue_title), "%s (%d)", tr(STR_MTP_QUEUE_TITLE), state->history_count);
    } else {
        snprintf(queue_title, sizeof(queue_title), "%s", tr(STR_MTP_QUEUE_TITLE));
    }
    ui_draw_text(g_font_small, LEFT_EDGE, y, COLOR_TEXT_DIM, queue_title);
    y += 30;

    if (state->history_count == 0) {
        ui_draw_text(g_font_small, LEFT_EDGE, y, COLOR_TEXT_DIM, tr(STR_MTP_QUEUE_EMPTY));
    } else {
        int max_rows = (FOOTER_Y - 20 - y) / QUEUE_ROW_H;
        if (max_rows < 1) max_rows = 1;
        int shown = state->history_count < max_rows ? state->history_count : max_rows;
        // Newest first - what just happened matters more than the first
        // file of a long batch, and it's the one still worth seeing once
        // the list no longer fits on screen.
        for (int i = 0; i < shown; i++) {
            draw_history_row(y, &state->history[state->history_count - 1 - i]);
            y += QUEUE_ROW_H;
        }
    }

    ui_draw_rect(LEFT_EDGE, FOOTER_Y - 12, CONTENT_W, 1, COLOR_PANEL);
    ui_draw_button_hint(LEFT_EDGE, FOOTER_Y, UI_BTN_B, tr(STR_MTP_HINT_EXIT));

    SDL_RenderPresent(g_renderer);
}

void ui_show_mtp(void) {
    char err[200];
    if (!mtp_start(err, sizeof(err))) {
        char msg[260];
        snprintf(msg, sizeof(msg), tr(STR_MTP_START_FAILED_TEMPLATE), err);
        ui_app_show_message(msg);
        return;
    }

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    padUpdate(&pad);

    MtpState state;
    memset(&state, 0, sizeof(state));
    state.status = MTP_STATUS_WAITING_FOR_USB;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        if (kDown & (HidNpadButton_B | HidNpadButton_Plus)) {
            ui_sound_play(UI_SOUND_BACK);
            break;
        }

        // Only actually blocks (redrawing itself via mtp_progress_cb) while
        // a file is being received/installed - otherwise a short poll, so
        // this screen's own render below still runs every frame the rest
        // of the time.
        MtpProgressCtx ctx = { .pad = &pad, .last_render_tick = 0, .state = &state };
        mtp_step(&state, mtp_progress_cb, &ctx);

        draw_idle(&state);
    }

    mtp_stop();
}
