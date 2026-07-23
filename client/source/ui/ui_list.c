#include "ui_list.h"
#include "ui_app.h"
#include "ui_storage.h"
#include "ui_status.h"
#include "ui_icons.h"

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SCREEN_W 1280
#define SCREEN_H 720

#define RIGHT_EDGE (SCREEN_W - 20)
#define LEFT_EDGE 20

#define STATUS_Y 8
#define HEADER_Y 40

#define PANEL_Y 40
#define PANEL_H 92
#define PANEL_W 300
#define PANEL_GAP 20
#define PANEL_NAND_X (RIGHT_EDGE - PANEL_W)
#define PANEL_SD_X (PANEL_NAND_X - PANEL_GAP - PANEL_W)

#define COL_HEADER_Y 148
#define COL_HEADER_H 34
#define LIST_TOP (COL_HEADER_Y + COL_HEADER_H + 10)
#define ROW_HEIGHT 42
#define VISIBLE_ROWS 10

#define COL_NAME_X 40
#define COL_TYPE_X 660
#define COL_VERSION_X 780
#define COL_CATEGORY_X 900
#define COL_SIZE_X 1080

// Grid view (Y toggles list <-> grid). Fixed constants tuned for the fixed
// 1280x720 layout this whole screen already assumes (see SCREEN_W/H above) -
// not derived dynamically, matching the rest of this file's style.
#define GRID_TOP COL_HEADER_Y
#define GRID_COLS 5
#define GRID_ROWS_VISIBLE 2
#define GRID_CELL_W 224
#define GRID_CELL_H 224
#define GRID_ICON_SIZE 168
#define GRID_GAP 16
#define GRID_TITLE_MAX_W (GRID_ICON_SIZE)

#define FOOTER_Y (SCREEN_H - 46)

// Categories sidebar (L toggles it). A generous fixed cap, matching this
// file's existing style (VISIBLE_ROWS/GRID_COLS etc. are fixed too) rather
// than dynamically sizing for the 1280x720 layout this whole screen assumes.
#define MAX_CATEGORIES 16
#define SIDEBAR_X LEFT_EDGE
#define SIDEBAR_PANEL_TOP 20
#define SIDEBAR_PANEL_BOTTOM (FOOTER_Y - 20)
#define SIDEBAR_TITLE_Y (SIDEBAR_PANEL_TOP + 16)
#define SIDEBAR_Y (SIDEBAR_TITLE_Y + 40)
#define SIDEBAR_W 320
#define SIDEBAR_ROW_H 38
#define SIDEBAR_VISIBLE_ROWS 13

// Indices into `entries` that pass the current category filter - a plain
// cap (matching APP_DLC_MAX/SOURCES_MAX elsewhere in this project) instead
// of a heap allocation, since a homebrew catalog is never going to be huge.
#define VISIBLE_MAX 256

typedef enum {
    VIEW_LIST = 0,
    VIEW_GRID,
} ViewMode;

typedef enum {
    SORT_TITLE = 0,
    SORT_CATEGORY,
    SORT_VERSION,
    SORT_MODE_COUNT,
} SortMode;

static const char *sort_mode_label(SortMode mode) {
    switch (mode) {
        case SORT_CATEGORY: return "Categoría";
        case SORT_VERSION: return "Versión";
        default: return "Título";
    }
}

static int cmp_by_title(const void *a, const void *b) {
    return strcasecmp(((const AppEntry *)a)->title, ((const AppEntry *)b)->title);
}
static int cmp_by_category(const void *a, const void *b) {
    int c = strcasecmp(((const AppEntry *)a)->category, ((const AppEntry *)b)->category);
    return c != 0 ? c : cmp_by_title(a, b);
}
static int cmp_by_version(const void *a, const void *b) {
    int c = strcasecmp(((const AppEntry *)a)->version, ((const AppEntry *)b)->version);
    return c != 0 ? c : cmp_by_title(a, b);
}

static void apply_sort(AppEntry *entries, int count, SortMode mode) {
    int (*cmp)(const void *, const void *) = cmp_by_title;
    if (mode == SORT_CATEGORY) cmp = cmp_by_category;
    if (mode == SORT_VERSION) cmp = cmp_by_version;
    qsort(entries, (size_t)count, sizeof(AppEntry), cmp);
}

// Every distinct, non-empty `category` value across `entries`, in first-seen
// order. Used to populate the sidebar - "Todos" (all of them) is handled
// separately by the caller, not included here.
static int collect_categories(const AppEntry *entries, int count,
                               char categories[][APP_ENTRY_CATEGORY_MAX]) {
    int n = 0;
    for (int i = 0; i < count && n < MAX_CATEGORIES; i++) {
        if (entries[i].category[0] == '\0') continue;
        bool found = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(categories[j], entries[i].category) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(categories[n], APP_ENTRY_CATEGORY_MAX, "%s", entries[i].category);
            n++;
        }
    }
    return n;
}

// Indices into `entries` whose category matches `filter` ("" = everything).
// Rebuilt whenever the filter or `entries`' order changes (sort, category
// pick) - cheap enough for a homebrew-catalog-sized list to just redo
// wholesale rather than track incrementally.
static int build_visible(const AppEntry *entries, int count, const char *filter, int *out_visible) {
    int n = 0;
    for (int i = 0; i < count && n < VISIBLE_MAX; i++) {
        if (filter[0] == '\0' || strcmp(entries[i].category, filter) == 0) {
            out_visible[n++] = i;
        }
    }
    return n;
}

// Boxed gauge in the Tinfoil style: label, big "X.X GB libres" line, thin
// usage bar. Distinct from the plain single-line rows this replaced.
static void draw_storage_panel(int x, int y, int w, int h, const char *label, bool ok,
                                s64 total, s64 free_bytes) {
    ui_draw_rect(x, y, w, h, COLOR_PANEL);
    ui_draw_text(g_font_small, x + 16, y + 10, COLOR_TEXT_DIM, label);

    if (!ok) {
        ui_draw_text(g_font_body, x + 16, y + 32, COLOR_TEXT_DIM, "no disponible");
        return;
    }

    char free_str[32];
    format_bytes(free_bytes, free_str, sizeof(free_str));
    char line[48];
    snprintf(line, sizeof(line), "%s libres", free_str);
    ui_draw_text(g_font_body, x + 16, y + 32, COLOR_TEXT, line);

    float pct = total > 0 ? (float)(total - free_bytes) / (float)total : 0.0f;
    ui_draw_progress_bar(x + 16, y + h - 24, w - 32, 12, pct, COLOR_ACCENT, COLOR_BG);
}

static const char *file_type_label(AppFileType type) {
    if (type == APP_FILE_TYPE_NSP) return "NSP";
    if (type == APP_FILE_TYPE_XCI) return "XCI";
    return "NRO";
}

static void format_size(long bytes, char *out, size_t out_size) {
    snprintf(out, out_size, "%.1f MB", bytes / (1024.0 * 1024.0));
}

// "14:32   WiFi+   87%" - trailing "+" on the network label marks charging,
// kept as a single glyph instead of a word so the line stays short enough
// to fit above the storage panels.
static void format_status_line(const SystemStatus *status, char *out, size_t out_size) {
    const char *clock = status->clock[0] ? status->clock : "--:--:--";
    const char *net = status->network_ok ? status->network_label : "Sin conexión";

    if (status->battery_ok) {
        snprintf(out, out_size, "%s    %s    %u%%%s", clock, net, status->battery_percent,
                 status->charging ? "+" : "");
    } else {
        snprintf(out, out_size, "%s    %s", clock, net);
    }
}

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

static void draw_grid_cell(int x, int y, const AppEntry *entry, bool is_selected) {
    if (is_selected) {
        ui_draw_rect(x - 6, y - 6, GRID_CELL_W - GRID_GAP + 12, GRID_CELL_H - GRID_GAP + 12, COLOR_ACCENT);
    }

    ui_draw_rect(x, y, GRID_ICON_SIZE, GRID_ICON_SIZE, COLOR_PANEL);

    SDL_Texture *icon = ui_icons_get(entry);
    if (icon) {
        SDL_Rect dst = { x, y, GRID_ICON_SIZE, GRID_ICON_SIZE };
        SDL_RenderCopy(g_renderer, icon, NULL, &dst);
    } else {
        // Placeholder while the icon loads (or if it never resolves): the
        // title's first letter, centered-ish, so the grid isn't just blank
        // panels before icons finish fetching.
        char initial[2] = { entry->title[0] ? entry->title[0] : '?', '\0' };
        ui_draw_text(g_font_title, x + GRID_ICON_SIZE / 2 - 8, y + GRID_ICON_SIZE / 2 - 16,
                     COLOR_TEXT_DIM, initial);
    }

    char title[64];
    truncate_to_width(g_font_small, entry->title, GRID_TITLE_MAX_W, title, sizeof(title));
    SDL_Color title_color = is_selected ? COLOR_TEXT : COLOR_TEXT_DIM;
    ui_draw_text(g_font_small, x, y + GRID_ICON_SIZE + 6, title_color, title);
}

// No bundled icon assets to map specific glyphs to arbitrary free-text
// categories, so each gets a small color swatch instead - still gives the
// sidebar rows a distinct per-category visual marker instead of a plain
// text list. Cycles if there are more categories than colors.
static const SDL_Color CATEGORY_SWATCH_COLORS[] = {
    { 0x3a, 0x8f, 0xd8, 0xff }, // blue (matches COLOR_ACCENT)
    { 0xd8, 0x8f, 0x3a, 0xff }, // orange
    { 0x5b, 0xc4, 0x5b, 0xff }, // green
    { 0xd8, 0x4a, 0x8f, 0xff }, // pink
    { 0x9a, 0x5b, 0xd8, 0xff }, // purple
    { 0x3a, 0xc9, 0xb0, 0xff }, // teal
};
#define CATEGORY_SWATCH_COUNT (sizeof(CATEGORY_SWATCH_COLORS) / sizeof(CATEGORY_SWATCH_COLORS[0]))
#define SIDEBAR_SWATCH_SIZE 14
#define SIDEBAR_TEXT_X (SIDEBAR_X + SIDEBAR_SWATCH_SIZE + 12)

// Spans the full content height (near the very top down to the footer
// rule) regardless of how many categories exist - a panel only as tall as
// its item count ends up as a small floating box that just partially
// covers whatever's underneath instead of cleanly replacing that column,
// which looks like a rendering glitch rather than a real sidebar.
static void draw_sidebar(const char categories[][APP_ENTRY_CATEGORY_MAX], int category_count,
                          int selected, int scroll) {
    int total_items = category_count + 1; // +1 for "Todos"
    int panel_h = SIDEBAR_PANEL_BOTTOM - SIDEBAR_PANEL_TOP;

    ui_draw_rect(SIDEBAR_X - 10, SIDEBAR_PANEL_TOP, SIDEBAR_W, panel_h, COLOR_PANEL);
    // Right-edge accent line so the panel reads as a distinct column rather
    // than a box floating over the grid/list.
    ui_draw_rect(SIDEBAR_X - 10 + SIDEBAR_W - 3, SIDEBAR_PANEL_TOP, 3, panel_h, COLOR_ACCENT);

    ui_draw_text(g_font_small, SIDEBAR_X, SIDEBAR_TITLE_Y, COLOR_TEXT_DIM, "CATEGORÍAS");

    for (int vi = scroll; vi < total_items && vi < scroll + SIDEBAR_VISIBLE_ROWS; vi++) {
        int row_y = SIDEBAR_Y + (vi - scroll) * SIDEBAR_ROW_H;
        bool is_selected = (vi == selected);
        if (is_selected) {
            // Bordered box (accent outline, panel-colored fill) instead of a
            // solid block - reads as a selection highlight, not a slab.
            int bx = SIDEBAR_X - 6, by = row_y - 4, bw = SIDEBAR_W - 20, bh = SIDEBAR_ROW_H - 6;
            ui_draw_rect(bx, by, bw, bh, COLOR_ACCENT);
            ui_draw_rect(bx + 3, by + 3, bw - 6, bh - 6, COLOR_PANEL);
        }
        SDL_Color color = is_selected ? COLOR_TEXT : COLOR_TEXT_DIM;
        const char *label = vi == 0 ? "Todos" : categories[vi - 1];

        // "Todos" gets a neutral marker (not a category of its own);
        // real categories cycle through the swatch palette by position.
        SDL_Color swatch = vi == 0 ? COLOR_TEXT_DIM : CATEGORY_SWATCH_COLORS[(vi - 1) % CATEGORY_SWATCH_COUNT];
        ui_draw_rect(SIDEBAR_X, row_y + 4, SIDEBAR_SWATCH_SIZE, SIDEBAR_SWATCH_SIZE, swatch);

        ui_draw_text(g_font_body, SIDEBAR_TEXT_X, row_y, color, label);
    }
}

int ui_show_list(AppEntry *entries, int count) {
    static ViewMode view_mode = VIEW_LIST;
    static SortMode sort_mode = SORT_TITLE;
    static char category_filter[APP_ENTRY_CATEGORY_MAX] = "";

    char categories[MAX_CATEGORIES][APP_ENTRY_CATEGORY_MAX];
    int category_count = collect_categories(entries, count, categories);

    // The catalog may have changed (sources reload) since category_filter
    // was last set - drop it if it no longer names a real category rather
    // than silently showing an empty list forever.
    if (category_filter[0] != '\0') {
        bool still_valid = false;
        for (int i = 0; i < category_count; i++) {
            if (strcmp(categories[i], category_filter) == 0) {
                still_valid = true;
                break;
            }
        }
        if (!still_valid) category_filter[0] = '\0';
    }

    int visible[VISIBLE_MAX];
    int visible_count = build_visible(entries, count, category_filter, visible);

    StorageInfo storage;
    ui_storage_refresh(&storage);

    SystemStatus status;
    ui_status_refresh(&status);
    char status_line[80];
    format_status_line(&status, status_line, sizeof(status_line));
    u64 last_status_tick = armGetSystemTick();

    // Persisted across calls (this screen is re-entered every time the
    // detail screen is backed out of) so backing out of a game doesn't
    // bounce the user back to the top of the list every time.
    static int selected = 0;
    static int scroll_offset = 0; // rows scrolled, in list-row or grid-row units depending on view_mode

    // The catalog/filter/sort can differ from the last time this screen was
    // shown (sources reload, or view_mode/category changed elsewhere) -
    // clamp rather than trust the previous position blindly. scroll_offset
    // doesn't need its own clamp: the per-frame scroll-follow logic below
    // already derives it from `selected` unconditionally.
    if (selected >= visible_count) selected = visible_count > 0 ? visible_count - 1 : 0;
    if (selected < 0) selected = 0;

    bool sidebar_open = false;
    int sidebar_selected = 0; // 0 = "Todos", i+1 = categories[i]
    int sidebar_scroll = 0;

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    // Prime the button-state baseline: a freshly initialized PadState has no
    // prior reading, so if a button is still physically held from the
    // previous screen (e.g. B held to cancel a download), the very first
    // padGetButtonsDown() would misreport it as newly pressed here.
    padUpdate(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        // Re-querying time/psm/nifm every frame would mean opening and
        // closing those service sessions 60x/sec for no practical benefit -
        // once a second is plenty for a clock/battery/network readout.
        u64 now_tick = armGetSystemTick();
        if (armTicksToNs(now_tick - last_status_tick) >= 1000000000ULL) {
            ui_status_refresh(&status);
            format_status_line(&status, status_line, sizeof(status_line));
            last_status_tick = now_tick;
        }

        if (sidebar_open) {
            int total_items = category_count + 1;
            if (kDown & HidNpadButton_Down) {
                if (sidebar_selected < total_items - 1) sidebar_selected++;
            }
            if (kDown & HidNpadButton_Up) {
                if (sidebar_selected > 0) sidebar_selected--;
            }
            if (kDown & HidNpadButton_A) {
                if (sidebar_selected == 0) {
                    category_filter[0] = '\0';
                } else {
                    snprintf(category_filter, sizeof(category_filter), "%s", categories[sidebar_selected - 1]);
                }
                visible_count = build_visible(entries, count, category_filter, visible);
                selected = 0;
                scroll_offset = 0;
                sidebar_open = false;
            }
            if (kDown & HidNpadButton_B) {
                sidebar_open = false;
            }
            if (sidebar_selected < sidebar_scroll) sidebar_scroll = sidebar_selected;
            if (sidebar_selected >= sidebar_scroll + SIDEBAR_VISIBLE_ROWS) {
                sidebar_scroll = sidebar_selected - SIDEBAR_VISIBLE_ROWS + 1;
            }
        } else {
            int cols = (view_mode == VIEW_GRID) ? GRID_COLS : 1;

            if (kDown & HidNpadButton_Down) {
                if (selected + cols < visible_count) selected += cols;
            }
            if (kDown & HidNpadButton_Up) {
                if (selected - cols >= 0) selected -= cols;
            }
            if (view_mode == VIEW_GRID) {
                if ((kDown & HidNpadButton_Right) && selected < visible_count - 1) selected++;
                if ((kDown & HidNpadButton_Left) && selected > 0) selected--;
            }
            if (kDown & HidNpadButton_A) {
                if (visible_count > 0) return visible[selected];
            }
            if (kDown & HidNpadButton_Minus) {
                return UI_LIST_OPEN_SOURCES;
            }
            if ((kDown & HidNpadButton_B) || (kDown & HidNpadButton_Plus)) {
                return UI_LIST_EXIT;
            }
            if (kDown & HidNpadButton_Y) {
                // Same `entries`/`selected` index means the same app in both
                // views - only the per-mode row/scroll math needs resetting.
                view_mode = (view_mode == VIEW_LIST) ? VIEW_GRID : VIEW_LIST;
                scroll_offset = 0;
            }
            if (kDown & HidNpadButton_X) {
                sort_mode = (SortMode)((sort_mode + 1) % SORT_MODE_COUNT);
                apply_sort(entries, count, sort_mode);
                // Sorting reorders `entries` in place - the set of ids
                // passing the filter is unchanged, but their positions are,
                // so `visible` must be rebuilt against the new order.
                visible_count = build_visible(entries, count, category_filter, visible);
                selected = 0;
                scroll_offset = 0;
            }
            if (kDown & HidNpadButton_L) {
                sidebar_open = true;
                sidebar_selected = 0;
                for (int i = 0; i < category_count; i++) {
                    if (strcmp(categories[i], category_filter) == 0) {
                        sidebar_selected = i + 1;
                        break;
                    }
                }
                sidebar_scroll = 0;
            }
        }

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, "FreeShop");
        ui_draw_text(g_font_body, 210, HEADER_Y + 4, COLOR_TEXT_DIM, "- Catálogo");
        ui_draw_text_right(g_font_body, RIGHT_EDGE, STATUS_Y, COLOR_TEXT, status_line);

        draw_storage_panel(PANEL_SD_X, PANEL_Y, PANEL_W, PANEL_H, "Tarjeta SD",
                            storage.sd_ok, storage.sd_total, storage.sd_free);
        draw_storage_panel(PANEL_NAND_X, PANEL_Y, PANEL_W, PANEL_H, "NAND",
                            storage.nand_ok, storage.nand_total, storage.nand_free);

        if (view_mode == VIEW_LIST) {
            ui_draw_rect(LEFT_EDGE, COL_HEADER_Y, RIGHT_EDGE - LEFT_EDGE, COL_HEADER_H, COLOR_PANEL);
            ui_draw_text(g_font_small, COL_NAME_X, COL_HEADER_Y + 8, COLOR_TEXT_DIM, "Nombre");
            ui_draw_text(g_font_small, COL_TYPE_X, COL_HEADER_Y + 8, COLOR_TEXT_DIM, "Tipo");
            ui_draw_text(g_font_small, COL_VERSION_X, COL_HEADER_Y + 8, COLOR_TEXT_DIM, "Versión");
            ui_draw_text(g_font_small, COL_CATEGORY_X, COL_HEADER_Y + 8, COLOR_TEXT_DIM, "Categoría");
            ui_draw_text(g_font_small, COL_SIZE_X, COL_HEADER_Y + 8, COLOR_TEXT_DIM, "Tamaño");

            if (visible_count == 0) {
                ui_draw_text(g_font_body, COL_NAME_X, LIST_TOP, COLOR_TEXT_DIM,
                             count == 0 ? "(el catálogo está vacío)" : "(sin apps en esta categoría)");
            }

            if (selected < scroll_offset) scroll_offset = selected;
            if (selected >= scroll_offset + VISIBLE_ROWS) scroll_offset = selected - VISIBLE_ROWS + 1;

            for (int vi = scroll_offset; vi < visible_count && vi < scroll_offset + VISIBLE_ROWS; vi++) {
                int i = visible[vi];
                int row_index = vi - scroll_offset;
                int row_y = LIST_TOP + row_index * ROW_HEIGHT;
                bool is_selected = (vi == selected);

                if (is_selected) {
                    ui_draw_rect(LEFT_EDGE, row_y - 6, RIGHT_EDGE - LEFT_EDGE, ROW_HEIGHT - 4, COLOR_ACCENT);
                } else if (row_index % 2 == 1) {
                    ui_draw_rect(LEFT_EDGE, row_y - 6, RIGHT_EDGE - LEFT_EDGE, ROW_HEIGHT - 4, COLOR_PANEL);
                }

                SDL_Color text_color = is_selected ? COLOR_BG : COLOR_TEXT;
                SDL_Color dim_color = is_selected ? COLOR_BG : COLOR_TEXT_DIM;

                char size_str[32];
                format_size(entries[i].file_size, size_str, sizeof(size_str));

                ui_draw_text(g_font_body, COL_NAME_X, row_y, text_color, entries[i].title);
                ui_draw_text(g_font_small, COL_TYPE_X, row_y + 2, dim_color, file_type_label(entries[i].file_type));
                ui_draw_text(g_font_small, COL_VERSION_X, row_y + 2, dim_color, entries[i].version);
                ui_draw_text(g_font_small, COL_CATEGORY_X, row_y + 2, dim_color, entries[i].category);
                ui_draw_text(g_font_small, COL_SIZE_X, row_y + 2, dim_color, size_str);
            }
        } else {
            ui_icons_begin_frame();

            int selected_row = selected / GRID_COLS;
            if (selected_row < scroll_offset) scroll_offset = selected_row;
            if (selected_row >= scroll_offset + GRID_ROWS_VISIBLE) scroll_offset = selected_row - GRID_ROWS_VISIBLE + 1;

            if (visible_count == 0) {
                ui_draw_text(g_font_body, LEFT_EDGE, GRID_TOP, COLOR_TEXT_DIM,
                             count == 0 ? "(el catálogo está vacío)" : "(sin apps en esta categoría)");
            }

            int first = scroll_offset * GRID_COLS;
            int last = first + GRID_ROWS_VISIBLE * GRID_COLS;
            for (int vi = first; vi < visible_count && vi < last; vi++) {
                int i = visible[vi];
                int row_in_view = (vi / GRID_COLS) - scroll_offset;
                int col = vi % GRID_COLS;
                int cell_x = LEFT_EDGE + col * GRID_CELL_W;
                int cell_y = GRID_TOP + row_in_view * GRID_CELL_H;
                draw_grid_cell(cell_x, cell_y, &entries[i], vi == selected);
            }
        }

        if (sidebar_open) {
            draw_sidebar(categories, category_count, sidebar_selected, sidebar_scroll);
        }

        ui_draw_rect(LEFT_EDGE, FOOTER_Y - 10, RIGHT_EDGE - LEFT_EDGE, 1, COLOR_PANEL);
        char footer[200];
        if (sidebar_open) {
            snprintf(footer, sizeof(footer), "Arriba/Abajo: elegir    A: aplicar    B: cerrar");
        } else {
            snprintf(footer, sizeof(footer),
                     "D-Pad: navegar    A: instalar    L: categorías%s    Y: vista %s    X: ordenar (%s)    "
                     "-: fuentes    B/+: salir",
                     category_filter[0] ? " (filtrado)" : "", view_mode == VIEW_LIST ? "cuadrícula" : "lista",
                     sort_mode_label(sort_mode));
        }
        ui_draw_text(g_font_small, LEFT_EDGE, FOOTER_Y, COLOR_TEXT_DIM, footer);

        SDL_RenderPresent(g_renderer);
    }

    return UI_LIST_EXIT;
}
