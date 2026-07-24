#include "ui_cleanup.h"
#include "ui_app.h"
#include "ui_icons.h"
#include "../install/ncm_cleanup.h"

#include <switch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define SCREEN_W 1280
#define SCREEN_H 720
#define LEFT_EDGE 20

// Must match ui_icons.c's own ICON_CACHE_DIR - not shared via a header
// since nothing else needs it, just kept here for this one purpose.
#define ICON_CACHE_DIR "sdmc:/switch/freeshop/icon_cache"

#define TEMP_SCAN_ROOT "sdmc:/switch"
#define TEMP_SCAN_MAX_FILES 256
#define TEMP_SCAN_PATH_MAX 512

typedef struct {
    char paths[TEMP_SCAN_MAX_FILES][TEMP_SCAN_PATH_MAX];
    int count;
    int64_t total_bytes;
    bool truncated;
} FileScanResult;

static bool has_suffix_ci(const char *name, const char *suffix) {
    size_t nlen = strlen(name), slen = strlen(suffix);
    if (slen > nlen) return false;
    return strcasecmp(name + (nlen - slen), suffix) == 0;
}

static void format_size(int64_t bytes, char *out, size_t out_size) {
    double size = (double)bytes;
    const char *unit = "bytes";
    if (size >= 1024.0 * 1024.0 * 1024.0) { size /= 1024.0 * 1024.0 * 1024.0; unit = "GB"; }
    else if (size >= 1024.0 * 1024.0) { size /= 1024.0 * 1024.0; unit = "MB"; }
    else if (size >= 1024.0) { size /= 1024.0; unit = "KB"; }
    if (strcmp(unit, "bytes") == 0) {
        snprintf(out, out_size, "%lld bytes", (long long)bytes);
    } else {
        snprintf(out, out_size, "%.2f %s", size, unit);
    }
}

// Recursively finds every .part/.hdr file under `dir_path` - both are this
// app's own naming convention for in-progress downloads/header prefetches
// (see http_download_to_file/install_nsp_native.c's hdr_path), so this
// can't collide with some unrelated homebrew's own files.
static void scan_temp_files(const char *dir_path, FileScanResult *result) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char full_path[TEMP_SCAN_PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_temp_files(full_path, result);
        } else if (has_suffix_ci(ent->d_name, ".part") || has_suffix_ci(ent->d_name, ".hdr")) {
            if (result->count >= TEMP_SCAN_MAX_FILES) {
                result->truncated = true;
                continue;
            }
            snprintf(result->paths[result->count], TEMP_SCAN_PATH_MAX, "%s", full_path);
            result->total_bytes += st.st_size;
            result->count++;
        }
    }
    closedir(dir);
}

// Non-recursive - the icon cache is a flat folder of one file per app id.
static void scan_icon_cache(FileScanResult *result) {
    DIR *dir = opendir(ICON_CACHE_DIR);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (result->count >= TEMP_SCAN_MAX_FILES) {
            result->truncated = true;
            break;
        }

        char full_path[TEMP_SCAN_PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", ICON_CACHE_DIR, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) continue;

        snprintf(result->paths[result->count], TEMP_SCAN_PATH_MAX, "%s", full_path);
        result->total_bytes += st.st_size;
        result->count++;
    }
    closedir(dir);
}

static void cleanup_temp_files_action(void) {
    FileScanResult result = {0};
    scan_temp_files(TEMP_SCAN_ROOT, &result);

    if (result.count == 0) {
        ui_app_show_message("No se encontraron archivos temporales sueltos (.part/.hdr).");
        return;
    }

    char size_str[32];
    format_size(result.total_bytes, size_str, sizeof(size_str));
    char msg[300];
    snprintf(msg, sizeof(msg),
             "Se encontraron %d archivo(s) temporal(es) sueltos (%s%s).\n\n¿Eliminarlos?",
             result.count, size_str, result.truncated ? "+" : "");
    if (!ui_app_show_confirm(msg)) return;

    for (int i = 0; i < result.count; i++) {
        remove(result.paths[i]);
    }
    ui_app_show_message("Archivos temporales eliminados.");
}

static void cleanup_icon_cache_action(void) {
    FileScanResult result = {0};
    scan_icon_cache(&result);

    if (result.count == 0) {
        ui_app_show_message("La caché de íconos ya está vacía.");
        return;
    }

    char size_str[32];
    format_size(result.total_bytes, size_str, sizeof(size_str));
    char msg[300];
    snprintf(msg, sizeof(msg),
             "La caché de íconos tiene %d archivo(s) (%s%s).\n\n"
             "Se volverán a descargar la próxima vez que los necesites.\n\n¿Vaciarla?",
             result.count, size_str, result.truncated ? "+" : "");
    if (!ui_app_show_confirm(msg)) return;

    for (int i = 0; i < result.count; i++) {
        remove(result.paths[i]);
    }
    // Purge the in-memory texture cache too, or the current session would
    // keep showing already-loaded icons until the app restarts.
    ui_icons_clear();
    ui_app_show_message("Caché de íconos vaciada.");
}

static void cleanup_ncm_orphans_action(void) {
    ui_app_show_message("Buscando contenido huérfano en el registro de la consola...\n\nEsto puede tardar un momento.");

    NcmOrphanScanResult scan;
    char err_buf[128];
    if (!ncm_cleanup_scan_orphans(&scan, err_buf, sizeof(err_buf))) {
        char msg[200];
        snprintf(msg, sizeof(msg), "No se pudo escanear: %s", err_buf);
        ui_app_show_message(msg);
        return;
    }

    if (scan.count == 0) {
        ui_app_show_message("No se encontró contenido huérfano - todo lo instalado está en orden.");
        return;
    }

    char size_str[32];
    format_size(scan.total_bytes, size_str, sizeof(size_str));
    char msg[300];
    snprintf(msg, sizeof(msg),
             "Se encontraron %d contenido(s) huérfano(s) (%s%s) - restos de instalaciones "
             "que fallaron o se cancelaron a la mitad. No pertenecen a ningún juego instalado "
             "actualmente.\n\n¿Eliminarlos?",
             scan.count, size_str, scan.truncated ? "+" : "");
    if (!ui_app_show_confirm(msg)) return;

    ncm_cleanup_delete_orphans(scan.ids, scan.count);
    ui_app_show_message("Contenido huérfano eliminado.");
}

void ui_show_cleanup(void) {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    padUpdate(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        // Each *_action() below shows a message/confirm dialog on its own
        // separate PadState - re-prime `pad` afterward so whatever button
        // dismissed it doesn't look like a fresh press on the next
        // padUpdate() and immediately re-trigger the same action. See
        // ui_explorer.c's identical fix for the full explanation.
        bool nested_ui_shown = false;
        if (kDown & HidNpadButton_A) { cleanup_temp_files_action(); nested_ui_shown = true; }
        if (kDown & HidNpadButton_X) { cleanup_icon_cache_action(); nested_ui_shown = true; }
        if (kDown & HidNpadButton_Y) { cleanup_ncm_orphans_action(); nested_ui_shown = true; }
        if ((kDown & HidNpadButton_B) || (kDown & HidNpadButton_Plus)) return;

        if (nested_ui_shown) {
            padUpdate(&pad);
        }

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        ui_draw_text(g_font_title, LEFT_EDGE, 40, COLOR_TEXT, "Limpieza");
        ui_draw_text(g_font_body, LEFT_EDGE, 100, COLOR_TEXT_DIM,
                     "Cada opción revisa primero y muestra qué encontró antes de borrar nada.");

        ui_draw_rect(LEFT_EDGE, 160, SCREEN_W - LEFT_EDGE * 2, 70, COLOR_PANEL);
        ui_draw_text(g_font_body, LEFT_EDGE + 16, 180, COLOR_TEXT, "A - Archivos temporales sueltos (.part / .hdr)");
        ui_draw_text(g_font_small, LEFT_EDGE + 16, 206, COLOR_TEXT_DIM,
                     "Restos de descargas/instalaciones interrumpidas de golpe (cierre forzado de la app).");

        ui_draw_rect(LEFT_EDGE, 246, SCREEN_W - LEFT_EDGE * 2, 70, COLOR_PANEL);
        ui_draw_text(g_font_body, LEFT_EDGE + 16, 266, COLOR_TEXT, "X - Caché de íconos");
        ui_draw_text(g_font_small, LEFT_EDGE + 16, 292, COLOR_TEXT_DIM,
                     "Vacía sdmc:/switch/freeshop/icon_cache - se vuelven a descargar solos.");

        ui_draw_rect(LEFT_EDGE, 332, SCREEN_W - LEFT_EDGE * 2, 70, COLOR_PANEL);
        ui_draw_text(g_font_body, LEFT_EDGE + 16, 352, COLOR_TEXT, "Y - Contenido huérfano en NCM");
        ui_draw_text(g_font_small, LEFT_EDGE + 16, 378, COLOR_TEXT_DIM,
                     "Restos de instalaciones NSP/XCI que fallaron a la mitad, ocupando espacio sin mostrarse en ningún lado.");

        ui_draw_text(g_font_small, LEFT_EDGE, SCREEN_H - 46, COLOR_TEXT_DIM, "B/+: volver");

        SDL_RenderPresent(g_renderer);
    }
}
