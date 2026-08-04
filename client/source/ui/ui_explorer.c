#include "ui_explorer.h"
#include "ui_app.h"
#include "ui_nav.h"
#include "ui_cleanup.h"

#include <switch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define SCREEN_W 1280
#define SCREEN_H 720
#define LEFT_EDGE 20
#define RIGHT_EDGE (SCREEN_W - 20)

#define HEADER_Y 40
#define PATH_Y 80
#define LIST_TOP 130
#define ROW_HEIGHT 36
#define VISIBLE_ROWS 13
#define FOOTER_Y (SCREEN_H - 46)

// A generous fixed cap (matching this project's other MAX constants -
// GRID_COLS, VISIBLE_MAX, etc. - rather than a heap allocation) - a folder
// with more entries than this just shows the first EXPLORER_MAX_ENTRIES,
// alphabetically, silently.
#define EXPLORER_MAX_ENTRIES 512
#define EXPLORER_PATH_MAX 512
#define EXPLORER_NAME_MAX 256

// Text viewer (A on a .txt/.log/etc.) caps - both bound memory use and avoid
// the exact "text runs off RIGHT_EDGE, invisible, no wrap" bug the main
// list's footer had (see ui_list.c) - ui_draw_text never wraps or clips.
#define TEXT_VIEW_MAX_BYTES (64 * 1024)
#define TEXT_VIEW_MAX_LINES 4000
#define TEXT_VIEW_VISIBLE_LINES 26
#define TEXT_VIEW_LINE_HEIGHT 20
#define TEXT_VIEW_LINE_MAX_CHARS 110
// Editing goes through the system software keyboard (swkbd) as a single
// "edit the whole file" session - only offered for files small enough that
// this stays practical on a controller-driven on-screen keyboard, well
// under whatever swkbd's own string length ceiling turns out to be.
#define TEXT_EDIT_MAX_BYTES 4096

typedef struct {
    char name[EXPLORER_NAME_MAX];
    bool is_dir;
} ExplorerEntry;

static bool has_suffix_ci(const char *name, const char *suffix) {
    size_t nlen = strlen(name), slen = strlen(suffix);
    if (slen > nlen) return false;
    return strcasecmp(name + (nlen - slen), suffix) == 0;
}

static bool is_text_like(const char *name) {
    static const char *exts[] = { ".txt", ".log", ".json", ".ini", ".cfg", ".md", ".xml", ".yml", ".yaml" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (has_suffix_ci(name, exts[i])) return true;
    }
    return false;
}

static int compare_entries(const void *a, const void *b) {
    const ExplorerEntry *ea = (const ExplorerEntry *)a;
    const ExplorerEntry *eb = (const ExplorerEntry *)b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1; // directories first
    return strcasecmp(ea->name, eb->name);
}

// Reads `path`'s entries (skipping "." and "..") into `out`, up to
// EXPLORER_MAX_ENTRIES, sorted directories-first then alphabetically.
// Returns the count. d_type isn't trusted (inconsistent across sdmc FS
// backends) - stat() is the reliable way to tell a directory from a file here.
static int list_dir(const char *path, ExplorerEntry *out) {
    DIR *dir = opendir(path);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while (count < EXPLORER_MAX_ENTRIES && (ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char full_path[EXPLORER_PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);

        struct stat st;
        bool is_dir = (stat(full_path, &st) == 0) && S_ISDIR(st.st_mode);

        snprintf(out[count].name, sizeof(out[count].name), "%s", ent->d_name);
        out[count].is_dir = is_dir;
        count++;
    }
    closedir(dir);

    qsort(out, count, sizeof(ExplorerEntry), compare_entries);
    return count;
}

// Deletes `path` - recursing into and removing every child first if it's a
// directory. Best-effort: keeps going even if one child fails, so a single
// stubborn file doesn't leave everything else undeleted too.
static bool delete_recursive(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;

    if (!S_ISDIR(st.st_mode)) {
        return remove(path) == 0;
    }

    DIR *dir = opendir(path);
    if (!dir) return false;

    bool ok = true;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[EXPLORER_PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (!delete_recursive(child)) ok = false;
    }
    closedir(dir);

    if (rmdir(path) != 0) ok = false;
    return ok;
}

static void show_entry_info(const char *full_path, const ExplorerEntry *e) {
    struct stat st;
    char msg[300];
    if (stat(full_path, &st) != 0) {
        snprintf(msg, sizeof(msg), "%.100s\n\nNo se pudo leer la información.", e->name);
    } else if (e->is_dir) {
        snprintf(msg, sizeof(msg), "%.100s\n\nCarpeta", e->name);
    } else {
        double size = (double)st.st_size;
        const char *unit = "bytes";
        if (size >= 1024.0 * 1024.0 * 1024.0) { size /= 1024.0 * 1024.0 * 1024.0; unit = "GB"; }
        else if (size >= 1024.0 * 1024.0) { size /= 1024.0 * 1024.0; unit = "MB"; }
        else if (size >= 1024.0) { size /= 1024.0; unit = "KB"; }

        if (strcmp(unit, "bytes") == 0) {
            snprintf(msg, sizeof(msg), "%.100s\n\nArchivo\nTamaño: %lld bytes", e->name, (long long)st.st_size);
        } else {
            snprintf(msg, sizeof(msg), "%.100s\n\nArchivo\nTamaño: %.2f %s (%lld bytes)",
                     e->name, size, unit, (long long)st.st_size);
        }
    }
    ui_app_show_message(msg);
}

// Splits raw_buf (NUL-terminated, `raw_len` bytes not counting the NUL)
// into display_lines for rendering - a destructive copy of raw_buf, since
// display_lines punches '\0's in where '\n's were; raw_buf itself is left
// untouched so it stays valid as the edit/save source of truth. Caps both
// line count and each line's on-screen character length so nothing can run
// off RIGHT_EDGE the way the main list's footer once did.
static char display_buf[TEXT_VIEW_MAX_BYTES + 1];
static const char *display_lines[TEXT_VIEW_MAX_LINES];
static int split_lines(const char *raw_buf, size_t raw_len) {
    size_t copy_len = raw_len < TEXT_VIEW_MAX_BYTES ? raw_len : TEXT_VIEW_MAX_BYTES;
    memcpy(display_buf, raw_buf, copy_len);
    display_buf[copy_len] = '\0';

    int line_count = 0;
    char *cursor = display_buf;
    while (line_count < TEXT_VIEW_MAX_LINES) {
        display_lines[line_count++] = cursor;
        char *newline = strchr(cursor, '\n');
        if (!newline) break;
        *newline = '\0';
        // Drop a trailing '\r' too (CRLF files), or it'd show as a stray
        // box glyph at the end of the line.
        size_t len = strlen(cursor);
        if (len > 0 && cursor[len - 1] == '\r') cursor[len - 1] = '\0';
        cursor = newline + 1;
    }
    return line_count;
}

// Shows a text file's content, scrollable with the D-Pad/stick. Reads at
// most TEXT_VIEW_MAX_BYTES. Files no bigger than TEXT_EDIT_MAX_BYTES can
// also be edited (Y) via the system keyboard and saved back (overwrites the
// whole file) - bigger ones are view-only, editing a large file through an
// on-screen keyboard one session at a time stops being practical well
// before 64KB.
static void show_text_file(const char *path) {
    static char raw_buf[TEXT_VIEW_MAX_BYTES + 1];

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ui_app_show_message("No se pudo abrir el archivo.");
        return;
    }
    size_t raw_len = fread(raw_buf, 1, TEXT_VIEW_MAX_BYTES, fp);
    bool truncated_file = (fgetc(fp) != EOF);
    fclose(fp);
    raw_buf[raw_len] = '\0';

    bool editable = !truncated_file && raw_len <= TEXT_EDIT_MAX_BYTES;
    int line_count = split_lines(raw_buf, raw_len);

    int scroll = 0;
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

        bool held_up = (kHeld & HidNpadButton_Up) || stick.y > NAV_STICK_DEADZONE;
        bool held_down = (kHeld & HidNpadButton_Down) || stick.y < -NAV_STICK_DEADZONE;

        if (nav_repeat_step(&nav_up, held_up, now_tick) && scroll > 0) scroll--;
        if (nav_repeat_step(&nav_down, held_down, now_tick) && scroll < line_count - TEXT_VIEW_VISIBLE_LINES) scroll++;

        if (kDown & (HidNpadButton_B | HidNpadButton_Plus | HidNpadButton_A)) return;

        if ((kDown & HidNpadButton_Y) && editable) {
            SwkbdConfig kbd;
            if (R_SUCCEEDED(swkbdCreate(&kbd, 0))) {
                swkbdConfigMakePresetDefault(&kbd);
                swkbdConfigSetHeaderText(&kbd, "Editar archivo");
                swkbdConfigSetGuideText(&kbd, path);
                swkbdConfigSetInitialText(&kbd, raw_buf);
                swkbdConfigSetStringLenMax(&kbd, TEXT_EDIT_MAX_BYTES);

                static char edited[TEXT_EDIT_MAX_BYTES + 1];
                if (R_SUCCEEDED(swkbdShow(&kbd, edited, sizeof(edited)))) {
                    FILE *out = fopen(path, "wb");
                    if (out) {
                        size_t edited_len = strlen(edited);
                        size_t written = fwrite(edited, 1, edited_len, out);
                        fclose(out);
                        if (written == edited_len) {
                            snprintf(raw_buf, sizeof(raw_buf), "%s", edited);
                            raw_len = edited_len;
                            line_count = split_lines(raw_buf, raw_len);
                            scroll = 0;
                        } else {
                            ui_app_show_message("No se pudo guardar el archivo por completo.");
                        }
                    } else {
                        ui_app_show_message("No se pudo abrir el archivo para guardar.");
                    }
                }
                swkbdClose(&kbd);
            }
            // The keyboard applet (and possibly the error message above)
            // took over the screen - re-prime `pad` or whatever button
            // dismissed the last one of those would misread as a fresh
            // press here. See ui_show_explorer's identical fix.
            padUpdate(&pad);
        }

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, "Ver archivo");
        char header[140];
        snprintf(header, sizeof(header), "%.100s%s", path, truncated_file ? "  (primeros 64KB)" : "");
        ui_draw_text(g_font_small, LEFT_EDGE, PATH_Y, COLOR_TEXT_DIM, header);

        int last = scroll + TEXT_VIEW_VISIBLE_LINES;
        if (last > line_count) last = line_count;
        for (int i = scroll; i < last; i++) {
            char clipped[TEXT_VIEW_LINE_MAX_CHARS + 1];
            snprintf(clipped, sizeof(clipped), "%s", display_lines[i]);
            ui_draw_text(g_font_small, LEFT_EDGE, LIST_TOP + (i - scroll) * TEXT_VIEW_LINE_HEIGHT, COLOR_TEXT, clipped);
        }

        int hint_x = LEFT_EDGE;
        hint_x = ui_draw_button_hint(hint_x, FOOTER_Y, UI_BTN_DPAD, "desplazar");
        if (editable) hint_x = ui_draw_button_hint(hint_x, FOOTER_Y, UI_BTN_Y, "editar");
        hint_x = ui_draw_button_hint(hint_x, FOOTER_Y, UI_BTN_B, "volver");
        if (!editable) {
            ui_draw_text(g_font_small, hint_x, FOOTER_Y, COLOR_TEXT_DIM, "(archivo muy grande para editar)");
        }

        SDL_RenderPresent(g_renderer);
    }
}

UiExplorerAction ui_show_explorer(char *out_path, size_t out_path_size, bool *out_is_xci) {
    // Persisted across calls (like ui_list.c's `selected`/`scroll_offset`)
    // so backing out and reopening the explorer resumes where the user left
    // off instead of bouncing back to sdmc:/ every time.
    static char current_path[EXPLORER_PATH_MAX] = "sdmc:/";
    static ExplorerEntry entries[EXPLORER_MAX_ENTRIES];
    static int entry_count = 0;
    static int selected = 0;
    static int scroll_offset = 0;
    static bool loaded = false;

    // Remembers `selected`/`scroll_offset` for every directory level above
    // the current one, so backing out of a subfolder (B) lands back on the
    // exact item/scroll position that was showing before entering it,
    // instead of resetting to the top - one saved position per level, in
    // the order they were entered. Persisted like the rest of this
    // function's state, for the same "resume where I left off" reason.
    #define EXPLORER_MAX_DEPTH 32
    static struct { int selected; int scroll_offset; } nav_stack[EXPLORER_MAX_DEPTH];
    static int nav_depth = 0;

    if (!loaded) {
        entry_count = list_dir(current_path, entries);
        loaded = true;
    }
    if (selected >= entry_count) selected = entry_count > 0 ? entry_count - 1 : 0;
    if (selected < 0) selected = 0;

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    // Prime the button-state baseline - see main.c's identical comment on
    // why (a button still held from the previous screen would otherwise
    // misreport as newly pressed here).
    padUpdate(&pad);

    NavRepeatState nav_up = {0}, nav_down = {0};

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        u64 kHeld = padGetButtons(&pad);
        HidAnalogStickState stick = padGetStickPos(&pad, 0);
        u64 now_tick = armGetSystemTick();

        bool held_up = (kHeld & HidNpadButton_Up) || stick.y > NAV_STICK_DEADZONE;
        bool held_down = (kHeld & HidNpadButton_Down) || stick.y < -NAV_STICK_DEADZONE;

        if (nav_repeat_step(&nav_up, held_up, now_tick) && selected > 0) selected--;
        if (nav_repeat_step(&nav_down, held_down, now_tick) && selected < entry_count - 1) selected++;

        if (selected < scroll_offset) scroll_offset = selected;
        if (selected >= scroll_offset + VISIBLE_ROWS) scroll_offset = selected - VISIBLE_ROWS + 1;

        char selected_path[EXPLORER_PATH_MAX];
        if (entry_count > 0) {
            snprintf(selected_path, sizeof(selected_path), "%s/%s", current_path, entries[selected].name);
        } else {
            selected_path[0] = '\0';
        }

        // Every branch below that opens a nested modal/screen (show_text_file,
        // ui_app_show_message/confirm, ui_show_cleanup) drives its OWN
        // separate PadState for as long as it's on screen - real button
        // presses happen on it (e.g. A to dismiss a message) that this
        // loop's `pad` never sees, since `pad` isn't updated while a nested
        // call has control. Without re-priming `pad` once more before this
        // iteration ends, whatever button dismissed the nested UI would look
        // like a *fresh* press to `pad` on the very next padUpdate() call
        // (its baseline is stale from before the nested call ran) and
        // immediately re-trigger this same handler - e.g. pressing A to
        // close the info popup instantly re-opening/re-installing the
        // selected entry. See main.c's identical priming comment for the
        // same fix applied at screen-entry time instead of mid-loop.
        bool nested_ui_shown = false;

        if ((kDown & HidNpadButton_A) && entry_count > 0) {
            ExplorerEntry *e = &entries[selected];
            if (e->is_dir) {
                if (nav_depth < EXPLORER_MAX_DEPTH) {
                    nav_stack[nav_depth].selected = selected;
                    nav_stack[nav_depth].scroll_offset = scroll_offset;
                    nav_depth++;
                }
                snprintf(current_path, sizeof(current_path), "%s", selected_path);
                entry_count = list_dir(current_path, entries);
                selected = 0;
                scroll_offset = 0;
            } else if (has_suffix_ci(e->name, ".nsp") || has_suffix_ci(e->name, ".xci")) {
                snprintf(out_path, out_path_size, "%s", selected_path);
                *out_is_xci = has_suffix_ci(e->name, ".xci");
                loaded = false; // re-list fresh next time the explorer opens
                return UI_EXPLORER_INSTALL;
            } else if (is_text_like(e->name)) {
                show_text_file(selected_path);
                nested_ui_shown = true;
            } else {
                ui_app_show_message("Ese archivo no es un .nsp/.xci ni un tipo de texto que se pueda ver aquí.");
                nested_ui_shown = true;
            }
        }

        if ((kDown & HidNpadButton_X) && entry_count > 0) {
            show_entry_info(selected_path, &entries[selected]);
            nested_ui_shown = true;
        }

        if ((kDown & HidNpadButton_Y) && entry_count > 0) {
            char confirm_msg[300];
            snprintf(confirm_msg, sizeof(confirm_msg),
                     "¿Eliminar \"%.200s\"?\n\nEsta acción no se puede deshacer.", entries[selected].name);
            if (ui_app_show_confirm(confirm_msg)) {
                delete_recursive(selected_path);
                entry_count = list_dir(current_path, entries);
                if (selected >= entry_count) selected = entry_count > 0 ? entry_count - 1 : 0;
            }
            nested_ui_shown = true;
        }

        if (kDown & HidNpadButton_ZL) {
            ui_show_cleanup();
            nested_ui_shown = true;
            // The cleanup screen may have deleted stray files sitting in
            // whatever directory brought the user here - refresh so a
            // just-deleted entry doesn't linger in the list.
            entry_count = list_dir(current_path, entries);
            if (selected >= entry_count) selected = entry_count > 0 ? entry_count - 1 : 0;
        }

        if (kDown & HidNpadButton_B) {
            if (strcmp(current_path, "sdmc:/") == 0) {
                // No parent to go up to - back out of the explorer entirely.
                loaded = false;
                return UI_EXPLORER_EXIT;
            }
            char *last_slash = strrchr(current_path, '/');
            // "sdmc:/" itself (6 chars) always has a slash at index 5 - only
            // truncate at a LATER slash, otherwise (one level deep already)
            // go straight back to the root.
            if (last_slash && last_slash > current_path + 5) {
                *last_slash = '\0';
            } else {
                snprintf(current_path, sizeof(current_path), "sdmc:/");
            }
            entry_count = list_dir(current_path, entries);
            if (nav_depth > 0) {
                nav_depth--;
                selected = nav_stack[nav_depth].selected;
                scroll_offset = nav_stack[nav_depth].scroll_offset;
                // The directory's contents may have changed since leaving it
                // (a file added/removed elsewhere, or by this screen's own
                // delete/cleanup actions) - clamp rather than trust a
                // possibly-stale index.
                if (selected >= entry_count) selected = entry_count > 0 ? entry_count - 1 : 0;
                if (selected < 0) selected = 0;
                if (scroll_offset > selected) scroll_offset = selected;
            } else {
                selected = 0;
                scroll_offset = 0;
            }
        }

        if (kDown & HidNpadButton_Plus) {
            loaded = false;
            return UI_EXPLORER_EXIT;
        }

        if (nested_ui_shown) {
            padUpdate(&pad);
        }

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, "Explorador de archivos");
        ui_draw_text(g_font_small, LEFT_EDGE, PATH_Y, COLOR_TEXT_DIM, current_path);

        if (entry_count == 0) {
            ui_draw_text(g_font_body, LEFT_EDGE, LIST_TOP, COLOR_TEXT_DIM, "(carpeta vacía)");
        }

        int last = scroll_offset + VISIBLE_ROWS;
        if (last > entry_count) last = entry_count;
        for (int i = scroll_offset; i < last; i++) {
            int row_index = i - scroll_offset;
            int row_y = LIST_TOP + row_index * ROW_HEIGHT;
            bool is_selected = (i == selected);

            if (row_index % 2 == 1) {
                ui_draw_rect(LEFT_EDGE, row_y - 6, RIGHT_EDGE - LEFT_EDGE, ROW_HEIGHT - 4, COLOR_PANEL);
            }

            // Normal colors regardless of focus - the Borealis-style focus
            // border below is what marks the selection now.
            char label[300];
            snprintf(label, sizeof(label), "%s%s", entries[i].is_dir ? "[DIR]  " : "       ", entries[i].name);
            ui_draw_text(g_font_body, LEFT_EDGE + 10, row_y, COLOR_TEXT, label);

            if (is_selected) {
                ui_draw_focus_border(LEFT_EDGE, row_y - 6, RIGHT_EDGE - LEFT_EDGE, ROW_HEIGHT - 4, 6);
            }
        }

        ui_draw_rect(LEFT_EDGE, FOOTER_Y - 10, RIGHT_EDGE - LEFT_EDGE, 1, COLOR_SEPARATOR);
        int hint_x = LEFT_EDGE;
        hint_x = ui_draw_button_hint(hint_x, FOOTER_Y, UI_BTN_DPAD, "navegar");
        hint_x = ui_draw_button_hint(hint_x, FOOTER_Y, UI_BTN_A, "abrir/instalar/ver");
        hint_x = ui_draw_button_hint(hint_x, FOOTER_Y, UI_BTN_B, "subir");
        ui_draw_button_hint(hint_x, FOOTER_Y, UI_BTN_ZL, "limpieza");

        hint_x = LEFT_EDGE;
        hint_x = ui_draw_button_hint(hint_x, FOOTER_Y + 24, UI_BTN_X, "info");
        hint_x = ui_draw_button_hint(hint_x, FOOTER_Y + 24, UI_BTN_Y, "eliminar");
        ui_draw_button_hint(hint_x, FOOTER_Y + 24, UI_BTN_PLUS, "salir");

        SDL_RenderPresent(g_renderer);
    }

    return UI_EXPLORER_EXIT;
}
