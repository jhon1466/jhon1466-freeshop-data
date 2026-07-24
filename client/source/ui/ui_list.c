#include "ui_list.h"
#include "ui_app.h"
#include "ui_storage.h"
#include "ui_status.h"
#include "ui_icons.h"
#include "ui_prefs.h"
#include "ui_nav.h"

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

// Always-visible horizontal category tab bar, replacing the old L-toggled
// sidebar - sits right below the storage panels, pushing everything below
// it (column header / grid) down from where it used to start.
#define TAB_BAR_Y (PANEL_Y + PANEL_H + 4)
#define TAB_BAR_H 40
#define TAB_BAR_ARROW_BOX 30
#define TAB_BAR_ARROW_W (TAB_BAR_ARROW_BOX + 16)
#define TAB_BAR_PAD_X 18
#define TAB_BAR_UNDERLINE_H 4

#define COL_HEADER_Y (TAB_BAR_Y + TAB_BAR_H + 10)
#define COL_HEADER_H 34
#define LIST_TOP (COL_HEADER_Y + COL_HEADER_H + 10)
#define ROW_HEIGHT 42
#define VISIBLE_ROWS 9

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
// GRID_COLS * GRID_CELL_W (1120px) doesn't fill the full content width
// (1240px) - centering the block instead of hugging LEFT_EDGE avoids the
// lopsided look of empty space piling up on the right only.
#define GRID_LEFT (LEFT_EDGE + ((RIGHT_EDGE - LEFT_EDGE) - GRID_COLS * GRID_CELL_W) / 2)

#define FOOTER_Y (SCREEN_H - 46)

// A generous fixed cap on distinct categories, matching this file's
// existing style (VISIBLE_ROWS/GRID_COLS etc. are fixed too) rather than
// dynamically sizing for the 1280x720 layout this whole screen assumes.
#define MAX_CATEGORIES 16

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
// order. Used to populate the category tab bar - "Todos" (all of them) is
// handled separately by the caller, not included here.
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
// Case-insensitive substring search - not strcasestr (a GNU extension gated
// behind _GNU_SOURCE in this toolchain's newlib) to avoid pulling in a
// broader feature-test macro just for this one call.
static bool title_contains(const char *title, const char *query) {
    size_t title_len = strlen(title), query_len = strlen(query);
    if (query_len == 0) return true;
    if (query_len > title_len) return false;
    for (size_t i = 0; i + query_len <= title_len; i++) {
        if (strncasecmp(title + i, query, query_len) == 0) return true;
    }
    return false;
}

static int build_visible(const AppEntry *entries, int count, const char *category_filter,
                          const char *search_query, int *out_visible) {
    int n = 0;
    for (int i = 0; i < count && n < VISIBLE_MAX; i++) {
        if (category_filter[0] != '\0' && strcmp(entries[i].category, category_filter) != 0) continue;
        if (!title_contains(entries[i].title, search_query)) continue;
        out_visible[n++] = i;
    }
    return n;
}

// Called right after view_mode/sort_mode/category_filter change - saves
// immediately rather than batching for some "on exit" point, since B/+/HOME
// can end the process at any time with no reliable hook to save from then.
static void save_prefs(ViewMode view_mode, SortMode sort_mode, const char *category_filter) {
    UiListPrefs prefs;
    prefs.view_mode = (int)view_mode;
    prefs.sort_mode = (int)sort_mode;
    snprintf(prefs.category_filter, sizeof(prefs.category_filter), "%s", category_filter);
    ui_prefs_save(&prefs);
}

static const char *empty_state_message(int count, const char *category_filter, const char *search_query) {
    if (count == 0) return "(el catálogo está vacío)";
    if (search_query[0] && category_filter[0]) return "(sin resultados para esta búsqueda en esta categoría)";
    if (search_query[0]) return "(sin resultados para esta búsqueda)";
    return "(sin apps en esta categoría)";
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

// Highlight box padding around the icon+title, symmetric on every side -
// was previously sized off GRID_CELL_W - GRID_GAP (208px), as if that were
// the icon's width, but the icon is GRID_ICON_SIZE (168px) and sits at the
// cell's left edge, not centered in that wider span - the highlight ended
// up ~6px past the icon on the left but ~46px past it on the right.
#define GRID_SELECT_PAD 8
// Below the icon: the 6px gap before the title (see the ui_draw_text call
// below) plus roughly one g_font_small (16pt) line.
#define GRID_SELECT_TITLE_H 28

// How much the selected cell's icon grows (12%) - GRID_CELL_W - GRID_ICON_SIZE
// (56px) is the slack between icons before the next column starts, so this
// has plenty of room without touching a neighboring cell.
#define GRID_ZOOM_TARGET 1.12f
// Eased toward GRID_ZOOM_TARGET by this fraction of the remaining distance
// each drawn frame - at ~60fps this settles in well under a second, fast
// enough to feel responsive to cursor movement rather than sluggish.
#define GRID_ZOOM_EASE 0.35f

// `zoom` (1.0 = no scaling) grows the icon in place - only meaningful when
// is_selected, callers pass 1.0 for every other cell. Anchored to the
// icon's bottom edge (grows up and sideways, never down) rather than
// centered on all four sides, so it never grows into the title drawn right
// below it. The highlight box and title stay fixed size, so it reads as the
// icon popping slightly out of its frame rather than the whole cell resizing.
static void draw_grid_cell(int x, int y, const AppEntry *entry, bool is_selected, float zoom) {
    if (is_selected) {
        int box_w = GRID_ICON_SIZE + GRID_SELECT_PAD * 2;
        int box_h = GRID_ICON_SIZE + GRID_SELECT_TITLE_H + GRID_SELECT_PAD * 2;
        ui_draw_rect(x - GRID_SELECT_PAD, y - GRID_SELECT_PAD, box_w, box_h, COLOR_ACCENT);
    }

    int icon_size = (int)(GRID_ICON_SIZE * zoom);
    int grow = icon_size - GRID_ICON_SIZE;
    SDL_Rect icon_rect = { x - grow / 2, y - grow, icon_size, icon_size };

    ui_draw_rect(icon_rect.x, icon_rect.y, icon_rect.w, icon_rect.h, COLOR_PANEL);

    SDL_Texture *icon = ui_icons_get(entry);
    if (icon) {
        SDL_RenderCopy(g_renderer, icon, NULL, &icon_rect);
    } else {
        // Placeholder while the icon loads (or if it never resolves): the
        // title's first letter, centered-ish, so the grid isn't just blank
        // panels before icons finish fetching.
        char initial[2] = { entry->title[0] ? entry->title[0] : '?', '\0' };
        ui_draw_text(g_font_title, icon_rect.x + icon_rect.w / 2 - 8, icon_rect.y + icon_rect.h / 2 - 16,
                     COLOR_TEXT_DIM, initial);
    }

    char title[64];
    truncate_to_width(g_font_small, entry->title, GRID_TITLE_MAX_W, title, sizeof(title));
    SDL_Color title_color = is_selected ? COLOR_TEXT : COLOR_TEXT_DIM;
    ui_draw_text(g_font_small, x, y + GRID_ICON_SIZE + 6, title_color, title);
}

// No "show everything" tab - a catalog is expected to always have at least
// one category, so category_filter always names a real one (see the
// still-valid check in ui_show_list, which defaults it to categories[0]
// whenever it's empty or stale). Returns the matching tab index, or 0 as a
// safe fallback if category_filter doesn't (yet) match anything.
static int category_tab_index(const char categories[][APP_ENTRY_CATEGORY_MAX], int category_count,
                               const char *category_filter) {
    for (int i = 0; i < category_count; i++) {
        if (strcmp(categories[i], category_filter) == 0) return i;
    }
    return 0;
}

// Always-visible horizontal tab strip (replaces the old L-toggled sidebar -
// users reported categories were too easy to miss tucked behind a hidden
// panel). The active tab gets a brighter label plus an accent underline so
// it's unambiguous which catalog is showing, matching how a plain color
// swatch could get lost at a glance. ZL/ZR step between tabs one at a time -
// button-hint boxes in both corners show which ones, since that's not
// otherwise obvious from the tab strip alone. Since more categories can
// exist than fit across 1280px, `tab_scroll_start` (persisted by the caller
// across frames) tracks which tab the strip currently starts rendering
// from, auto-advancing to keep the active tab in view. Returns the
// (possibly adjusted) tab_scroll_start for the caller to keep.
static int draw_category_tabs(const char categories[][APP_ENTRY_CATEGORY_MAX], int category_count,
                               const char *category_filter, int tab_scroll_start) {
    ui_draw_rect(LEFT_EDGE, TAB_BAR_Y, RIGHT_EDGE - LEFT_EDGE, TAB_BAR_H, COLOR_PANEL);

    // ZL/ZR button-hint boxes, bordered like the old sidebar's selection
    // box - always drawn the same way (not dimmed/disabled), since they
    // always do something: cycling wraps around rather than stopping.
    int box_y = TAB_BAR_Y + (TAB_BAR_H - TAB_BAR_ARROW_BOX) / 2;
    int left_box_x = LEFT_EDGE + 6;
    int right_box_x = RIGHT_EDGE - 6 - TAB_BAR_ARROW_BOX;
    ui_draw_rect(left_box_x, box_y, TAB_BAR_ARROW_BOX, TAB_BAR_ARROW_BOX, COLOR_ACCENT);
    ui_draw_rect(left_box_x + 2, box_y + 2, TAB_BAR_ARROW_BOX - 4, TAB_BAR_ARROW_BOX - 4, COLOR_PANEL);
    ui_draw_text(g_font_small, left_box_x + 4, box_y + 7, COLOR_TEXT, "ZL");
    ui_draw_rect(right_box_x, box_y, TAB_BAR_ARROW_BOX, TAB_BAR_ARROW_BOX, COLOR_ACCENT);
    ui_draw_rect(right_box_x + 2, box_y + 2, TAB_BAR_ARROW_BOX - 4, TAB_BAR_ARROW_BOX - 4, COLOR_PANEL);
    ui_draw_text(g_font_small, right_box_x + 4, box_y + 7, COLOR_TEXT, "ZR");

    if (category_count == 0) {
        ui_draw_text(g_font_small, left_box_x + TAB_BAR_ARROW_BOX + 16, TAB_BAR_Y + 8,
                     COLOR_TEXT_DIM, "(sin categorías)");
        return 0;
    }

    int current_index = category_tab_index(categories, category_count, category_filter);

    int content_left = LEFT_EDGE + TAB_BAR_ARROW_W;
    int content_right = RIGHT_EDGE - TAB_BAR_ARROW_W;
    int content_w = content_right - content_left;

    if (current_index < tab_scroll_start) tab_scroll_start = current_index;
    for (;;) {
        int x = 0;
        bool fits_current = false;
        for (int i = tab_scroll_start; i < category_count; i++) {
            int w, h;
            TTF_SizeUTF8(g_font_body, categories[i], &w, &h);
            int tab_w = w + TAB_BAR_PAD_X * 2;
            if (x + tab_w > content_w) break;
            x += tab_w;
            if (i == current_index) fits_current = true;
        }
        if (fits_current || tab_scroll_start >= current_index) break;
        tab_scroll_start++;
    }

    int x = content_left;
    for (int i = tab_scroll_start; i < category_count; i++) {
        int w, h;
        TTF_SizeUTF8(g_font_body, categories[i], &w, &h);
        int tab_w = w + TAB_BAR_PAD_X * 2;
        if (x + tab_w > content_right) break;

        bool is_active = (i == current_index);
        ui_draw_text(g_font_body, x + TAB_BAR_PAD_X, TAB_BAR_Y + 8,
                     is_active ? COLOR_TEXT : COLOR_TEXT_DIM, categories[i]);
        if (is_active) {
            ui_draw_rect(x, TAB_BAR_Y + TAB_BAR_H - TAB_BAR_UNDERLINE_H, tab_w, TAB_BAR_UNDERLINE_H, COLOR_ACCENT);
        }
        if (i + 1 < category_count) {
            ui_draw_rect(x + tab_w, TAB_BAR_Y + 6, 1, TAB_BAR_H - 12, COLOR_BG);
        }
        x += tab_w;
    }

    return tab_scroll_start;
}

int ui_show_list(AppEntry *entries, int count) {
    static ViewMode view_mode = VIEW_LIST;
    static SortMode sort_mode = SORT_TITLE;
    static char category_filter[APP_ENTRY_CATEGORY_MAX] = "";
    static char search_query[64] = "";

    // Only on the very first call this process - view_mode/sort_mode/
    // category_filter are already persisted in-memory (via `static`) across
    // re-entries from the detail screen, same as selected/scroll_offset.
    static bool prefs_loaded_once = false;
    if (!prefs_loaded_once) {
        prefs_loaded_once = true;
        UiListPrefs prefs;
        ui_prefs_load(&prefs);
        if (prefs.view_mode == VIEW_LIST || prefs.view_mode == VIEW_GRID) {
            view_mode = (ViewMode)prefs.view_mode;
        }
        if (prefs.sort_mode >= 0 && prefs.sort_mode < SORT_MODE_COUNT) {
            sort_mode = (SortMode)prefs.sort_mode;
        }
        snprintf(category_filter, sizeof(category_filter), "%s", prefs.category_filter);
        // sort_mode otherwise only actually gets applied to `entries` when
        // the user next presses X - without this, a loaded non-default
        // sort would sit unapplied (array order untouched) while the
        // footer already claims it's active.
        apply_sort(entries, count, sort_mode);
    }

    char categories[MAX_CATEGORIES][APP_ENTRY_CATEGORY_MAX];
    int category_count = collect_categories(entries, count, categories);

    // No "Todos" tab - category_filter should always name a real category.
    // Default it to the first one whenever it's empty (first launch, no
    // saved preference yet) or stale (the catalog changed since - sources
    // reload, or the category itself got renamed/removed) rather than
    // leaving it pointing at nothing.
    {
        bool still_valid = false;
        for (int i = 0; i < category_count; i++) {
            if (strcmp(categories[i], category_filter) == 0) {
                still_valid = true;
                break;
            }
        }
        if (!still_valid) {
            if (category_count > 0) {
                snprintf(category_filter, sizeof(category_filter), "%s", categories[0]);
            } else {
                category_filter[0] = '\0';
            }
        }
    }

    int visible[VISIBLE_MAX];
    int visible_count = build_visible(entries, count, category_filter, search_query, visible);

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

    // Grid view's selected-icon "pop": eases toward GRID_ZOOM_TARGET each
    // frame rather than jumping there instantly, and resets to 1.0
    // whenever the selection changes so every new focus re-triggers the pop
    // instead of picking up mid-animation. Persisted across calls like
    // `selected` above, for the same reason.
    static float grid_zoom = 1.0f;
    static int grid_zoom_selected = -1;

    // Independent auto-repeat timing per direction - see nav_repeat_step().
    static NavRepeatState nav_up = {0}, nav_down = {0}, nav_left = {0}, nav_right = {0};

    // The catalog/filter/sort can differ from the last time this screen was
    // shown (sources reload, or view_mode/category changed elsewhere) -
    // clamp rather than trust the previous position blindly. scroll_offset
    // doesn't need its own clamp: the per-frame scroll-follow logic below
    // already derives it from `selected` unconditionally.
    if (selected >= visible_count) selected = visible_count > 0 ? visible_count - 1 : 0;
    if (selected < 0) selected = 0;

    // Which tab the always-visible category strip currently starts
    // rendering from - not persisted across re-entries (recomputed to keep
    // the active tab in view the moment this screen is shown again).
    int tab_scroll_start = 0;

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

        bool show_full_title = visible_count > 0;

        {
            int cols = (view_mode == VIEW_GRID) ? GRID_COLS : 1;

            // Held state for repeat purposes - D-Pad or left stick, either
            // one counts. (Stick Y follows the usual up=positive convention;
            // if this ends up inverted on real hardware it's this one sign
            // that needs flipping.)
            u64 kHeld = padGetButtons(&pad);
            HidAnalogStickState stick = padGetStickPos(&pad, 0);
            bool held_up = (kHeld & HidNpadButton_Up) || stick.y > NAV_STICK_DEADZONE;
            bool held_down = (kHeld & HidNpadButton_Down) || stick.y < -NAV_STICK_DEADZONE;
            bool held_left = (kHeld & HidNpadButton_Left) || stick.x < -NAV_STICK_DEADZONE;
            bool held_right = (kHeld & HidNpadButton_Right) || stick.x > NAV_STICK_DEADZONE;

            if (nav_repeat_step(&nav_down, held_down, now_tick)) {
                // A straight "selected+cols < visible_count" check refuses
                // to move at all once the target row is the last one and
                // it's shorter than a full row (visible_count isn't a
                // multiple of cols) - the exact column below simply doesn't
                // exist there. Move to it when it exists, otherwise land on
                // the last item instead of not moving - but only when
                // there's a row below at all (selected isn't already in the
                // last, possibly-partial, row).
                int last_row_start = ((visible_count - 1) / cols) * cols;
                if (selected < last_row_start) {
                    int target = selected + cols;
                    selected = (target < visible_count) ? target : visible_count - 1;
                }
            }
            if (nav_repeat_step(&nav_up, held_up, now_tick)) {
                if (selected - cols >= 0) selected -= cols;
            }
            if (view_mode == VIEW_GRID) {
                if (nav_repeat_step(&nav_right, held_right, now_tick) && selected < visible_count - 1) selected++;
                if (nav_repeat_step(&nav_left, held_left, now_tick) && selected > 0) selected--;
            } else {
                // List view doesn't use left/right - keep their repeat
                // timers from carrying stale state into grid view if the
                // user switches while holding one (Y toggles view_mode
                // without waiting for buttons to be released).
                nav_left.was_held = false;
                nav_right.was_held = false;
            }
            if (kDown & HidNpadButton_A) {
                if (visible_count > 0) return visible[selected];
            }
            if (kDown & HidNpadButton_Minus) {
                return UI_LIST_OPEN_SOURCES;
            }
            if (kDown & HidNpadButton_L) {
                return UI_LIST_OPEN_ABOUT;
            }
            if (kDown & HidNpadButton_StickR) {
                return UI_LIST_OPEN_EXPLORER;
            }
            if ((kDown & HidNpadButton_B) || (kDown & HidNpadButton_Plus)) {
                return UI_LIST_EXIT;
            }
            if (kDown & HidNpadButton_Y) {
                // Same `entries`/`selected` index means the same app in both
                // views - only the per-mode row/scroll math needs resetting.
                view_mode = (view_mode == VIEW_LIST) ? VIEW_GRID : VIEW_LIST;
                scroll_offset = 0;
                save_prefs(view_mode, sort_mode, category_filter);
            }
            if (kDown & HidNpadButton_X) {
                sort_mode = (SortMode)((sort_mode + 1) % SORT_MODE_COUNT);
                apply_sort(entries, count, sort_mode);
                // Sorting reorders `entries` in place - the set of ids
                // passing the filter is unchanged, but their positions are,
                // so `visible` must be rebuilt against the new order.
                visible_count = build_visible(entries, count, category_filter, search_query, visible);
                selected = 0;
                scroll_offset = 0;
                save_prefs(view_mode, sort_mode, category_filter);
            }
            if ((kDown & (HidNpadButton_ZL | HidNpadButton_ZR)) && category_count > 0) {
                // Steps directly to the next/previous category tab, no
                // "Todos" - wraps around at either end.
                int current_index = category_tab_index(categories, category_count, category_filter);
                int step = (kDown & HidNpadButton_ZL) ? -1 : 1;
                int new_index = (current_index + step + category_count) % category_count;
                snprintf(category_filter, sizeof(category_filter), "%s", categories[new_index]);
                visible_count = build_visible(entries, count, category_filter, search_query, visible);
                selected = 0;
                scroll_offset = 0;
                save_prefs(view_mode, sort_mode, category_filter);
            }
            if (kDown & HidNpadButton_R) {
                SwkbdConfig kbd;
                if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
                    swkbdConfigMakePresetDefault(&kbd);
                    swkbdConfigSetHeaderText(&kbd, "Buscar en el catálogo");
                    swkbdConfigSetGuideText(&kbd, "Título del juego/app (vacío para quitar el filtro)");
                    swkbdConfigSetInitialText(&kbd, search_query);
                    swkbdConfigSetStringLenMax(&kbd, sizeof(search_query) - 1);

                    char typed[sizeof(search_query)];
                    if (R_SUCCEEDED(swkbdShow(&kbd, typed, sizeof(typed)))) {
                        snprintf(search_query, sizeof(search_query), "%s", typed);
                        visible_count = build_visible(entries, count, category_filter, search_query, visible);
                        selected = 0;
                        scroll_offset = 0;
                    }
                    swkbdClose(&kbd);
                }
                // The keyboard applet takes over the screen - the baseline
                // read at the top of this function is now stale (whatever
                // was held when swkbd closed would otherwise be misread as
                // newly pressed on the very next frame).
                padUpdate(&pad);
            }
        }

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, "FreeShop");
        if (search_query[0]) {
            char header_line[96];
            snprintf(header_line, sizeof(header_line), "- Catálogo - buscando \"%s\"", search_query);
            ui_draw_text(g_font_body, 210, HEADER_Y + 4, COLOR_TEXT_DIM, header_line);
        } else {
            ui_draw_text(g_font_body, 210, HEADER_Y + 4, COLOR_TEXT_DIM, "- Catálogo");
        }
        ui_draw_text_right(g_font_body, RIGHT_EDGE, STATUS_Y, COLOR_TEXT, status_line);

        draw_storage_panel(PANEL_SD_X, PANEL_Y, PANEL_W, PANEL_H, "Tarjeta SD",
                            storage.sd_ok, storage.sd_total, storage.sd_free);
        draw_storage_panel(PANEL_NAND_X, PANEL_Y, PANEL_W, PANEL_H, "NAND",
                            storage.nand_ok, storage.nand_total, storage.nand_free);

        tab_scroll_start = draw_category_tabs(categories, category_count, category_filter, tab_scroll_start);

        if (view_mode == VIEW_LIST) {
            ui_draw_rect(LEFT_EDGE, COL_HEADER_Y, RIGHT_EDGE - LEFT_EDGE, COL_HEADER_H, COLOR_PANEL);
            ui_draw_text(g_font_small, COL_NAME_X, COL_HEADER_Y + 8, COLOR_TEXT_DIM, "Nombre");
            ui_draw_text(g_font_small, COL_TYPE_X, COL_HEADER_Y + 8, COLOR_TEXT_DIM, "Tipo");
            ui_draw_text(g_font_small, COL_VERSION_X, COL_HEADER_Y + 8, COLOR_TEXT_DIM, "Versión");
            ui_draw_text(g_font_small, COL_CATEGORY_X, COL_HEADER_Y + 8, COLOR_TEXT_DIM, "Categoría");
            ui_draw_text(g_font_small, COL_SIZE_X, COL_HEADER_Y + 8, COLOR_TEXT_DIM, "Tamaño");

            if (visible_count == 0) {
                ui_draw_text(g_font_body, COL_NAME_X, LIST_TOP, COLOR_TEXT_DIM,
                             empty_state_message(count, category_filter, search_query));
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

            if (selected != grid_zoom_selected) {
                grid_zoom = 1.0f;
                grid_zoom_selected = selected;
            }
            grid_zoom += (GRID_ZOOM_TARGET - grid_zoom) * GRID_ZOOM_EASE;

            int selected_row = selected / GRID_COLS;
            if (selected_row < scroll_offset) scroll_offset = selected_row;
            if (selected_row >= scroll_offset + GRID_ROWS_VISIBLE) scroll_offset = selected_row - GRID_ROWS_VISIBLE + 1;

            if (visible_count == 0) {
                ui_draw_text(g_font_body, GRID_LEFT, GRID_TOP, COLOR_TEXT_DIM,
                             empty_state_message(count, category_filter, search_query));
            }

            int first = scroll_offset * GRID_COLS;
            int last = first + GRID_ROWS_VISIBLE * GRID_COLS;
            for (int vi = first; vi < visible_count && vi < last; vi++) {
                int i = visible[vi];
                int row_in_view = (vi / GRID_COLS) - scroll_offset;
                int col = vi % GRID_COLS;
                int cell_x = GRID_LEFT + col * GRID_CELL_W;
                int cell_y = GRID_TOP + row_in_view * GRID_CELL_H;
                bool is_selected = vi == selected;
                draw_grid_cell(cell_x, cell_y, &entries[i], is_selected, is_selected ? grid_zoom : 1.0f);
            }
        }

        // Titles get cut short with "..." to fit the grid's narrow cells
        // (and could in the list view too, for a long enough one) - always
        // show the selected entry's full title here instead of guessing
        // from the truncated version.
        if (show_full_title) {
            const AppEntry *selected_entry = &entries[visible[selected]];
            ui_draw_rect(LEFT_EDGE, FOOTER_Y - 40, RIGHT_EDGE - LEFT_EDGE, 26, COLOR_PANEL);
            ui_draw_text(g_font_body, LEFT_EDGE + 10, FOOTER_Y - 36, COLOR_TEXT, selected_entry->title);
        }

        ui_draw_rect(LEFT_EDGE, FOOTER_Y - 10, RIGHT_EDGE - LEFT_EDGE, 1, COLOR_PANEL);
        // Split across two lines - all of this crammed onto one (as it used
        // to be) ran wide enough to overflow past RIGHT_EDGE at 1280px,
        // silently clipping whatever came after the overflow point off the
        // edge of the screen (ui_draw_text doesn't wrap or clip-warn) -
        // that's what made "Stick R: explorador" undiscoverable, not the
        // button itself.
        char footer1[160];
        snprintf(footer1, sizeof(footer1),
                 "D-Pad: navegar    A: instalar    ZL/ZR: categoría    Y: vista %s    X: ordenar (%s)",
                 view_mode == VIEW_LIST ? "cuadrícula" : "lista", sort_mode_label(sort_mode));
        ui_draw_text(g_font_small, LEFT_EDGE, FOOTER_Y, COLOR_TEXT_DIM, footer1);

        char footer2[160];
        snprintf(footer2, sizeof(footer2),
                 "R: buscar%s    -: fuentes    L: acerca de    Stick R: explorador    B/+: salir",
                 search_query[0] ? " (activa)" : "");
        ui_draw_text(g_font_small, LEFT_EDGE, FOOTER_Y + 20, COLOR_TEXT_DIM, footer2);

        SDL_RenderPresent(g_renderer);
    }

    return UI_LIST_EXIT;
}
