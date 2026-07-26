#include "ui_queue.h"
#include "ui_app.h"
#include "ui_icons.h"
#include "ui_nav.h"
#include "../install/install_dispatch.h"
#include "../i18n.h"

#include <switch.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 1280
#define SCREEN_H 720
#define LEFT_EDGE 20
#define RIGHT_EDGE (SCREEN_W - 20)

#define HEADER_Y 40
#define LIST_TOP 120
#define ROW_H 60
#define VISIBLE_ROWS 7
#define ROW_ICON 44

// Progress panel shown at the bottom while a batch is installing.
#define PROGRESS_PANEL_Y 560

// Only actually redraw/cancel-check this often during an install, for the
// same reason main.c's install_progress_cb throttles - see that comment.
#define QUEUE_WORK_INTERVAL_NS 150000000ULL // ~6-7 times/sec

#define COLOR_ERROR ((SDL_Color){0xd8, 0x5a, 0x5a, 0xff})

typedef enum {
    Q_PENDING,
    Q_ACTIVE,
    Q_DONE,
    Q_FAILED,
} QItemStatus;

// ---- queue data ----

static char s_ids[UI_QUEUE_MAX][APP_ENTRY_ID_MAX];
static int s_count = 0;

bool ui_queue_contains(const char *id) {
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_ids[i], id) == 0) return true;
    }
    return false;
}

int ui_queue_count(void) {
    return s_count;
}

void ui_queue_add(const char *id) {
    if (ui_queue_contains(id) || s_count >= UI_QUEUE_MAX) return;
    snprintf(s_ids[s_count], sizeof(s_ids[s_count]), "%s", id);
    s_count++;
}

void ui_queue_remove(const char *id) {
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_ids[i], id) != 0) continue;
        for (int j = i; j < s_count - 1; j++) {
            snprintf(s_ids[j], sizeof(s_ids[j]), "%s", s_ids[j + 1]);
        }
        s_count--;
        return;
    }
}

// ---- rendering ----

static const AppEntry *resolve(AppEntry *entries, int count, const char *id) {
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].id, id) == 0) return &entries[i];
    }
    return NULL;
}

static const char *type_label(AppFileType t) {
    switch (t) {
        case APP_FILE_TYPE_NSP: return "NSP";
        case APP_FILE_TYPE_XCI: return "XCI";
        case APP_FILE_TYPE_PORT: return "Port";
        case APP_FILE_TYPE_NSZ: return "NSZ";
        default: return "NRO";
    }
}

static void status_text(QItemStatus st, const char **out_label, SDL_Color *out_color) {
    switch (st) {
        case Q_ACTIVE:  *out_label = tr(STR_QUEUE_STATUS_DOWNLOADING); *out_color = COLOR_ACCENT; break;
        case Q_DONE:    *out_label = tr(STR_QUEUE_STATUS_INSTALLED);   *out_color = COLOR_QUEUED; break;
        case Q_FAILED:  *out_label = tr(STR_QUEUE_STATUS_ERROR);       *out_color = COLOR_ERROR;  break;
        default:        *out_label = tr(STR_QUEUE_STATUS_WAITING);     *out_color = COLOR_TEXT_DIM; break;
    }
}

// Draws one queue row. `status` may be Q_PENDING for the browse view (no
// install running yet). `highlight` draws the selection/active accent behind it.
static void draw_row(AppEntry *entries, int entry_count, const char *id, int row_y,
                     QItemStatus status, bool highlight) {
    const AppEntry *e = resolve(entries, entry_count, id);

    if (highlight) {
        ui_draw_rect(LEFT_EDGE, row_y, RIGHT_EDGE - LEFT_EDGE, ROW_H - 6, COLOR_PANEL);
    }

    int icon_x = LEFT_EDGE + 10;
    int icon_y = row_y + (ROW_H - 6 - ROW_ICON) / 2;
    ui_draw_rect(icon_x, icon_y, ROW_ICON, ROW_ICON, COLOR_BG);
    if (e) {
        SDL_Texture *icon = ui_icons_get(e);
        if (icon) {
            SDL_Rect dst = { icon_x, icon_y, ROW_ICON, ROW_ICON };
            SDL_RenderCopy(g_renderer, icon, NULL, &dst);
        }
    }

    int text_x = icon_x + ROW_ICON + 14;
    ui_draw_text(g_font_body, text_x, row_y + 8, COLOR_TEXT, e ? e->title : id);

    if (e) {
        char meta[48];
        snprintf(meta, sizeof(meta), "%s  -  %.1f MB", type_label(e->file_type), e->file_size / (1024.0 * 1024.0));
        ui_draw_text(g_font_small, text_x, row_y + 34, COLOR_TEXT_DIM, meta);
    }

    const char *st_label;
    SDL_Color st_color;
    status_text(status, &st_label, &st_color);
    ui_draw_text_right(g_font_small, RIGHT_EDGE - 16, row_y + 20, st_color, st_label);
}

// ---- install-phase progress rendering ----

typedef struct {
    PadState *pad;
    AppEntry *entries;
    int entry_count;
    const QItemStatus *statuses;
    int active_index;    // index into the queue currently installing
    int scroll_offset;   // so the active row stays visible in the list
    u64 start_tick;
    bool started;
    u64 last_render_tick;
    InstallPhase phase;
} QueueProgressCtx;

static void draw_queue_installing(QueueProgressCtx *ctx, long total, long now) {
    ui_icons_begin_frame();

    SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(g_renderer);

    ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, tr(STR_QUEUE_TITLE));

    int shown = 0;
    for (int i = ctx->scroll_offset; i < s_count && shown < VISIBLE_ROWS; i++, shown++) {
        int row_y = LIST_TOP + shown * ROW_H;
        draw_row(ctx->entries, ctx->entry_count, s_ids[i], row_y, ctx->statuses[i], i == ctx->active_index);
    }

    // Progress panel for the active item.
    ui_draw_rect(LEFT_EDGE, PROGRESS_PANEL_Y, RIGHT_EDGE - LEFT_EDGE, 120, COLOR_PANEL);

    const AppEntry *active = (ctx->active_index >= 0 && ctx->active_index < s_count)
                                 ? resolve(ctx->entries, ctx->entry_count, s_ids[ctx->active_index])
                                 : NULL;
    char head[200];
    snprintf(head, sizeof(head), tr(STR_QUEUE_ITEM_OF_TEMPLATE),
             ctx->phase == INSTALL_PHASE_INSTALLING ? tr(STR_QUEUE_PHASE_INSTALLING) : tr(STR_QUEUE_PHASE_DOWNLOADING),
             ctx->active_index + 1, s_count, active ? active->title : "");
    ui_draw_text(g_font_body, LEFT_EDGE + 16, PROGRESS_PANEL_Y + 12, COLOR_TEXT, head);

    float pct = total > 0 ? (float)now / (float)total : 0.0f;
    ui_draw_progress_bar(LEFT_EDGE + 16, PROGRESS_PANEL_Y + 48, RIGHT_EDGE - LEFT_EDGE - 32, 20, pct,
                         COLOR_ACCENT, COLOR_BG);

    char line[96];
    if (total > 0) {
        snprintf(line, sizeof(line), "%d%%  (%.1f / %.1f MB)", (int)(pct * 100),
                 now / (1024.0 * 1024.0), total / (1024.0 * 1024.0));
    } else {
        snprintf(line, sizeof(line), "%.1f MB", now / (1024.0 * 1024.0));
    }

    if (ctx->started && total > 0 && now > 0) {
        double elapsed = armTicksToNs(armGetSystemTick() - ctx->start_tick) / 1e9;
        if (elapsed > 0.5) {
            double bps = now / elapsed;
            if (bps > 0) {
                double remaining = (total - now) / bps;
                int mins = (int)(remaining / 60);
                int secs = (int)remaining % 60;
                char more[64];
                snprintf(more, sizeof(more), "   -   %.1f MB/s   -   %dm %ds", bps / (1024.0 * 1024.0), mins, secs);
                strncat(line, more, sizeof(line) - strlen(line) - 1);
            }
        }
    }
    ui_draw_text(g_font_small, LEFT_EDGE + 16, PROGRESS_PANEL_Y + 78, COLOR_TEXT_DIM, line);
    ui_draw_text_right(g_font_small, RIGHT_EDGE - 16, PROGRESS_PANEL_Y + 78, COLOR_TEXT_DIM, tr(STR_QUEUE_CANCEL_HINT));

    SDL_RenderPresent(g_renderer);
}

static bool queue_progress_cb(long total, long now, void *userdata) {
    QueueProgressCtx *ctx = (QueueProgressCtx *)userdata;

    u64 now_tick = armGetSystemTick();
    if (ctx && armTicksToNs(now_tick - ctx->last_render_tick) < QUEUE_WORK_INTERVAL_NS) {
        return true;
    }
    if (ctx) ctx->last_render_tick = now_tick;

    bool cancel = false;
    if (ctx && ctx->pad) {
        padUpdate(ctx->pad);
        cancel = (padGetButtons(ctx->pad) & HidNpadButton_B) != 0;
    }
    if (ctx && !ctx->started) {
        ctx->start_tick = now_tick;
        ctx->started = true;
    }

    if (ctx) draw_queue_installing(ctx, total, now);
    return !cancel;
}

static void on_queue_phase(InstallPhase phase, void *userdata) {
    QueueProgressCtx *ctx = (QueueProgressCtx *)userdata;
    if (ctx) ctx->phase = phase;
}

// ---- the batch install ----

static void run_queue_install(AppEntry *entries, int entry_count) {
    QItemStatus statuses[UI_QUEUE_MAX];
    for (int i = 0; i < s_count; i++) statuses[i] = Q_PENDING;

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    padUpdate(&pad);

    QueueProgressCtx ctx = {
        .pad = &pad, .entries = entries, .entry_count = entry_count, .statuses = statuses,
        .active_index = 0, .scroll_offset = 0, .start_tick = 0, .started = false,
        .last_render_tick = 0, .phase = INSTALL_PHASE_DOWNLOADING,
    };

    int total_items = s_count;
    int ok_count = 0;
    bool canceled = false;
    char err_buf[256];

    // Free every cached icon texture before the batch starts - same reason
    // as main.c's single-install path: these are the app's biggest memory
    // consumer and installs (NSZ decompression especially) need that room.
    // The rows below draw their icon via ui_icons_get, which simply
    // re-reads from the on-SD cache as each one comes back into view.
    ui_icons_clear();

    for (int i = 0; i < total_items; i++) {
        const AppEntry *e = resolve(entries, entry_count, s_ids[i]);
        if (!e) { statuses[i] = Q_FAILED; continue; }

        statuses[i] = Q_ACTIVE;
        ctx.active_index = i;
        ctx.started = false;
        ctx.phase = INSTALL_PHASE_DOWNLOADING;
        // Keep the active row in view.
        if (i >= ctx.scroll_offset + VISIBLE_ROWS) ctx.scroll_offset = i - VISIBLE_ROWS + 1;
        if (i < ctx.scroll_offset) ctx.scroll_offset = i;

        InstallOneResult r = install_one_entry(e, queue_progress_cb, on_queue_phase, &ctx, err_buf, sizeof(err_buf));
        if (r == INSTALL_ONE_OK) {
            statuses[i] = Q_DONE;
            ok_count++;
        } else if (r == INSTALL_ONE_CANCELED) {
            statuses[i] = Q_PENDING;
            canceled = true;
            break;
        } else {
            statuses[i] = Q_FAILED;
        }
    }

    // Results view - the final list stays on screen (each row showing its
    // outcome) until the user backs out, rather than a throwaway popup.
    padUpdate(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        if (kDown & (HidNpadButton_B | HidNpadButton_A | HidNpadButton_Plus)) break;

        ui_icons_begin_frame();
        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, tr(STR_QUEUE_TITLE));

        int shown = 0;
        int scroll = 0;
        for (int i = scroll; i < s_count && shown < VISIBLE_ROWS; i++, shown++) {
            int row_y = LIST_TOP + shown * ROW_H;
            draw_row(entries, entry_count, s_ids[i], row_y, statuses[i], false);
        }

        char summary[128];
        if (canceled) {
            snprintf(summary, sizeof(summary), tr(STR_QUEUE_CANCELED_TEMPLATE), ok_count, total_items);
        } else {
            snprintf(summary, sizeof(summary), tr(STR_QUEUE_DONE_TEMPLATE), ok_count, total_items);
        }
        ui_draw_text(g_font_body, LEFT_EDGE, SCREEN_H - 80, COLOR_TEXT, summary);
        ui_draw_text(g_font_small, LEFT_EDGE, SCREEN_H - 46, COLOR_TEXT_DIM, tr(STR_QUEUE_RESULTS_HINT));

        SDL_RenderPresent(g_renderer);
    }

    // The batch consumed the queue either way - anything not installed
    // (canceled/failed) can be re-added from its detail screen.
    s_count = 0;
}

// ---- the browse screen ----

void ui_show_queue(AppEntry *entries, int count) {
    int selected = 0;
    int scroll_offset = 0;

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    padUpdate(&pad);

    NavRepeatState nav_up = {0}, nav_down = {0};

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        u64 kHeld = padGetButtons(&pad);
        HidAnalogStickState stick = padGetStickPos(&pad, 0);
        u64 now_tick = armGetSystemTick();

        if (selected >= s_count) selected = s_count > 0 ? s_count - 1 : 0;

        bool held_up = (kHeld & HidNpadButton_Up) || stick.y > NAV_STICK_DEADZONE;
        bool held_down = (kHeld & HidNpadButton_Down) || stick.y < -NAV_STICK_DEADZONE;
        if (nav_repeat_step(&nav_up, held_up, now_tick) && selected > 0) selected--;
        if (nav_repeat_step(&nav_down, held_down, now_tick) && selected < s_count - 1) selected++;

        if (selected < scroll_offset) scroll_offset = selected;
        if (selected >= scroll_offset + VISIBLE_ROWS) scroll_offset = selected - VISIBLE_ROWS + 1;

        if ((kDown & HidNpadButton_X) && s_count > 0) {
            ui_queue_remove(s_ids[selected]);
            if (selected >= s_count) selected = s_count > 0 ? s_count - 1 : 0;
        }
        if ((kDown & HidNpadButton_A) && s_count > 0) {
            run_queue_install(entries, count);
            // The queue is emptied by the batch - nothing left to browse,
            // so drop straight back to the main list.
            return;
        }
        if (kDown & (HidNpadButton_B | HidNpadButton_Plus)) {
            return;
        }

        ui_icons_begin_frame();
        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        char title[64];
        snprintf(title, sizeof(title), tr(STR_QUEUE_TITLE_COUNT_TEMPLATE), s_count);
        ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, title);

        if (s_count == 0) {
            ui_draw_text(g_font_body, LEFT_EDGE, LIST_TOP, COLOR_TEXT_DIM, tr(STR_QUEUE_EMPTY_LINE1));
            ui_draw_text(g_font_small, LEFT_EDGE, LIST_TOP + 34, COLOR_TEXT_DIM, tr(STR_QUEUE_EMPTY_LINE2));
        }

        int shown = 0;
        for (int i = scroll_offset; i < s_count && shown < VISIBLE_ROWS; i++, shown++) {
            int row_y = LIST_TOP + shown * ROW_H;
            draw_row(entries, count, s_ids[i], row_y, Q_PENDING, i == selected);
        }

        ui_draw_rect(LEFT_EDGE, SCREEN_H - 56, RIGHT_EDGE - LEFT_EDGE, 1, COLOR_PANEL);
        if (s_count > 0) {
            ui_draw_text(g_font_small, LEFT_EDGE, SCREEN_H - 44, COLOR_TEXT_DIM, tr(STR_QUEUE_FOOTER_WITH_ITEMS));
        } else {
            ui_draw_text(g_font_small, LEFT_EDGE, SCREEN_H - 44, COLOR_TEXT_DIM, tr(STR_QUEUE_FOOTER_EMPTY));
        }

        SDL_RenderPresent(g_renderer);
    }
}
