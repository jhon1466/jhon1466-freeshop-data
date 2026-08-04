#include "ui_list.h"
#include "ui_app.h"
#include "ui_storage.h"
#include "ui_status.h"
#include "ui_icons.h"
#include "ui_prefs.h"
#include "ui_nav.h"
#include "ui_queue.h"
#include "ui_sound.h"
#include "ui_fx.h"
#include "../i18n.h"

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SCREEN_W 1280
#define SCREEN_H 720

#define RIGHT_EDGE (SCREEN_W - UI_FRAME_FOOTER_PAD_SIDES)
#define LEFT_EDGE UI_FRAME_FOOTER_PAD_SIDES

// ---- Borealis TabFrame shell ----
//
// This screen's chrome is Borealis's, not a layout of our own: an
// AppletFrame header band (title left, clock/battery right, separator rule
// under it), a flush-left full-height sidebar of sections, and a footer rule
// with right-aligned button hints. The metrics come from Borealis's own
// registered style values (see ui_app.h's block for the list) and from
// pipensx's main_frame.hpp, which sets brls/tab_frame/sidebar_width to 248
// and collapses it to an 88px icon rail while the content has focus.
//
// The two large SD/NAND storage panels this screen used to devote its whole
// top-right to are gone: pipensx shows storage as a compact meter where it
// is actually actionable (its game detail screen, next to the install
// button), not as permanent furniture on the catalog. The same information
// now lives in a small meter pinned to the bottom of the sidebar.
#define HEADER_Y 28
#define STATUS_Y 34

#define SIDEBAR_X 0
#define SIDEBAR_W_EXPANDED 248
#define SIDEBAR_W_COLLAPSED 88
#define SIDEBAR_GAP 30
#define SIDEBAR_Y (UI_FRAME_HEADER_H + 1)
#define SIDEBAR_BOTTOM (SCREEN_H - UI_FRAME_FOOTER_H)
#define SIDEBAR_ITEM_H 56
#define SIDEBAR_ICON_SIZE 22
#define SIDEBAR_PAD_LEFT 22
// Compact storage meters pinned to the sidebar's bottom - the replacement
// for the old top-right panels (see the block comment above).
#define SIDEBAR_METER_H 34
#define SIDEBAR_METERS_H (SIDEBAR_METER_H * 2 + 10)

#define CONTENT_TOP (UI_FRAME_HEADER_H + 20)

// Everything content-related (tab bar, column header, grid, list rows) is
// anchored off a runtime `content_left` local instead of the page's own
// LEFT_EDGE, to make room for the sidebar and reflow when it collapses -
// LEFT_EDGE itself keeps meaning "page edge" and still anchors the
// header/footer bars and full-title bar, which span the sidebar's width too
// (see their draw calls below). Column positions below are offsets *from*
// content_left (added to it at the one place each is used, in ui_show_list)
// rather than absolute coordinates, for the same reflow-on-collapse reason.

// Always-visible horizontal category tab bar, directly under the header
// rule now that the storage panels no longer occupy that band.
#define TAB_BAR_Y CONTENT_TOP
#define TAB_BAR_H 40
#define TAB_BAR_ARROW_BOX 30
#define TAB_BAR_ARROW_W (TAB_BAR_ARROW_BOX + 16)
#define TAB_BAR_PAD_X 18
#define TAB_BAR_UNDERLINE_H 4

#define COL_HEADER_Y (TAB_BAR_Y + TAB_BAR_H + 10)
#define COL_HEADER_H 34
#define LIST_TOP (COL_HEADER_Y + COL_HEADER_H + 10)
#define ROW_HEIGHT 42
// Derived rather than fixed now: reclaiming the storage-panel band gave the
// content area ~50px back, and hard-coding 9 would have left that as dead
// space at the bottom instead of another row of results.
#define VISIBLE_ROWS ((SCREEN_H - UI_FRAME_FOOTER_H - 16 - LIST_TOP) / ROW_HEIGHT)

#define COL_NAME_OFFSET 20
// Names have to stop short of the "Tipo" column - titles from a raw-folder
// source are filenames, which are routinely long enough to run straight
// through every column to its right. The two offsets' difference is the
// same regardless of content_left, so this one stays a plain constant.
#define COL_NAME_MAX_W (COL_TYPE_OFFSET - COL_NAME_OFFSET - 20)
#define COL_TYPE_OFFSET 500
#define COL_VERSION_OFFSET 600
#define COL_CATEGORY_OFFSET 700
#define COL_SIZE_OFFSET 870
// Same reasoning as COL_NAME_MAX_W: a source-provided value (a real app
// version string, a scraped category name) has no length guarantee, so
// without clipping a long one bleeds into whatever column sits to its
// right instead of just running up against it.
#define COL_VERSION_MAX_W (COL_CATEGORY_OFFSET - COL_VERSION_OFFSET - 12)
#define COL_CATEGORY_MAX_W (COL_SIZE_OFFSET - COL_CATEGORY_OFFSET - 12)

// Grid view (Y toggles list <-> grid). Fixed constants tuned for the fixed
// 1280x720 layout this whole screen already assumes (see SCREEN_W/H above) -
// not derived dynamically, matching the rest of this file's style. 5
// columns at 192px each (icons 144px, same ~25% zoom/growth slack ratio as
// before) fit the narrower content area left over once the sidebar takes
// its space even with the sidebar expanded (the worst case - collapsed
// leaves even more room): content width there is ~980px, 5*192=960 fits
// with margin to spare. grid_left centers the row within whatever width is
// actually available, so both sidebar states just work without a separate
// runtime branch.
#define GRID_TOP COL_HEADER_Y
#define GRID_COLS 5
#define GRID_ROWS_VISIBLE 2
#define GRID_CELL_W 192
#define GRID_CELL_H 224
// pipensx's own cover art is 180x180 - this stops short of that (160) rather
// than matching it exactly: GRID_CELL_W's 192 only leaves 192-icon_size of
// slack before the next column starts, and the focused card's GRID_ZOOM_TARGET
// (12% pop) needs headroom out of that same slack too - at a literal 180 the
// zoomed card would grow past the next column's cell. 160 keeps 32px of
// slack, comfortably more than the ~19px the zoom needs, without widening
// GRID_CELL_W past what 5 columns already fits in the sidebar-expanded
// worst case (~980px content width, see the comment above GRID_TOP).
#define GRID_ICON_SIZE 160
// Rounded-corner radius for grid cover art (see ui_mask_rounded_corners) -
// pipensx's own cover art uses exactly this ("medium" rounding, 8px - see
// its src/ui/theme.hpp's kRadiusMedium, confirmed against catalog_grid.hpp's
// own cover art radius) rather than a value picked to just look OK here.
#define GRID_ICON_RADIUS 8
#define GRID_GAP 16
#define GRID_TITLE_MAX_W (GRID_ICON_SIZE)

// Vertically centered in Borealis's 73px footer band (below its rule).
#define FOOTER_Y (SCREEN_H - UI_FRAME_FOOTER_H + UI_FRAME_FOOTER_H / 2 - 12)

// A generous fixed cap on distinct categories, matching this file's
// existing style (VISIBLE_ROWS/GRID_COLS etc. are fixed too) rather than
// dynamically sizing for the 1280x720 layout this whole screen assumes.
#define MAX_CATEGORIES 16

// Indices into `entries` that pass the current category filter. Used to be
// 256 on the assumption a homebrew catalog would never be huge - the
// switch-games torrent catalog alone (see sources.h's
// SOURCE_KIND_TORRENT_CATALOG) carries several thousand entries, and 256
// silently cut the list off partway through with no indication anything
// was missing. `visible` (below) is `static`, so this costs BSS, not
// stack - 8192 ints is 32KB, nothing.
#define VISIBLE_MAX 8192

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
        case SORT_CATEGORY: return tr(STR_LIST_SORT_CATEGORY);
        case SORT_VERSION: return tr(STR_LIST_SORT_VERSION);
        default: return tr(STR_LIST_SORT_TITLE);
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

// The catalog home screen (pipensx-style, see ui_show_list's shelf-home
// state below) shows a handful of horizontally-scrolling shelves instead of
// one flat grid. pipensx's own default shelves ("Popular" by peer count,
// "New"/"Recently Updated" by precise publish/update timestamps) need
// catalog metadata this app's sources don't carry - AppEntry has no peer
// count and no publish timestamp finer than the bare year catalog.c's
// copy_year_field extracts. So instead: one "Recién agregados" shelf (every
// root entry, newest year first) plus one shelf per real category, which is
// data this catalog actually has.
#define MAX_SHELVES 8
// Bounded window per shelf so building/redrawing them is cheap regardless of
// catalog size - "Ver todo" (the trailing virtual card every shelf gets,
// see draw_shelf) is the escape hatch into the rest via the existing
// flat grid/list and its own category_filter.
#define SHELF_MAX_ITEMS 20

typedef struct {
    char label[APP_ENTRY_CATEGORY_MAX + 16];
    // "" for the "Recién agregados" shelf (spans every category); a real
    // category name for the rest - fed straight into build_visible()'s
    // category_filter when its "Ver todo" card is picked.
    char category_filter[APP_ENTRY_CATEGORY_MAX];
    int indices[SHELF_MAX_ITEMS];
    int count;
} Shelf;

// Sorts a small index array by entries[i].version descending (newest year
// first) - insertion sort, SHELF_MAX_ITEMS is tiny, not worth qsort's
// comparator ceremony for this. Same string-compare basis cmp_by_version
// already uses (4-digit years compare correctly as plain strings), just the
// opposite direction.
static void sort_indices_by_version_desc(const AppEntry *entries, int *indices, int n) {
    for (int i = 1; i < n; i++) {
        int key = indices[i];
        const char *key_version = entries[key].version;
        int j = i;
        while (j > 0 && strcasecmp(entries[indices[j - 1]].version, key_version) < 0) {
            indices[j] = indices[j - 1];
            j--;
        }
        indices[j] = key;
    }
}

static int build_shelves(const AppEntry *entries, int count,
                          const char categories[][APP_ENTRY_CATEGORY_MAX], int category_count,
                          Shelf *out_shelves) {
    int n = 0;

    Shelf *recent = &out_shelves[n];
    snprintf(recent->label, sizeof(recent->label), "%s", tr(STR_LIST_SHELF_RECENT));
    recent->category_filter[0] = '\0';
    recent->count = 0;
    for (int i = 0; i < count && recent->count < SHELF_MAX_ITEMS; i++)
        recent->indices[recent->count++] = i;
    sort_indices_by_version_desc(entries, recent->indices, recent->count);
    if (recent->count > 0) n++;

    for (int c = 0; c < category_count && n < MAX_SHELVES; c++) {
        Shelf *shelf = &out_shelves[n];
        snprintf(shelf->label, sizeof(shelf->label), "%s", categories[c]);
        snprintf(shelf->category_filter, sizeof(shelf->category_filter), "%s", categories[c]);
        shelf->count = 0;
        for (int i = 0; i < count && shelf->count < SHELF_MAX_ITEMS; i++) {
            if (strcmp(entries[i].category, categories[c]) == 0)
                shelf->indices[shelf->count++] = i;
        }
        if (shelf->count > 0) n++;
    }

    return n;
}

// Called right after view_mode/sort_mode/category_filter change - saves
// immediately rather than batching for some "on exit" point, since B/+/HOME
// can end the process at any time with no reliable hook to save from then.
static void save_prefs(ViewMode view_mode, SortMode sort_mode, const char *category_filter,
                        bool sidebar_collapsed) {
    // Effects/sound are owned by ui_fx/ui_sound rather than tracked here -
    // read them back from there so saving a view/sort change never clobbers
    // whatever the user set them to.
    UiListPrefs prefs;
    prefs.view_mode = (int)view_mode;
    prefs.sort_mode = (int)sort_mode;
    snprintf(prefs.category_filter, sizeof(prefs.category_filter), "%s", category_filter);
    prefs.effects_disabled = !ui_fx_enabled();
    prefs.sound_disabled = !ui_sound_enabled();
    prefs.sidebar_collapsed = sidebar_collapsed;
    ui_prefs_save(&prefs);
}

static const char *empty_state_message(int count, const char *category_filter, const char *search_query) {
    if (count == 0) return tr(STR_LIST_EMPTY_CATALOG);
    if (search_query[0] && category_filter[0]) return tr(STR_LIST_EMPTY_SEARCH_IN_CATEGORY);
    if (search_query[0]) return tr(STR_LIST_EMPTY_SEARCH);
    return tr(STR_LIST_EMPTY_CATEGORY);
}

// (The two large boxed SD/NAND gauges that used to live across this
// screen's top-right band are gone - see the Borealis TabFrame shell block
// at the top of this file. The same figures are now drawn by
// draw_sidebar_meter at the bottom of the sidebar.)

static const char *file_type_label(AppFileType type) {
    if (type == APP_FILE_TYPE_NSP) return "NSP";
    if (type == APP_FILE_TYPE_XCI) return "XCI";
    if (type == APP_FILE_TYPE_NSZ) return "NSZ";
    if (type == APP_FILE_TYPE_PORT) return "Port";
    return "NRO";
}

static void format_size(long bytes, char *out, size_t out_size) {
    // A raw-directory source can legitimately not know a file's size (the
    // server answered neither a HEAD nor a ranged GET with one - see
    // catalog.c's try_fetch_raw_directory). Showing "0.0 MB" there reads as
    // "this file is empty" rather than "not reported".
    if (bytes <= 0) {
        snprintf(out, out_size, "-");
        return;
    }
    if (bytes >= 1024L * 1024L * 1024L) {
        snprintf(out, out_size, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        return;
    }
    snprintf(out, out_size, "%.1f MB", bytes / (1024.0 * 1024.0));
}

#define STATUS_ICON_H 12
#define STATUS_GAP 10
#define STATUS_ICON_TEXT_GAP 5

// Draws the clock, a WiFi/Ethernet signal icon + label, and a battery gauge
// icon + percentage, right-aligned as one group ending at `right_x` - the
// console's own status-bar chips (see ui_draw_wifi_icon/ui_draw_battery_icon)
// instead of the words "WiFi" and a bare "87%" this used to be. Text pieces
// are measured first so the whole group can be laid out right-to-left in one
// pass, the same trick ui_draw_text_right uses for a single string.
static void draw_status_bar(int right_x, int y, const SystemStatus *status) {
    const char *clock = status->clock[0] ? status->clock : "--:--:--";
    const char *net = status->network_ok ? status->network_label : tr(STR_LIST_NO_CONNECTION);

    int clock_w = 0, h = 0;
    TTF_SizeUTF8(g_font_body, clock, &clock_w, &h);
    int net_w = 0;
    TTF_SizeUTF8(g_font_body, net, &net_w, &h);

    char battery_text[8];
    int battery_text_w = 0;
    if (status->battery_ok) {
        snprintf(battery_text, sizeof(battery_text), "%u%%", status->battery_percent);
        TTF_SizeUTF8(g_font_body, battery_text, &battery_text_w, &h);
    }

    const int wifi_icon_w = STATUS_ICON_H; // ui_draw_wifi_icon's 4 bars land within a roughly square footprint
    const int battery_icon_w = 20;

    int x = right_x;
    if (status->battery_ok) {
        x -= battery_text_w;
        ui_draw_text(g_font_body, x, y, COLOR_TEXT, battery_text);
        x -= STATUS_ICON_TEXT_GAP + battery_icon_w;
        ui_draw_battery_icon(x, y + (h - STATUS_ICON_H) / 2 + 2, battery_icon_w, STATUS_ICON_H,
                             (int)status->battery_percent, status->charging, true);
        x -= STATUS_GAP;
    }

    x -= net_w;
    ui_draw_text(g_font_body, x, y, COLOR_TEXT, net);
    x -= STATUS_ICON_TEXT_GAP + wifi_icon_w;
    ui_draw_wifi_icon(x, y + (h - STATUS_ICON_H) / 2 + 2, STATUS_ICON_H,
                      (int)status->wifi_strength, status->is_wifi, status->network_ok);
    x -= STATUS_GAP;

    x -= clock_w;
    ui_draw_text(g_font_body, x, y, COLOR_TEXT, clock);
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

// Small "marked for the download queue" indicator - a filled square with a
// checkmark drawn from line segments (each doubled up a pixel on either
// side for a bolder stroke, since SDL_RenderDrawLine is always 1px) instead
// of a font glyph, so it reads as an actual checkbox mark rather than a
// stray "+" character sitting in a flat square.
static void draw_queue_badge(int x, int y, int size) {
    ui_draw_rect(x, y, size, size, COLOR_QUEUED);

    SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    int x1 = x + size * 2 / 10, y1 = y + size * 5 / 10;
    int x2 = x + size * 4 / 10, y2 = y + size * 7 / 10;
    int x3 = x + size * 8 / 10, y3 = y + size * 3 / 10;
    for (int off = -1; off <= 1; off++) {
        SDL_RenderDrawLine(g_renderer, x1, y1 + off, x2, y2 + off);
        SDL_RenderDrawLine(g_renderer, x2, y2 + off, x3, y3 + off);
    }
}

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
    // Cover art is a plain square texture (no real alpha-clipping in SDL2) -
    // painting the corners back to the same COLOR_PANEL the rect above was
    // filled with reads as a rounded card instead.
    ui_mask_rounded_corners(icon_rect.x, icon_rect.y, icon_rect.w, icon_rect.h, GRID_ICON_RADIUS, COLOR_PANEL);

    // Download-queue badge (hold A to toggle) - top-right corner, matching
    // the usual "marked for multi-select" convention (Google Photos etc.).
    // Anchored to the icon's original bounds (not `icon_rect`, which shifts
    // during the selected icon's zoom-pop), so it stays put regardless of
    // selection/zoom state.
    if (ui_queue_contains(entry->id)) {
        draw_queue_badge(x + GRID_ICON_SIZE - 20, y, 20);
    }

    char title[64];
    truncate_to_width(g_font_small, entry->title, GRID_TITLE_MAX_W, title, sizeof(title));
    SDL_Color title_color = is_selected ? COLOR_TEXT : COLOR_TEXT_DIM;
    ui_draw_text(g_font_small, x, y + GRID_ICON_SIZE + 6, title_color, title);

    // Focus last, so the border sits on top of the cover art it frames
    // rather than being painted over by it - Borealis draws its highlight
    // around a focused widget, never as a fill behind it (see
    // ui_draw_focus_border).
    if (is_selected) {
        int box_w = GRID_ICON_SIZE + GRID_SELECT_PAD * 2;
        int box_h = GRID_ICON_SIZE + GRID_SELECT_TITLE_H + GRID_SELECT_PAD * 2;
        ui_draw_focus_border(x - GRID_SELECT_PAD, y - GRID_SELECT_PAD, box_w, box_h, 12);
    }
}

// Catalog home screen (pipensx-style) - hero banner + shelves. See
// build_shelves()'s doc comment for what data this is built from.
#define HERO_H 160
#define HERO_ICON_SIZE 130
#define HERO_PAD 16
#define SHELF_LABEL_H 26
#define SHELF_ROW_H (SHELF_LABEL_H + GRID_CELL_H)

#define HERO_DESC_MAX_LINES 6
#define HERO_DESC_LINE_MAX 160

// Word-wraps `text` into `out_lines` without drawing anything - same
// algorithm ui_draw_text_wrapped uses (word by word, TTF_SizeUTF8 per
// candidate line), just producing lines instead of drawing them. Callers
// that redraw the same text every frame (draw_hero does, since it's on
// screen for as long as the catalog home screen is up) should call this
// ONCE and cache the result rather than re-wrapping - measuring text width
// is real per-word work, and doing it 60 times a second for a multi-
// sentence description is exactly the kind of per-frame cost that reads as
// the whole screen being sluggish.
static int wrap_text_into_lines(TTF_Font *font, const char *text, int max_width,
                                char out_lines[][HERO_DESC_LINE_MAX], int max_lines) {
    if (!font || !text) return 0;

    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", text);

    int line_count = 0;
    char line[HERO_DESC_LINE_MAX] = "";
    char *saveptr = NULL;
    char *word = strtok_r(buf, " ", &saveptr);

    while (word && line_count < max_lines) {
        char candidate[HERO_DESC_LINE_MAX];
        if (line[0] == '\0') {
            snprintf(candidate, sizeof(candidate), "%s", word);
        } else {
            snprintf(candidate, sizeof(candidate), "%s %s", line, word);
        }

        int w = 0, h = 0;
        TTF_SizeUTF8(font, candidate, &w, &h);

        if (w > max_width && line[0] != '\0') {
            snprintf(out_lines[line_count++], HERO_DESC_LINE_MAX, "%s", line);
            snprintf(line, sizeof(line), "%s", word);
        } else {
            snprintf(line, sizeof(line), "%s", candidate);
        }

        word = strtok_r(NULL, " ", &saveptr);
    }
    if (line[0] != '\0' && line_count < max_lines) {
        snprintf(out_lines[line_count++], HERO_DESC_LINE_MAX, "%s", line);
    }
    return line_count;
}

// One large card for the currently-picked "featured" entry (see
// ui_show_list's hero_index) - icon on the left (the same rounded-corner
// treatment as a grid card, just bigger), title + a pre-wrapped description
// snippet (see wrap_text_into_lines - computed once when hero_index is
// picked, not per frame) on the right. `focused` draws the same accent-
// border treatment draw_grid_cell uses for a selected card, just around the
// whole panel.
static void draw_hero(int x, int y, int w, const AppEntry *entry, bool focused,
                      const char desc_lines[][HERO_DESC_LINE_MAX], int desc_line_count) {
    ui_draw_rounded_rect(x, y, w, HERO_H, 8, COLOR_PANEL);

    int icon_x = x + HERO_PAD, icon_y = y + (HERO_H - HERO_ICON_SIZE) / 2;
    SDL_Texture *icon = ui_icons_get(entry);
    if (icon) {
        SDL_Rect icon_rect = { icon_x, icon_y, HERO_ICON_SIZE, HERO_ICON_SIZE };
        SDL_RenderCopy(g_renderer, icon, NULL, &icon_rect);
    } else {
        ui_draw_rect(icon_x, icon_y, HERO_ICON_SIZE, HERO_ICON_SIZE, COLOR_BG);
        char initial[2] = { entry->title[0] ? entry->title[0] : '?', '\0' };
        ui_draw_text(g_font_title, icon_x + HERO_ICON_SIZE / 2 - 8, icon_y + HERO_ICON_SIZE / 2 - 16,
                     COLOR_TEXT_DIM, initial);
    }
    ui_mask_rounded_corners(icon_x, icon_y, HERO_ICON_SIZE, HERO_ICON_SIZE, GRID_ICON_RADIUS, COLOR_PANEL);

    int text_x = icon_x + HERO_ICON_SIZE + HERO_PAD;
    int text_w = x + w - HERO_PAD - text_x;
    int ty = y + HERO_PAD;
    ui_draw_text(g_font_small, text_x, ty, COLOR_ACCENT, tr(STR_LIST_SHELF_FEATURED));
    ty += 22;
    char title_line[96];
    truncate_to_width(g_font_title, entry->title, text_w, title_line, sizeof(title_line));
    ui_draw_text(g_font_title, text_x, ty, COLOR_TEXT, title_line);
    ty += 36;
    for (int i = 0; i < desc_line_count; i++) {
        ui_draw_text(g_font_small, text_x, ty, COLOR_TEXT_DIM, desc_lines[i]);
        ty += 20;
    }

    if (focused) ui_draw_focus_border(x, y, w, HERO_H, 8);
}

// One shelf: a label followed by a horizontally-scrolling row of cards
// (reusing draw_grid_cell as-is, so shelf cards look identical to the flat
// grid's), plus a trailing virtual "Ver todo" card past the real ones -
// every shelf gets one, since SHELF_MAX_ITEMS is a display cap, not the
// real count of matching entries. `*scroll` is the shelf's first visible
// card index, updated in place to keep `focus_col` in view whenever
// `row_focused` - the same "keep the active index in view" idea
// draw_category_tabs uses for the category strip, simplified here since
// every card is the same fixed width (no need to measure text first).
static void draw_shelf(int content_left, int content_w, int y, const Shelf *shelf,
                       const AppEntry *entries, int focus_col, int *scroll, bool row_focused) {
    ui_draw_text(g_font_body, content_left, y, COLOR_TEXT, shelf->label);

    int cards_visible = content_w / GRID_CELL_W;
    if (cards_visible < 1) cards_visible = 1;
    int total_slots = shelf->count + 1; // + the virtual "Ver todo" card

    if (row_focused) {
        if (focus_col < *scroll) *scroll = focus_col;
        if (focus_col >= *scroll + cards_visible) *scroll = focus_col - cards_visible + 1;
    }
    if (*scroll > total_slots - cards_visible) *scroll = total_slots - cards_visible;
    if (*scroll < 0) *scroll = 0;

    int card_y = y + SHELF_LABEL_H;
    int x = content_left;
    for (int i = *scroll; i < total_slots && x + GRID_CELL_W <= content_left + content_w; i++) {
        bool is_focused_card = row_focused && (i == focus_col);
        if (i < shelf->count) {
            draw_grid_cell(x, card_y, &entries[shelf->indices[i]], is_focused_card, 1.0f);
        } else {
            // Virtual "Ver todo" card - same selection-box sizing as a real
            // card's (see draw_grid_cell/GRID_SELECT_PAD) so the focus
            // border doesn't visibly change size sliding onto it.
            ui_draw_rounded_rect(x, card_y, GRID_ICON_SIZE, GRID_ICON_SIZE, GRID_ICON_RADIUS, COLOR_PANEL);
            int w = 0, h = 0;
            TTF_SizeUTF8(g_font_body, tr(STR_LIST_SHELF_SEE_ALL), &w, &h);
            ui_draw_text(g_font_body, x + (GRID_ICON_SIZE - w) / 2, card_y + GRID_ICON_SIZE / 2 - h / 2,
                         is_focused_card ? COLOR_TEXT : COLOR_TEXT_DIM, tr(STR_LIST_SHELF_SEE_ALL));
            if (is_focused_card) {
                int box_w = GRID_ICON_SIZE + GRID_SELECT_PAD * 2;
                int box_h = GRID_ICON_SIZE + GRID_SELECT_TITLE_H + GRID_SELECT_PAD * 2;
                ui_draw_focus_border(x - GRID_SELECT_PAD, card_y - GRID_SELECT_PAD, box_w, box_h, 12);
            }
        }
        x += GRID_CELL_W;
    }
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
                               const char *category_filter, int tab_scroll_start, int tabs_left) {
    ui_draw_rounded_rect(tabs_left, TAB_BAR_Y, RIGHT_EDGE - tabs_left, TAB_BAR_H, 8, COLOR_PANEL);

    // ZL/ZR button-hint boxes - always drawn the same way (not
    // dimmed/disabled), since they always do something: cycling wraps
    // around rather than stopping.
    int box_y = TAB_BAR_Y + (TAB_BAR_H - TAB_BAR_ARROW_BOX) / 2;
    int left_box_x = tabs_left + 6;
    int right_box_x = RIGHT_EDGE - 6 - TAB_BAR_ARROW_BOX;
    const char *zl_glyph = ui_button_glyph(UI_BTN_ZL);
    const char *zr_glyph = ui_button_glyph(UI_BTN_ZR);
    ui_draw_rect(left_box_x, box_y, TAB_BAR_ARROW_BOX, TAB_BAR_ARROW_BOX, COLOR_ACCENT);
    ui_draw_rect(left_box_x + 2, box_y + 2, TAB_BAR_ARROW_BOX - 4, TAB_BAR_ARROW_BOX - 4, COLOR_PANEL);
    if (zl_glyph) {
        ui_draw_text(g_font_glyph, left_box_x + 3, box_y + 1, COLOR_TEXT, zl_glyph);
    } else {
        ui_draw_text(g_font_small, left_box_x + 4, box_y + 7, COLOR_TEXT, "ZL");
    }
    ui_draw_rect(right_box_x, box_y, TAB_BAR_ARROW_BOX, TAB_BAR_ARROW_BOX, COLOR_ACCENT);
    ui_draw_rect(right_box_x + 2, box_y + 2, TAB_BAR_ARROW_BOX - 4, TAB_BAR_ARROW_BOX - 4, COLOR_PANEL);
    if (zr_glyph) {
        ui_draw_text(g_font_glyph, right_box_x + 3, box_y + 1, COLOR_TEXT, zr_glyph);
    } else {
        ui_draw_text(g_font_small, right_box_x + 4, box_y + 7, COLOR_TEXT, "ZR");
    }

    if (category_count == 0) {
        ui_draw_text(g_font_small, left_box_x + TAB_BAR_ARROW_BOX + 16, TAB_BAR_Y + 8,
                     COLOR_TEXT_DIM, tr(STR_LIST_NO_CATEGORIES));
        return 0;
    }

    int current_index = category_tab_index(categories, category_count, category_filter);

    int content_left = tabs_left + TAB_BAR_ARROW_W;
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

typedef enum {
    SIDEBAR_CATALOG = 0,
    SIDEBAR_EXPLORER,
    SIDEBAR_QUEUE,
    SIDEBAR_SAVES,
    SIDEBAR_MTP,
    SIDEBAR_FTP,
    SIDEBAR_SOURCES,
    SIDEBAR_ABOUT,
    SIDEBAR_COUNT,
} SidebarSection;

// Draws a `color`-bordered box with a `bg`-filled interior, 2px in on every
// side - the same two-rect trick draw_category_tabs already uses for its
// ZL/ZR hint boxes, reused here for every icon that needs an outline rather
// than a solid fill (ui_draw_rect only ever fills; there's no stroke
// primitive, and no SDL_gfx or similar linked in to add one).
static void draw_icon_outline(int x, int y, int w, int h, SDL_Color color, SDL_Color bg) {
    ui_draw_rect(x, y, w, h, color);
    ui_draw_rect(x + 2, y + 2, w - 4, h - 4, bg);
}

// One small glyph per section, hand-drawn from ui_draw_rect/SDL_RenderDrawLine
// - the same toolset draw_queue_badge's checkmark already uses, rather than
// pulling in an image format/asset for what's a handful of straight lines
// each. `bg` is only used for the outlined icons' (Explorador/Guardados/
// Acerca de) interior - must match whatever's actually behind the icon
// (the selection highlight color when this row is selected) or the outline
// reads as a filled box instead of a stroke.
static void draw_sidebar_icon(SidebarSection section, int x, int y, SDL_Color color, SDL_Color bg) {
    switch (section) {
        case SIDEBAR_CATALOG:
            // 2x2 grid of squares.
            ui_draw_rect(x, y, 8, 8, color);
            ui_draw_rect(x + 12, y, 8, 8, color);
            ui_draw_rect(x, y + 12, 8, 8, color);
            ui_draw_rect(x + 12, y + 12, 8, 8, color);
            break;
        case SIDEBAR_EXPLORER:
            // Folder: a tab plus an outlined body.
            ui_draw_rect(x, y + 3, 9, 4, color);
            draw_icon_outline(x, y + 6, 20, 12, color, bg);
            break;
        case SIDEBAR_QUEUE:
            // Three list bars, shrinking - reads as "queued items".
            ui_draw_rect(x, y + 2, 20, 3, color);
            ui_draw_rect(x, y + 9, 15, 3, color);
            ui_draw_rect(x, y + 16, 10, 3, color);
            break;
        case SIDEBAR_SAVES:
            // Floppy disk: outlined body, corner notch, label area.
            draw_icon_outline(x, y, 20, 20, color, bg);
            ui_draw_rect(x + 12, y + 2, 6, 5, color);
            ui_draw_rect(x + 4, y + 11, 12, 7, color);
            break;
        case SIDEBAR_MTP:
            // USB connector: outlined shell + a short stem trailing into
            // the cable.
            draw_icon_outline(x + 2, y + 4, 16, 10, color, bg);
            ui_draw_rect(x + 8, y + 14, 4, 6, color);
            break;
        case SIDEBAR_FTP:
            // Two arrows, up and down - "transfer both ways", the same
            // shorthand most file-manager apps use for FTP/network transfer.
            SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);
            SDL_RenderDrawLine(g_renderer, x + 5, y + 3, x + 5, y + 17);
            SDL_RenderDrawLine(g_renderer, x + 5, y + 3, x + 1, y + 8);
            SDL_RenderDrawLine(g_renderer, x + 5, y + 3, x + 9, y + 8);
            SDL_RenderDrawLine(g_renderer, x + 15, y + 17, x + 15, y + 3);
            SDL_RenderDrawLine(g_renderer, x + 15, y + 17, x + 11, y + 12);
            SDL_RenderDrawLine(g_renderer, x + 15, y + 17, x + 19, y + 12);
            break;
        case SIDEBAR_SOURCES:
            // Three equal-width bars with side rails - "stack/database".
            ui_draw_rect(x, y + 1, 20, 4, color);
            ui_draw_rect(x, y + 8, 20, 4, color);
            ui_draw_rect(x, y + 15, 20, 4, color);
            SDL_SetRenderDrawColor(g_renderer, color.r, color.g, color.b, color.a);
            SDL_RenderDrawLine(g_renderer, x, y + 1, x, y + 18);
            SDL_RenderDrawLine(g_renderer, x + 19, y + 1, x + 19, y + 18);
            break;
        case SIDEBAR_ABOUT:
            // Outlined box + "i", same bordered-box-with-a-letter idea as
            // the ZL/ZR hint boxes.
            draw_icon_outline(x, y, 20, 20, color, bg);
            ui_draw_text(g_font_small, x + 7, y + 2, color, "i");
            break;
        default:
            break;
    }
}

// `sidebar_has_focus` only changes the highlight color (COLOR_ACCENT when
// input is going to the sidebar, a dim outline otherwise) - the sidebar
// itself is always drawn, so its currently-remembered selection is visible
// even while the catalog content has focus, not just after switching to it.
// `collapsed` hides the text labels (icon-only rail) without changing
// anything about selection/navigation - purely a rendering choice, `sidebar_w`
// (computed by the caller from the same flag) is what actually reflows the
// content area next to it.
// One compact storage gauge for the sidebar's bottom - label and free-space
// figure on one line, a thin bar under it. This is the whole of what the two
// big top-right panels used to show, in the space Borealis leaves at the
// bottom of a sidebar, so reclaiming the header band didn't mean losing the
// information.
static void draw_sidebar_meter(int x, int y, int w, const char *label, bool ok,
                                s64 total, s64 free_bytes) {
    ui_draw_text(g_font_small, x, y, COLOR_TEXT_DIM, label);
    if (ok) {
        char free_str[32];
        format_bytes(free_bytes, free_str, sizeof(free_str));
        ui_draw_text_right(g_font_small, x + w, y, COLOR_TEXT, free_str);
    }
    float pct = (ok && total > 0) ? (float)(total - free_bytes) / (float)total : 0.0f;
    ui_draw_progress_bar(x, y + 20, w, 7, pct, COLOR_ACCENT, COLOR_TRACK);
}

static void draw_sidebar(int selected_item, bool sidebar_has_focus, bool collapsed, int sidebar_w,
                          int queue_count, const StorageInfo *storage) {
    static const StrId kLabels[SIDEBAR_COUNT] = {
        STR_LIST_SIDEBAR_CATALOG, STR_LIST_SIDEBAR_EXPLORER, STR_LIST_SIDEBAR_QUEUE, STR_LIST_SIDEBAR_SAVES,
        STR_LIST_SIDEBAR_MTP,     STR_LIST_SIDEBAR_FTP,      STR_LIST_SIDEBAR_SOURCES, STR_LIST_SIDEBAR_ABOUT,
    };

    // Flush-left, full-height panel between the header and footer rules -
    // Borealis's TabFrame sidebar, not a floating rounded box.
    ui_draw_rect(SIDEBAR_X, SIDEBAR_Y, sidebar_w, SIDEBAR_BOTTOM - SIDEBAR_Y, COLOR_PANEL);
    ui_draw_rect(SIDEBAR_X + sidebar_w - 1, SIDEBAR_Y, 1, SIDEBAR_BOTTOM - SIDEBAR_Y, COLOR_SEPARATOR);

    for (int i = 0; i < SIDEBAR_COUNT; i++) {
        int y = SIDEBAR_Y + i * SIDEBAR_ITEM_H;
        bool is_selected = (i == selected_item);

        // Borealis marks the active sidebar item with a colored indicator
        // bar down its leading edge over a slightly recessed background -
        // not by inverting the whole row - and layers the standard focus
        // border on top only while the sidebar actually has input focus.
        // (Borealis's own default indicator is a teal RGB(0,255,204); this
        // uses pipensx's accent instead, since pipensx rethemes everything
        // else to that neon blue and a lone teal stripe would read as an
        // inconsistency rather than a deliberate choice.)
        SDL_Color row_bg = COLOR_PANEL;
        if (is_selected) {
            row_bg = COLOR_BG;
            ui_draw_rect(SIDEBAR_X, y, sidebar_w, SIDEBAR_ITEM_H, row_bg);
            ui_draw_rect(SIDEBAR_X, y + 8, 4, SIDEBAR_ITEM_H - 16, COLOR_ACCENT);
        }

        SDL_Color fg = COLOR_TEXT;
        int icon_x = SIDEBAR_X + SIDEBAR_PAD_LEFT;
        int icon_y = y + (SIDEBAR_ITEM_H - SIDEBAR_ICON_SIZE) / 2;
        draw_sidebar_icon((SidebarSection)i, icon_x, icon_y, fg, row_bg);

        if (collapsed) continue;

        char label[48];
        if (i == SIDEBAR_QUEUE && queue_count > 0) {
            snprintf(label, sizeof(label), "%s (%d)", tr(kLabels[i]), queue_count);
        } else {
            snprintf(label, sizeof(label), "%s", tr(kLabels[i]));
        }
        int label_x = icon_x + SIDEBAR_ICON_SIZE + 14;
        ui_draw_text(g_font_body, label_x, y + (SIDEBAR_ITEM_H - 20) / 2, fg, label);
    }

    // Drawn after every row so the border frames the focused one rather
    // than being overdrawn by the next row's background.
    if (sidebar_has_focus && selected_item >= 0 && selected_item < SIDEBAR_COUNT) {
        ui_draw_focus_border(SIDEBAR_X, SIDEBAR_Y + selected_item * SIDEBAR_ITEM_H,
                             sidebar_w, SIDEBAR_ITEM_H, 0);
    }

    // Storage meters pinned to the bottom, only when there's room for their
    // labels (the collapsed icon rail is too narrow to say anything useful).
    if (!collapsed && storage) {
        int meter_w = sidebar_w - SIDEBAR_PAD_LEFT * 2;
        int meters_y = SIDEBAR_BOTTOM - SIDEBAR_METERS_H - 12;
        ui_draw_rect(SIDEBAR_X + SIDEBAR_PAD_LEFT, meters_y - 12, meter_w, 1, COLOR_SEPARATOR);
        draw_sidebar_meter(SIDEBAR_X + SIDEBAR_PAD_LEFT, meters_y, meter_w, tr(STR_LIST_SD_CARD),
                           storage->sd_ok, storage->sd_total, storage->sd_free);
        draw_sidebar_meter(SIDEBAR_X + SIDEBAR_PAD_LEFT, meters_y + SIDEBAR_METER_H + 10, meter_w, "NAND",
                           storage->nand_ok, storage->nand_total, storage->nand_free);
    }
}

int ui_show_list(AppEntry *entries, int count) {
    static ViewMode view_mode = VIEW_LIST;
    static SortMode sort_mode = SORT_TITLE;
    static char category_filter[APP_ENTRY_CATEGORY_MAX] = "";
    static char search_query[64] = "";
    // Icon-only rail vs full width with labels - toggled with "-" from
    // either focus state (see sidebar_focused below), independent of which
    // pane input is going to. Loaded/saved alongside view_mode/sort_mode
    // just below, so it survives a relaunch.
    static bool sidebar_collapsed = false;

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
        sidebar_collapsed = prefs.sidebar_collapsed;
        // sort_mode otherwise only actually gets applied to `entries` when
        // the user next presses X - without this, a loaded non-default
        // sort would sit unapplied (array order untouched) while the
        // footer already claims it's active.
        apply_sort(entries, count, sort_mode);
    }

    // collect_categories/build_visible are O(catalog size) string scans -
    // fine as a "redo wholesale" once per actual change (that's still what
    // happens at every mutation site below: X/sort, category cycle,
    // search), but this whole function is called once per rendered frame
    // (see main.c's `while (running && appletMainLoop())`), and unlike
    // those sites, this top-level pair used to run unconditionally on
    // *every* frame regardless of whether anything changed - fine at a
    // few hundred entries, a measurable per-frame cost at this catalog's
    // current size (thousands, once the torrent source is enabled - see
    // sources.h). Cached and only rebuilt when what they depend on
    // (entries/count identity, category_filter, search_query) actually
    // changed since the last call.
    static const AppEntry *cached_entries_ptr = NULL;
    static int cached_count = -1;
    static char cached_category_filter[APP_ENTRY_CATEGORY_MAX] = "\x01";  // never a real filter value
    static char cached_search_query[64] = "";
    static char categories[MAX_CATEGORIES][APP_ENTRY_CATEGORY_MAX];
    static int category_count = 0;
    static int visible[VISIBLE_MAX];
    static int visible_count = 0;

    // Catalog home screen (pipensx-style shelves + hero) - see
    // build_shelves()'s own doc comment for why the shelf criteria differ
    // from pipensx's. Rebuilt in the same lists_dirty block below (shelves
    // only actually depend on entries/count + categories, not
    // category_filter/search_query, but redoing them alongside those is
    // cheap - bounded by MAX_SHELVES * count, not proportional to how often
    // search/category actually change).
    static Shelf shelves[MAX_SHELVES];
    static int shelf_count = 0;
    // false = shelves (the default landing view); true = today's flat
    // grid/list, reached via a shelf's "Ver todo" card, Y, or a non-empty
    // search query (search always drops out of shelves, matching pipensx).
    static bool drilldown_active = false;
    // -1 = the hero card is focused; 0..shelf_count-1 = that shelf row.
    static int shelf_focus_row = -1;
    static int shelf_focus_col[MAX_SHELVES] = {0};
    static int shelf_scroll[MAX_SHELVES] = {0}; // first visible card index, per shelf
    static int shelf_row_scroll = 0; // first visible shelf row (shelves scroll vertically too)
    static int hero_index = -1;
    // Pre-wrapped alongside hero_index (see wrap_text_into_lines) instead of
    // re-wrapping draw_hero's description every frame - the hero is on
    // screen for as long as the catalog home is, so re-measuring a whole
    // paragraph word by word 60 times a second was a real, avoidable cost.
    static char hero_desc_lines[HERO_DESC_MAX_LINES][HERO_DESC_LINE_MAX];
    static int hero_desc_line_count = 0;

    // Split out from lists_dirty below (which also fires on a
    // category_filter/search_query-only change) because hero_index is an
    // index into `entries` itself - it MUST be re-picked whenever `entries`
    // is a genuinely different array (a catalog reload swaps it for a new
    // one; the old index could point at freed memory or just the wrong
    // game), but re-picking it on every category/search change alone would
    // make the "featured" pick change disruptively while the user is just
    // searching, which is not what "changes each time you open the app"
    // means.
    bool entries_changed = cached_entries_ptr != entries || cached_count != count;
    bool lists_dirty = entries_changed ||
                       strcmp(cached_category_filter, category_filter) != 0 ||
                       strcmp(cached_search_query, search_query) != 0;
    if (lists_dirty) {
        category_count = collect_categories(entries, count, categories);

        // No "Todos" tab - category_filter should always name a real
        // category. Default it to the first one whenever it's empty
        // (first launch, no saved preference yet) or stale (the catalog
        // changed since - sources reload, or the category itself got
        // renamed/removed) rather than leaving it pointing at nothing.
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

        visible_count = build_visible(entries, count, category_filter, search_query, visible);
        shelf_count = build_shelves(entries, count, categories, category_count, shelves);
        if (shelf_focus_row >= shelf_count) shelf_focus_row = shelf_count - 1;
        // Without this, a reload that shrinks the shelf count could leave
        // shelf_row_scroll pointing past the end - the render loop below
        // starts from shelf_row_scroll and just draws nothing past
        // shelf_count, so the screen would show blank shelves until the
        // user happened to press up/down (the only other place this gets
        // clamped).
        if (shelf_row_scroll > shelf_count - 1) shelf_row_scroll = shelf_count > 0 ? shelf_count - 1 : 0;

        cached_entries_ptr = entries;
        cached_count = count;
        snprintf(cached_category_filter, sizeof(cached_category_filter), "%s", category_filter);
        snprintf(cached_search_query, sizeof(cached_search_query), "%s", search_query);
    }

    if (entries_changed) {
        // Invalidated unconditionally first - `entries` just became a
        // different array (or count changed), so any previous hero_index is
        // an index into memory this function no longer owns even if a
        // replacement can't be picked below (e.g. a reload leaving
        // shelf_count at 0).
        hero_index = -1;
        if (shelf_count > 0) {
            // Top few of the "Recién agregados" shelf (already newest-first,
            // see build_shelves), one picked pseudo-randomly so the hero
            // isn't the exact same title every single launch. Only entries
            // with real cover art are eligible - a hero banner with no icon
            // reads as broken, not "featured".
            const Shelf *recent = &shelves[0];
            int candidates[8];
            int candidate_count = 0;
            for (int i = 0; i < recent->count && candidate_count < 8; i++) {
                if (entries[recent->indices[i]].icon_url[0])
                    candidates[candidate_count++] = recent->indices[i];
            }
            if (candidate_count > 0) {
                hero_index = candidates[armGetSystemTick() % (u64)candidate_count];
            }
        }

        hero_desc_line_count = 0;
        if (hero_index >= 0) {
            // Wrapped to the narrowest the hero panel can ever be (sidebar
            // expanded - see draw_hero's own text_w derivation, mirrored
            // here) rather than whatever content_left happens to be this
            // frame, since that animates with the sidebar collapsing/
            // expanding - wrapping once to the worst case means the cached
            // lines always fit, just with a little extra breathing room on
            // the right while the sidebar is collapsed.
            int conservative_content_left = SIDEBAR_X + SIDEBAR_W_EXPANDED + SIDEBAR_GAP;
            int conservative_hero_w = RIGHT_EDGE - conservative_content_left;
            int conservative_text_w = conservative_hero_w - 3 * HERO_PAD - HERO_ICON_SIZE;
            hero_desc_line_count = wrap_text_into_lines(g_font_small, entries[hero_index].description,
                                                        conservative_text_w, hero_desc_lines,
                                                        HERO_DESC_MAX_LINES);
        }
    }

    StorageInfo storage;
    ui_storage_refresh(&storage);

    SystemStatus status;
    ui_status_refresh(&status);
    u64 last_status_tick = armGetSystemTick();

    // Persisted across calls (this screen is re-entered every time the
    // detail screen is backed out of) so backing out of a game doesn't
    // bounce the user back to the top of the list every time.
    static int selected = 0;
    static int scroll_offset = 0; // rows scrolled, in list-row or grid-row units depending on view_mode

    // Sidebar focus/selection - persisted the same way and for the same
    // reason: re-entering this screen after closing a sidebar-opened screen
    // (e.g. Acerca de) should still have the sidebar focused and on the
    // section the user just came from, not silently reset to the catalog.
    static bool sidebar_focused = false;
    static int sidebar_selected = SIDEBAR_CATALOG;

    // Grid view's selected-icon "pop": eases toward GRID_ZOOM_TARGET each
    // frame rather than jumping there instantly, and resets to 1.0
    // whenever the selection changes so every new focus re-triggers the pop
    // instead of picking up mid-animation. Persisted across calls like
    // `selected` above, for the same reason.
    static float grid_zoom = 1.0f;
    static int grid_zoom_selected = -1;

    // Eases toward SIDEBAR_W_EXPANDED/COLLAPSED on collapse/expand instead
    // of snapping - same ui_fx_ease() pattern as grid_zoom above (and
    // instant when effects are off, since ui_fx_ease already does that).
    // Seeded to -1 so the very first frame snaps to whatever
    // sidebar_collapsed loaded as, rather than animating in from 0.
    static float sidebar_w_anim = -1.0f;
    if (sidebar_w_anim < 0.0f) {
        sidebar_w_anim = sidebar_collapsed ? SIDEBAR_W_COLLAPSED : SIDEBAR_W_EXPANDED;
    }

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

    // Edge-detected the same way kDown is (padGetButtonsDown vs held) - a
    // tap fires once, the instant a finger touches down, not every frame
    // it stays down. Screen coordinates are already native 1280x720 (the
    // touch digitizer matches display resolution 1:1), so hit-testing
    // reuses the exact same rects this screen renders with, no scaling.
    static bool touch_was_down = false;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        HidTouchScreenState touch_state = {0};
        hidGetTouchScreenStates(&touch_state, 1);
        bool touch_down_now = touch_state.count > 0;
        bool touch_tap = touch_down_now && !touch_was_down;
        int touch_x = touch_down_now ? (int)touch_state.touches[0].x : -1;
        int touch_y = touch_down_now ? (int)touch_state.touches[0].y : -1;
        touch_was_down = touch_down_now;

        // Re-querying time/psm/nifm every frame would mean opening and
        // closing those service sessions 60x/sec for no practical benefit -
        // once a second is plenty for a clock/battery/network readout.
        u64 now_tick = armGetSystemTick();
        if (armTicksToNs(now_tick - last_status_tick) >= 1000000000ULL) {
            ui_status_refresh(&status);
            last_status_tick = now_tick;
        }

        bool show_full_title = (drilldown_active || search_query[0]) && visible_count > 0;

        // Sidebar width (eased) and everything the content area derives
        // from it - recomputed every frame since sidebar_collapsed can
        // change any frame (the "-" toggle below) and the ease target with
        // it. Computed up here (not just before rendering) so the touch
        // hit-testing right below uses the exact same rects this frame
        // will render - both need it, and duplicating it in two places
        // risked them drifting out of sync. Rounded rather than truncated
        // so the content area's edges don't visibly jitter by a pixel as
        // sidebar_w_anim crosses whole numbers mid-animation.
        // Borealis folds its sidebar purely on focus - it shrinks to the
        // icon rail the moment focus moves into the tab's content and
        // expands again when focus returns to the menu (pipensx's
        // main_frame.hpp: "The fold is driven purely by focus"). The stored
        // sidebar_collapsed preference still forces the rail permanently for
        // anyone who wants the extra width all the time.
        float sidebar_w_target = (sidebar_collapsed || !sidebar_focused)
                                     ? SIDEBAR_W_COLLAPSED : SIDEBAR_W_EXPANDED;
        sidebar_w_anim = ui_fx_ease(sidebar_w_anim, sidebar_w_target, 0.22f);
        int sidebar_w = (int)(sidebar_w_anim + 0.5f);
        int content_left = SIDEBAR_X + sidebar_w + SIDEBAR_GAP;
        int col_name_x = content_left + COL_NAME_OFFSET;
        int col_type_x = content_left + COL_TYPE_OFFSET;
        int col_version_x = content_left + COL_VERSION_OFFSET;
        int col_category_x = content_left + COL_CATEGORY_OFFSET;
        int col_size_x = content_left + COL_SIZE_OFFSET;
        int grid_left = content_left + ((RIGHT_EDGE - content_left) - GRID_COLS * GRID_CELL_W) / 2;

        // Touch: tapping a sidebar row focuses+selects+activates it in one
        // gesture (no separate "focus the panel, then pick" step - that's
        // only needed for D-Pad/button navigation, which has no notion of
        // "point directly at row 3"). Tapping a grid/list entry selects and
        // installs it in one gesture too, same reasoning. Doesn't touch
        // Explorador/Cola/Fuentes/Guardados/Acerca de's own screens - each
        // of those still needs its own touch handling added separately.
        bool touch_activate_sidebar = false;
        bool touch_activate_content = false;
        if (touch_tap) {
            bool in_sidebar_x = touch_x >= SIDEBAR_X && touch_x < SIDEBAR_X + sidebar_w;
            if (in_sidebar_x && touch_y >= SIDEBAR_Y && touch_y < SIDEBAR_Y + SIDEBAR_COUNT * SIDEBAR_ITEM_H) {
                int row = (touch_y - SIDEBAR_Y) / SIDEBAR_ITEM_H;
                if (row >= 0 && row < SIDEBAR_COUNT) {
                    sidebar_selected = row;
                    sidebar_focused = true;
                    touch_activate_sidebar = true;
                }
            } else if (!(drilldown_active || search_query[0])) {
                // Catalog home screen (shelves) isn't hit-tested yet - a tap
                // here does nothing rather than falling through to the flat
                // grid/list's own (currently unrendered) cell rects, which
                // would wrongly activate whatever the D-Pad happens to have
                // focused. Controller/keyboard navigation is fully wired for
                // this screen; touch is a known gap to add separately.
            } else if (visible_count > 0 && view_mode == VIEW_GRID) {
                int first = scroll_offset * GRID_COLS;
                int last_vi = first + GRID_ROWS_VISIBLE * GRID_COLS;
                for (int vi = first; vi < visible_count && vi < last_vi; vi++) {
                    int row_in_view = (vi / GRID_COLS) - scroll_offset;
                    int col = vi % GRID_COLS;
                    int cx = grid_left + col * GRID_CELL_W, cy = GRID_TOP + row_in_view * GRID_CELL_H;
                    if (touch_x >= cx && touch_x < cx + GRID_CELL_W &&
                        touch_y >= cy - GRID_SELECT_PAD && touch_y < cy + GRID_ICON_SIZE + GRID_SELECT_TITLE_H) {
                        selected = vi;
                        sidebar_focused = false;
                        touch_activate_content = true;
                        break;
                    }
                }
            } else if (visible_count > 0 && view_mode == VIEW_LIST) {
                for (int vi = scroll_offset; vi < visible_count && vi < scroll_offset + VISIBLE_ROWS; vi++) {
                    int row_y = LIST_TOP + (vi - scroll_offset) * ROW_HEIGHT;
                    if (touch_x >= content_left && touch_x < RIGHT_EDGE &&
                        touch_y >= row_y - 6 && touch_y < row_y - 6 + ROW_HEIGHT - 4) {
                        selected = vi;
                        sidebar_focused = false;
                        touch_activate_content = true;
                        break;
                    }
                }
            }
        }

        // Works from either focus state, and doesn't itself change which
        // one is active - collapsing/expanding is orthogonal to where input
        // is going (see the SidebarSection/sidebar_focused comments above).
        if (kDown & HidNpadButton_Minus) {
            sidebar_collapsed = !sidebar_collapsed;
            ui_sound_play(UI_SOUND_NAVIGATE);
            save_prefs(view_mode, sort_mode, category_filter, sidebar_collapsed);
        }

        if (sidebar_focused) {
            if ((kDown & HidNpadButton_Down) && sidebar_selected < SIDEBAR_COUNT - 1) {
                sidebar_selected++;
                ui_sound_play(UI_SOUND_NAVIGATE);
            }
            if ((kDown & HidNpadButton_Up) && sidebar_selected > 0) {
                sidebar_selected--;
                ui_sound_play(UI_SOUND_NAVIGATE);
            }
            if (kDown & HidNpadButton_L) {
                sidebar_focused = false;
                ui_sound_play(UI_SOUND_NAVIGATE);
            }
            if ((kDown & HidNpadButton_A) || touch_activate_sidebar) {
                ui_sound_play(UI_SOUND_CONFIRM);
                switch (sidebar_selected) {
                    case SIDEBAR_CATALOG:  sidebar_focused = false; break;
                    case SIDEBAR_EXPLORER: return UI_LIST_OPEN_EXPLORER;
                    case SIDEBAR_QUEUE:    return UI_LIST_OPEN_QUEUE;
                    case SIDEBAR_SAVES:    return UI_LIST_OPEN_SAVES;
                    case SIDEBAR_MTP:      return UI_LIST_OPEN_MTP;
                    case SIDEBAR_FTP:      return UI_LIST_OPEN_FTP;
                    case SIDEBAR_SOURCES:  return UI_LIST_OPEN_SOURCES;
                    case SIDEBAR_ABOUT:    return UI_LIST_OPEN_ABOUT;
                }
            }
            if (kDown & HidNpadButton_B) {
                ui_sound_play(UI_SOUND_BACK);
                return UI_LIST_EXIT;
            }
        } else {
            if (kDown & HidNpadButton_L) {
                sidebar_focused = true;
                ui_sound_play(UI_SOUND_NAVIGATE);
            }
            if (kDown & HidNpadButton_StickL) {
                ui_sound_play(UI_SOUND_CONFIRM);
                return UI_LIST_RELOAD_CATALOG;
            }

            // Held state for repeat purposes - D-Pad or left stick, either
            // one counts. (Stick Y follows the usual up=positive convention;
            // if this ends up inverted on real hardware it's this one sign
            // that needs flipping.) Shared by both branches below - the flat
            // grid/list navigation and the catalog home screen's hero/shelf
            // navigation feel the same either way.
            u64 kHeld = padGetButtons(&pad);
            HidAnalogStickState stick = padGetStickPos(&pad, 0);
            bool held_up = (kHeld & HidNpadButton_Up) || stick.y > NAV_STICK_DEADZONE;
            bool held_down = (kHeld & HidNpadButton_Down) || stick.y < -NAV_STICK_DEADZONE;
            bool held_left = (kHeld & HidNpadButton_Left) || stick.x < -NAV_STICK_DEADZONE;
            bool held_right = (kHeld & HidNpadButton_Right) || stick.x > NAV_STICK_DEADZONE;

            if (drilldown_active || search_query[0]) {
                int cols = (view_mode == VIEW_GRID) ? GRID_COLS : 1;
                // One navigate tone per actual move, driven off the
                // selection changing rather than off each button check -
                // held auto-repeat, stick, and D-Pad then all sound
                // identical, and a press that doesn't move (already at an
                // edge) stays silent.
                int selected_before = selected;

                if (nav_repeat_step(&nav_down, held_down, now_tick)) {
                    // A straight "selected+cols < visible_count" check
                    // refuses to move at all once the target row is the
                    // last one and it's shorter than a full row
                    // (visible_count isn't a multiple of cols) - the exact
                    // column below simply doesn't exist there. Move to it
                    // when it exists, otherwise land on the last item
                    // instead of not moving - but only when there's a row
                    // below at all (selected isn't already in the last,
                    // possibly-partial, row).
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
                if (selected != selected_before) ui_sound_play(UI_SOUND_NAVIGATE);

                if (((kDown & HidNpadButton_A) || touch_activate_content) && visible_count > 0) {
                    ui_sound_play(UI_SOUND_CONFIRM);
                    return visible[selected];
                }
                if (kDown & HidNpadButton_B) {
                    ui_sound_play(UI_SOUND_BACK);
                    // A search query takes precedence over just backing out
                    // of a drilldown - B clears the search first (mirrors
                    // pipensx: B backs out of a search before it backs out
                    // of whatever's underneath it), and only falls all the
                    // way to exiting the app once neither a search nor a
                    // drilldown is active - i.e. once already back at the
                    // shelf home.
                    if (search_query[0]) {
                        search_query[0] = '\0';
                        visible_count = build_visible(entries, count, category_filter, search_query, visible);
                        selected = 0;
                        scroll_offset = 0;
                    } else if (drilldown_active) {
                        drilldown_active = false;
                    } else {
                        return UI_LIST_EXIT;
                    }
                }
                if (kDown & HidNpadButton_Y) {
                    // Same `entries`/`selected` index means the same app in
                    // both views - only the per-mode row/scroll math needs
                    // resetting.
                    view_mode = (view_mode == VIEW_LIST) ? VIEW_GRID : VIEW_LIST;
                    scroll_offset = 0;
                    ui_sound_play(UI_SOUND_NAVIGATE);
                    save_prefs(view_mode, sort_mode, category_filter, sidebar_collapsed);
                }
                if (kDown & HidNpadButton_X) {
                    sort_mode = (SortMode)((sort_mode + 1) % SORT_MODE_COUNT);
                    apply_sort(entries, count, sort_mode);
                    // Sorting reorders `entries` in place - the set of ids
                    // passing the filter is unchanged, but their positions
                    // are, so `visible` must be rebuilt against the new
                    // order.
                    visible_count = build_visible(entries, count, category_filter, search_query, visible);
                    selected = 0;
                    scroll_offset = 0;
                    ui_sound_play(UI_SOUND_NAVIGATE);
                    save_prefs(view_mode, sort_mode, category_filter, sidebar_collapsed);
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
                    ui_sound_play(UI_SOUND_NAVIGATE);
                    save_prefs(view_mode, sort_mode, category_filter, sidebar_collapsed);
                }
            } else {
                // Catalog home screen: hero (row -1) + shelves (row
                // 0..shelf_count-1), each with its own remembered column -
                // see ui_show_list's shelf_focus_row/shelf_focus_col.
                int row_before = shelf_focus_row;
                int col_before = shelf_focus_row >= 0 ? shelf_focus_col[shelf_focus_row] : 0;

                if (nav_repeat_step(&nav_down, held_down, now_tick) && shelf_focus_row < shelf_count - 1) {
                    shelf_focus_row++;
                }
                if (nav_repeat_step(&nav_up, held_up, now_tick) && shelf_focus_row > -1) {
                    shelf_focus_row--;
                }
                if (shelf_focus_row >= shelf_count) shelf_focus_row = shelf_count - 1;

                if (shelf_focus_row >= 0) {
                    // Selectable slots on this row = real cards + 1 virtual
                    // "Ver todo" (see draw_shelf) - max_col is that virtual
                    // slot's own index, i.e. an inclusive upper bound.
                    int max_col = shelves[shelf_focus_row].count;
                    if (nav_repeat_step(&nav_right, held_right, now_tick) &&
                        shelf_focus_col[shelf_focus_row] < max_col) {
                        shelf_focus_col[shelf_focus_row]++;
                    }
                    if (nav_repeat_step(&nav_left, held_left, now_tick) &&
                        shelf_focus_col[shelf_focus_row] > 0) {
                        shelf_focus_col[shelf_focus_row]--;
                    }
                    if (shelf_focus_col[shelf_focus_row] > max_col) shelf_focus_col[shelf_focus_row] = max_col;
                } else {
                    nav_left.was_held = false;
                    nav_right.was_held = false;
                }

                // Keep the focused shelf row in view - same "keep the
                // active index in view" idea as everywhere else in this
                // file (draw_category_tabs, scroll_offset above), just for
                // whole shelf rows instead of grid rows/list rows/tabs.
                int focus_row_for_scroll = shelf_focus_row < 0 ? 0 : shelf_focus_row;
                if (focus_row_for_scroll < shelf_row_scroll) shelf_row_scroll = focus_row_for_scroll;
                if (shelf_focus_row >= 0) {
                    int shelves_top = TAB_BAR_Y + HERO_H + 16;
                    int visible_shelf_rows = (FOOTER_Y - 40 - shelves_top) / SHELF_ROW_H;
                    if (visible_shelf_rows < 1) visible_shelf_rows = 1;
                    if (shelf_focus_row >= shelf_row_scroll + visible_shelf_rows)
                        shelf_row_scroll = shelf_focus_row - visible_shelf_rows + 1;
                }

                if (shelf_focus_row != row_before ||
                    (shelf_focus_row >= 0 && shelf_focus_col[shelf_focus_row] != col_before)) {
                    ui_sound_play(UI_SOUND_NAVIGATE);
                }

                if ((kDown & HidNpadButton_A) || touch_activate_content) {
                    if (shelf_focus_row < 0) {
                        if (hero_index >= 0) {
                            ui_sound_play(UI_SOUND_CONFIRM);
                            return hero_index;
                        }
                    } else {
                        const Shelf *shelf = &shelves[shelf_focus_row];
                        int col = shelf_focus_col[shelf_focus_row];
                        if (col < shelf->count) {
                            ui_sound_play(UI_SOUND_CONFIRM);
                            return shelf->indices[col];
                        }
                        // The virtual "Ver todo" slot - drop into the
                        // existing flat grid/list. A category shelf filters
                        // to that category; the "Recién agregados" shelf
                        // has no single category (its own category_filter
                        // is ""), so instead it just sorts whatever
                        // category is already selected by year - the flat
                        // view has no "every category at once" mode (see
                        // category_tab_index's doc comment on why
                        // category_filter always names a real category).
                        ui_sound_play(UI_SOUND_CONFIRM);
                        if (shelf->category_filter[0]) {
                            snprintf(category_filter, sizeof(category_filter), "%s", shelf->category_filter);
                        } else {
                            sort_mode = SORT_VERSION;
                            apply_sort(entries, count, sort_mode);
                        }
                        visible_count = build_visible(entries, count, category_filter, search_query, visible);
                        selected = 0;
                        scroll_offset = 0;
                        drilldown_active = true;
                        save_prefs(view_mode, sort_mode, category_filter, sidebar_collapsed);
                    }
                }
                if (kDown & HidNpadButton_Y) {
                    ui_sound_play(UI_SOUND_NAVIGATE);
                    drilldown_active = true;
                    selected = 0;
                    scroll_offset = 0;
                }
                if (kDown & HidNpadButton_B) {
                    ui_sound_play(UI_SOUND_BACK);
                    return UI_LIST_EXIT;
                }
            }

            if (kDown & HidNpadButton_R) {
                SwkbdConfig kbd;
                if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
                    swkbdConfigMakePresetDefault(&kbd);
                    swkbdConfigSetHeaderText(&kbd, tr(STR_LIST_SEARCH_KBD_HEADER));
                    swkbdConfigSetGuideText(&kbd, tr(STR_LIST_SEARCH_KBD_GUIDE));
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
        ui_fx_draw_background();

        // Borealis AppletFrame header: app title on the left of an 88px
        // band, its context (search query / catalog size) as a subtitle
        // beside it, clock+battery on the right, one separator rule under
        // the whole thing.
        {
            char header_line[96];
            if (search_query[0]) {
                snprintf(header_line, sizeof(header_line), tr(STR_LIST_HEADER_SEARCHING_TEMPLATE), search_query);
            } else {
                // `count` is the root (non-DLC) entry count ui_show_list was
                // called with - i.e. how many actual games/apps the catalog
                // holds, which is what a user asking "how many games are
                // there" means, not the raw AppEntry count (that includes
                // every DLC/update row too, each of which only ever surfaces
                // nested under its base game's detail screen - see
                // build_root_entries).
                snprintf(header_line, sizeof(header_line), tr(STR_LIST_HEADER_CATALOG_COUNT_TEMPLATE), count);
            }
            ui_draw_frame_header(SCREEN_W, "FreeShop", header_line);
        }
        draw_status_bar(SCREEN_W - UI_FRAME_HEADER_PAD_SIDES, STATUS_Y, &status);

        // Only the label fade needs the fully-settled collapsed state, not
        // the mid-animation width - labels would otherwise render squashed
        // into a still-shrinking rail for a few frames each time. Matches
        // the focus-driven fold target computed above, so labels vanish for
        // exactly the states the rail is narrow in.
        draw_sidebar(sidebar_selected, sidebar_focused, sidebar_collapsed || !sidebar_focused,
                     sidebar_w, ui_queue_count(), &storage);

        if (!drilldown_active && !search_query[0]) {
            ui_icons_begin_frame();

            int hero_y = TAB_BAR_Y;
            bool hero_focused = shelf_focus_row < 0;
            if (hero_index >= 0) {
                draw_hero(content_left, hero_y, RIGHT_EDGE - content_left, &entries[hero_index], hero_focused,
                         hero_desc_lines, hero_desc_line_count);
            } else {
                ui_draw_rounded_rect(content_left, hero_y, RIGHT_EDGE - content_left, HERO_H, 8, COLOR_PANEL);
            }

            int shelves_top = hero_y + HERO_H + 16;
            int shelves_bottom = FOOTER_Y - 40;
            int visible_shelf_rows = (shelves_bottom - shelves_top) / SHELF_ROW_H;
            if (visible_shelf_rows < 1) visible_shelf_rows = 1;

            if (shelf_count == 0) {
                ui_draw_text(g_font_body, content_left, shelves_top, COLOR_TEXT_DIM,
                             empty_state_message(count, category_filter, search_query));
            }

            for (int r = shelf_row_scroll; r < shelf_count && r < shelf_row_scroll + visible_shelf_rows; r++) {
                int row_y = shelves_top + (r - shelf_row_scroll) * SHELF_ROW_H;
                bool row_focused = (r == shelf_focus_row);
                draw_shelf(content_left, RIGHT_EDGE - content_left, row_y, &shelves[r], entries,
                          shelf_focus_col[r], &shelf_scroll[r], row_focused);
            }
        } else {
        tab_scroll_start = draw_category_tabs(categories, category_count, category_filter, tab_scroll_start,
                                               content_left);

        if (view_mode == VIEW_LIST) {
            ui_draw_rect(content_left, COL_HEADER_Y, RIGHT_EDGE - content_left, COL_HEADER_H, COLOR_PANEL);
            ui_draw_text(g_font_small, col_name_x, COL_HEADER_Y + 8, COLOR_TEXT_DIM, tr(STR_LIST_COL_NAME));
            ui_draw_text(g_font_small, col_type_x, COL_HEADER_Y + 8, COLOR_TEXT_DIM, tr(STR_LIST_COL_TYPE));
            ui_draw_text(g_font_small, col_version_x, COL_HEADER_Y + 8, COLOR_TEXT_DIM, tr(STR_LIST_COL_VERSION));
            ui_draw_text(g_font_small, col_category_x, COL_HEADER_Y + 8, COLOR_TEXT_DIM, tr(STR_LIST_COL_CATEGORY));
            ui_draw_text(g_font_small, col_size_x, COL_HEADER_Y + 8, COLOR_TEXT_DIM, tr(STR_LIST_COL_SIZE));

            if (visible_count == 0) {
                ui_draw_text(g_font_body, col_name_x, LIST_TOP, COLOR_TEXT_DIM,
                             empty_state_message(count, category_filter, search_query));
            }

            if (selected < scroll_offset) scroll_offset = selected;
            if (selected >= scroll_offset + VISIBLE_ROWS) scroll_offset = selected - VISIBLE_ROWS + 1;

            // Zebra striping first, so the highlight bar below lands on top
            // of it rather than being carved up by it.
            for (int vi = scroll_offset; vi < visible_count && vi < scroll_offset + VISIBLE_ROWS; vi++) {
                int row_index = vi - scroll_offset;
                if (row_index % 2 != 1) continue;
                int row_y = LIST_TOP + row_index * ROW_HEIGHT;
                ui_draw_rect(content_left, row_y - 6, RIGHT_EDGE - content_left, ROW_HEIGHT - 4, COLOR_PANEL);
            }

            // The highlight slides to the newly selected row instead of
            // teleporting. Kept as one bar drawn from an eased position
            // rather than a per-row rect - that's what makes the movement
            // continuous between rows. Persisted across calls (this screen
            // is re-entered from the detail screen) and seeded to -1 so the
            // very first frame snaps rather than flying in from the top.
            // Only the position is computed here; the border itself is drawn
            // after the rows below, since Borealis's focus frames its
            // widget rather than sitting behind it.
            static float highlight_y = -1.0f;
            if (visible_count > 0) {
                float target_y = (float)(LIST_TOP + (selected - scroll_offset) * ROW_HEIGHT - 6);
                highlight_y = (highlight_y < 0.0f) ? target_y : ui_fx_ease(highlight_y, target_y, 0.30f);
            } else {
                highlight_y = -1.0f;
            }

            for (int vi = scroll_offset; vi < visible_count && vi < scroll_offset + VISIBLE_ROWS; vi++) {
                int i = visible[vi];
                int row_index = vi - scroll_offset;
                int row_y = LIST_TOP + row_index * ROW_HEIGHT;

                // Rows keep their normal colors whether focused or not -
                // the focus border alone marks the selection now, so there
                // is no accent fill behind the text to invert against.
                SDL_Color text_color = COLOR_TEXT;
                SDL_Color dim_color = COLOR_TEXT_DIM;

                char size_str[32];
                format_size(entries[i].file_size, size_str, sizeof(size_str));

                // Download-queue badge (hold A to toggle) - drawn in the
                // margin between LEFT_EDGE and col_name_x, same idea as the
                // grid view's badge.
                if (ui_queue_contains(entries[i].id)) {
                    draw_queue_badge(content_left + 2, row_y - 3, 16);
                }

                char row_title[160];
                truncate_to_width(g_font_body, entries[i].title, COL_NAME_MAX_W, row_title, sizeof(row_title));
                ui_draw_text(g_font_body, col_name_x, row_y, text_color, row_title);
                ui_draw_text(g_font_small, col_type_x, row_y + 2, dim_color, file_type_label(entries[i].file_type));

                char row_version[32], row_category[APP_ENTRY_CATEGORY_MAX];
                truncate_to_width(g_font_small, entries[i].version, COL_VERSION_MAX_W, row_version, sizeof(row_version));
                truncate_to_width(g_font_small, entries[i].category, COL_CATEGORY_MAX_W, row_category, sizeof(row_category));
                ui_draw_text(g_font_small, col_version_x, row_y + 2, dim_color, row_version);
                ui_draw_text(g_font_small, col_category_x, row_y + 2, dim_color, row_category);
                ui_draw_text(g_font_small, col_size_x, row_y + 2, dim_color, size_str);
            }

            if (visible_count > 0 && highlight_y >= 0.0f) {
                ui_draw_focus_border(content_left, (int)highlight_y, RIGHT_EDGE - content_left,
                                     ROW_HEIGHT - 4, 6);
            }
        } else {
            ui_icons_begin_frame();

            if (selected != grid_zoom_selected) {
                grid_zoom = 1.0f;
                grid_zoom_selected = selected;
            }
            grid_zoom = ui_fx_ease(grid_zoom, GRID_ZOOM_TARGET, GRID_ZOOM_EASE);

            int selected_row = selected / GRID_COLS;
            if (selected_row < scroll_offset) scroll_offset = selected_row;
            if (selected_row >= scroll_offset + GRID_ROWS_VISIBLE) scroll_offset = selected_row - GRID_ROWS_VISIBLE + 1;

            if (visible_count == 0) {
                ui_draw_text(g_font_body, grid_left, GRID_TOP, COLOR_TEXT_DIM,
                             empty_state_message(count, category_filter, search_query));
            }

            int first = scroll_offset * GRID_COLS;
            int last = first + GRID_ROWS_VISIBLE * GRID_COLS;
            for (int vi = first; vi < visible_count && vi < last; vi++) {
                int i = visible[vi];
                int row_in_view = (vi / GRID_COLS) - scroll_offset;
                int col = vi % GRID_COLS;
                int cell_x = grid_left + col * GRID_CELL_W;
                int cell_y = GRID_TOP + row_in_view * GRID_CELL_H;
                bool is_selected = vi == selected;
                draw_grid_cell(cell_x, cell_y, &entries[i], is_selected, is_selected ? grid_zoom : 1.0f);
            }
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

        char panel_hint[32];
        snprintf(panel_hint, sizeof(panel_hint), tr(STR_LIST_HINT_PANEL_TEMPLATE),
                 tr(sidebar_collapsed ? STR_LIST_SIDEBAR_EXPAND : STR_LIST_SIDEBAR_COLLAPSE));

        ui_draw_rect(LEFT_EDGE, FOOTER_Y - 10, RIGHT_EDGE - LEFT_EDGE, 1, COLOR_SEPARATOR);
        if (sidebar_focused) {
            int fx = LEFT_EDGE;
            fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_UP_DOWN, tr(STR_LIST_HINT_NAVIGATE));
            fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_A, tr(STR_LIST_HINT_OPEN));
            fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_L, tr(STR_LIST_HINT_BACK_TO_CATALOG));
            fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_MINUS, panel_hint);
            ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_B, tr(STR_LIST_HINT_EXIT));
        } else {
            // Split across two lines - all of this crammed onto one ran wide
            // enough to overflow past RIGHT_EDGE at 1280px, silently
            // clipping whatever came after the overflow point off the edge
            // of the screen (draw_footer_hint doesn't wrap or clip-warn).
            char view_hint[24];
            snprintf(view_hint, sizeof(view_hint), tr(STR_LIST_HINT_VIEW_TEMPLATE),
                     view_mode == VIEW_LIST ? tr(STR_LIST_VIEW_GRID) : tr(STR_LIST_VIEW_LIST));
            char sort_hint[32];
            snprintf(sort_hint, sizeof(sort_hint), tr(STR_LIST_HINT_SORT_TEMPLATE), sort_mode_label(sort_mode));

            int fx = LEFT_EDGE;
            fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_DPAD, tr(STR_LIST_HINT_NAVIGATE));
            fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_A, tr(STR_LIST_HINT_INSTALL));
            fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_ZL, NULL);
            fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_ZR, tr(STR_LIST_HINT_CATEGORY));
            fx = ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_Y, view_hint);
            ui_draw_button_hint(fx, FOOTER_Y, UI_BTN_X, sort_hint);

            char search_hint[24];
            snprintf(search_hint, sizeof(search_hint), tr(STR_LIST_HINT_SEARCH_TEMPLATE),
                     search_query[0] ? tr(STR_LIST_SEARCH_ACTIVE_SUFFIX) : "");

            fx = LEFT_EDGE;
            fx = ui_draw_button_hint(fx, FOOTER_Y + 26, UI_BTN_R, search_hint);
            fx = ui_draw_button_hint(fx, FOOTER_Y + 26, UI_BTN_L, tr(STR_LIST_HINT_MENU));
            fx = ui_draw_button_hint(fx, FOOTER_Y + 26, UI_BTN_MINUS, panel_hint);
            fx = ui_draw_button_hint(fx, FOOTER_Y + 26, UI_BTN_STICK_L, tr(STR_LIST_HINT_RELOAD));
            ui_draw_button_hint(fx, FOOTER_Y + 26, UI_BTN_B, tr(STR_LIST_HINT_EXIT));
        }

        SDL_RenderPresent(g_renderer);
    }

    return UI_LIST_EXIT;
}
