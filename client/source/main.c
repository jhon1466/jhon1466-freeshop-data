#include <switch.h>
#include <curl/curl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "i18n.h"
#include "catalog/catalog.h"
#include "catalog/sources.h"
#include "ui/ui_app.h"
#include "ui/ui_list.h"
#include "ui/ui_detail.h"
#include "ui/ui_sources.h"
#include "ui/ui_icons.h"
#include "ui/ui_about.h"
#include "ui/ui_explorer.h"
#include "ui/ui_queue.h"
#include "ui/ui_prefs.h"
#include "ui/ui_sound.h"
#include "ui/ui_fx.h"
#include "install/install.h"
#include "install/install_nsp.h"
#include "install/install_nsp_native.h"
#include "install/install_xci_native.h"
#include "install/install_port.h"
#include "install/install_local.h"
#include "install/install_dispatch.h"
#include "update/self_update.h"

// Diagnostic-only: appends one line to sdmc:/switch/freeshop/update_debug.log
// covering the self-update chain-load hops, which are otherwise near-
// impossible to debug from a photo of a dialog that's already gone by the
// time it's captured. A handful of short lines per launch - not worth
// rotating/capping. Best-effort: a failure to open the log is silently
// ignored rather than interrupting startup over a debug aid.
static void update_debug_log(const char *fmt, ...) {
    FILE *fp = fopen("sdmc:/switch/freeshop/update_debug.log", "a");
    if (!fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

// Fetches each enabled source's catalog and concatenates them into one
// array. A source that fails to fetch is skipped rather than aborting the
// whole load - it's only a hard failure if every enabled source fails (or
// there are none). Each entry remembers which source it came from via
// AppEntry.source_base_url (stamped by catalog_fetch itself).
static CatalogResult fetch_merged_catalog(const SourceList *sources, AppEntry **out_entries,
                                           int *out_count, char *err_buf, size_t err_buf_size) {
    *out_entries = NULL;
    *out_count = 0;

    AppEntry *merged = NULL;
    int merged_count = 0;
    int ok_sources = 0;
    char last_err[200] = "";

    for (int i = 0; i < sources->count; i++) {
        if (!sources->items[i].enabled) continue;

        AppEntry *src_entries = NULL;
        int src_count = 0;
        char src_err[160];
        CatalogResult r = catalog_fetch(sources->items[i].base_url, &src_entries, &src_count,
                                         src_err, sizeof(src_err));
        if (r != CATALOG_OK) {
            snprintf(last_err, sizeof(last_err), "%s: %s", sources->items[i].name, src_err);
            continue;
        }
        ok_sources++;

        if (src_count > 0) {
            AppEntry *grown = (AppEntry *)realloc(merged, (size_t)(merged_count + src_count) * sizeof(AppEntry));
            if (!grown) {
                catalog_free(src_entries);
                free(merged);
                if (err_buf) snprintf(err_buf, err_buf_size, "memoria insuficiente combinando catálogos");
                return CATALOG_ERR_PARSE;
            }
            merged = grown;
            memcpy(merged + merged_count, src_entries, (size_t)src_count * sizeof(AppEntry));
            merged_count += src_count;
        }
        catalog_free(src_entries);
    }

    if (ok_sources == 0) {
        free(merged);
        if (err_buf) {
            snprintf(err_buf, err_buf_size, "%s",
                     last_err[0] ? last_err : "no hay fuentes de catálogo habilitadas");
        }
        return CATALOG_ERR_NETWORK;
    }

    *out_entries = merged;
    *out_count = merged_count;
    return CATALOG_OK;
}

// Filters `entries` down to a value-copy array of just the "root" entries
// (empty parent_id) - DLC/updates (non-empty parent_id) are never shown in
// the main list/grid, only surfaced via their base game's detail screen
// (see ui_show_detail). AppEntry has no internal heap ownership (fixed-size
// buffers only), so copying by value and free()-ing this array separately
// from `entries` is safe. Caller must free() the result.
static AppEntry *build_root_entries(const AppEntry *entries, int count, int *out_root_count) {
    AppEntry *root = (AppEntry *)malloc((size_t)(count > 0 ? count : 1) * sizeof(AppEntry));
    int n = 0;
    if (root) {
        for (int i = 0; i < count; i++) {
            if (entries[i].parent_id[0] == '\0') root[n++] = entries[i];
        }
    }
    *out_root_count = n;
    return root;
}

// Fixed cap on DLC/updates shown per game - generous for a homebrew catalog,
// avoids a heap allocation for what's normally a handful of entries.
#define APP_DLC_MAX 32

// Scans the full `entries` for rows whose parent_id matches `game_id`,
// copying up to APP_DLC_MAX of them into `out_dlc` (caller-owned, on the
// stack). Returns how many were found.
static int find_dlc_entries(const AppEntry *entries, int count, const char *game_id, AppEntry *out_dlc) {
    int n = 0;
    for (int i = 0; i < count && n < APP_DLC_MAX; i++) {
        if (entries[i].parent_id[0] != '\0' && strcmp(entries[i].parent_id, game_id) == 0) {
            out_dlc[n++] = entries[i];
        }
    }
    return n;
}

// Last-resort fallback if the graphics stack (SDL2/TTF/pl) fails to
// initialize - shows the failure as readable text via the plain libnx
// console instead of a black screen or silent hang. Never reached once
// ui_app_init() has succeeded.
static void fallback_console_error(const char *msg) {
    consoleInit(NULL);
    printf("%s\n\nPresiona + para salir.\n", msg);
    consoleUpdate(NULL);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
}

// Wraps the pad (for cancel detection) with timing state for the
// speed/ETA estimate below - install_progress_cb is reused across every
// install, so this must be freshly reset (start_tick/started) before each
// individual download, not a static/persisted-across-installs value.
typedef struct {
    PadState *pad;
    u64 start_tick;
    bool started;
    u64 last_render_tick;
    // Defaults to downloading - installers that also write content to NCM
    // storage (install_nsp_native, install_xci_native) flip this via
    // on_install_phase once that phase actually starts.
    InstallPhase phase;
    // What's being installed, shown under the phase heading: the catalog
    // title, a filename for a local install, "FreeShop" for a self-update.
    // May be NULL, in which case no name line is drawn at all rather than
    // showing something that could be mistaken for a real title.
    const char *title;
} InstallProgressCtx;

static void on_install_phase(InstallPhase phase, void *userdata) {
    InstallProgressCtx *ctx = (InstallProgressCtx *)userdata;
    if (ctx) ctx->phase = phase;
}

// Only actually does any work (cancel check + redraw) at most this often -
// see the comment below on why.
#define INSTALL_WORK_INTERVAL_NS 150000000ULL // ~6-7 times/sec

// Returns false (cancel) once the user holds B - held rather than
// edge-triggered because this only actually gets checked a handful of
// times a second (see below), and a single-frame press could land between
// two checks.
static bool install_progress_cb(long total, long now, void *userdata) {
    InstallProgressCtx *ctx = (InstallProgressCtx *)userdata;

    // curl can call this progress callback far more often than either a
    // cancel check or a redraw is actually useful. Two things used to run
    // on every single call: padUpdate() (an IPC round-trip to the hid
    // service) and a full render - the render alone does several
    // *uncached* TTF glyph rasterizations (ui_draw_text creates and
    // destroys a texture every call) plus an SDL_RenderPresent that blocks
    // for vsync (~16ms at 60Hz). Doing that on every callback call was
    // found to serialize a large chunk of CPU time behind this callback
    // instead of letting curl actually drain the socket, throttling real
    // download speed to a fraction of what the network/CPU could otherwise
    // do - this looked exactly like a network or crypto bottleneck (tried
    // IPv4 forcing, cipher selection, bigger buffers - none of it helped)
    // because nothing about the transfer itself was the problem. Gating
    // both the IPC call and the render behind the same throttle, instead of
    // just the render, means curl runs completely uninterrupted by this
    // callback between checks - only a cheap tick comparison happens then.
    u64 now_tick = armGetSystemTick();
    if (ctx && armTicksToNs(now_tick - ctx->last_render_tick) < INSTALL_WORK_INTERVAL_NS) {
        return true;
    }
    if (ctx) ctx->last_render_tick = now_tick;

    PadState *pad = ctx ? ctx->pad : NULL;
    bool cancel = false;
    if (pad) {
        padUpdate(pad);
        cancel = (padGetButtons(pad) & HidNpadButton_B) != 0;
    }

    if (ctx && !ctx->started) {
        ctx->start_tick = now_tick;
        ctx->started = true;
    }

    SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
    SDL_RenderClear(g_renderer);

    // No animated background or wrapped title here on purpose - this
    // callback fires several times a second for the entire duration of a
    // download/install, competing for CPU with the network transfer and NCM
    // writes themselves. The glow background's texture blending noticeably
    // slowed real installs when tried here; kept on screens that sit idle
    // (list, detail, about, the queue's browse/results views).
    const char *phase_title = (ctx && ctx->phase == INSTALL_PHASE_INSTALLING) ? "Instalando..." : "Descargando...";
    ui_draw_text(g_font_title, 90, 250, COLOR_TEXT, phase_title);

    // Which title this is - truncated to one line rather than wrapped:
    // wrapping would rasterize several lines on every one of these
    // redraws, and this callback is deliberately kept cheap (see above).
    if (ctx && ctx->title && ctx->title[0]) {
        char name_line[160];
        ui_truncate_to_width(g_font_body, ctx->title, 1100, name_line, sizeof(name_line));
        ui_draw_text(g_font_body, 90, 300, COLOR_TEXT, name_line);
    }

    char line[96];
    float pct = 0.0f;
    char done_str[32];
    ui_format_bytes(now, done_str, sizeof(done_str));
    if (total > 0) {
        pct = (float)now / (float)total;
        char total_str[32];
        ui_format_bytes(total, total_str, sizeof(total_str));
        snprintf(line, sizeof(line), "%d%%   (%s / %s)", (int)(pct * 100), done_str, total_str);
    } else {
        snprintf(line, sizeof(line), "%s descargados", done_str);
    }
    ui_draw_text(g_font_body, 90, 340, COLOR_TEXT_DIM, line);
    ui_draw_progress_bar(90, 380, 1100, 24, pct, COLOR_ACCENT, COLOR_PANEL);

    // Average speed since the download started (simple and robust - no
    // ring buffer to size/manage - trades a little responsiveness to
    // sudden speed changes for that simplicity). Skipped for the first
    // half second so an early, noisy sample doesn't flash a wild estimate.
    if (ctx && ctx->started && total > 0 && now > 0) {
        double elapsed_sec = armTicksToNs(armGetSystemTick() - ctx->start_tick) / 1e9;
        if (elapsed_sec > 0.5) {
            double bytes_per_sec = now / elapsed_sec;
            if (bytes_per_sec > 0) {
                double remaining_sec = (total - now) / bytes_per_sec;
                int mins = (int)(remaining_sec / 60);
                int secs = (int)remaining_sec % 60;
                char speed_line[64];
                snprintf(speed_line, sizeof(speed_line), "%.1f MB/s - tiempo restante: %dm %ds",
                         bytes_per_sec / (1024.0 * 1024.0), mins, secs);
                ui_draw_text(g_font_small, 90, 420, COLOR_TEXT_DIM, speed_line);
            }
        }
    }

    ui_draw_text(g_font_small, 90, 450, COLOR_TEXT_DIM, "B: cancelar");

    SDL_RenderPresent(g_renderer);

    return !cancel;
}

int main(int argc, char **argv) {
    // hbmenu passes the running .nro's own sdmc path as argv[0] - needed for
    // self_update_apply() to know what file to replace. NULL/empty when not
    // launched that way (e.g. nxlink during development) - self-update just
    // skips itself in that case rather than failing outright.
    const char *self_path = (argc > 0 && argv && argv[0] && argv[0][0] != '\0') ? argv[0] : NULL;
    update_debug_log("main() start: argc=%d argv[0]=%s", argc, self_path ? self_path : "(null)");

    char err_buf[256];

    if (!ui_app_init(err_buf, sizeof(err_buf))) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Error al iniciar los gráficos:\n%s", err_buf);
        fallback_console_error(msg);
        return 1;
    }

    // Mounts this .nro's embedded RomFS (client/romfs/ at build time) as
    // romfs:/ - static assets that must always be available regardless of
    // network (the donation QR shown in "Acerca de"). Best-effort: a
    // failure here just means that one image doesn't load later, not worth
    // aborting startup over.
    romfsInit();

    // Picks the language every tr() call after this point returns in - see
    // i18n.h. Cheap (one service call), and every screen this app draws
    // needs it, so it happens as early as possible.
    i18n_init();

    // Ambient background effects and UI tones, both honoring whatever the
    // user last set (defaulting to on). Failures inside these are
    // deliberately silent - they're decoration, and a console that can't
    // open an audio device should still get a working store.
    {
        UiListPrefs startup_prefs;
        ui_prefs_load(&startup_prefs);
        ui_fx_init();
        ui_fx_set_enabled(!startup_prefs.effects_disabled);
        ui_sound_init();
        ui_sound_set_enabled(!startup_prefs.sound_disabled);
    }

    // Without this, the console dims/sleeps on its own inactivity timer
    // regardless of what's happening in the app - including mid-download,
    // where curl keeps running but there's no controller input for
    // however long the transfer takes. Best-effort (ignore failure - worst
    // case the console behaves as if this call was never made).
    appletSetAutoSleepDisabled(true);

    // Second hop of a self-update (see update/self_update.h): this process
    // is running from the ".update" staging file a previous launch
    // downloaded and chain-loaded into specifically so it could safely
    // replace the canonical path - confirmed on real hardware that a
    // running .nro can't remove()/rename() *itself* (the very first
    // diagnostic report after adding errno logging showed rename() failing
    // with EEXIST on the same path remove() had just reported ENOENT for -
    // nx-hbloader evidently keeps the file open/locked for the life of the
    // process). Deliberately minimal here (no socket/curl init) - this
    // hop's only job is the swap-and-relaunch, and the less it does before
    // that, the less that can go wrong along the way.
    bool is_staging = self_update_is_staging_copy(self_path);
    update_debug_log("is_staging_copy(%s) = %s", self_path ? self_path : "(null)", is_staging ? "true" : "false");
    if (is_staging) {
        char canonical_path[512];
        char swap_err[256];
        SelfUpdateSwapResult sres = self_update_finish_swap(self_path, canonical_path, sizeof(canonical_path),
                                                              swap_err, sizeof(swap_err));
        if (sres == SELF_UPDATE_SWAP_OK) {
            // The file swap itself already succeeded at this point - only
            // the automatic relaunch depends on chain-load support.
            bool has_next_load = envHasNextLoad();
            update_debug_log("finish_swap OK -> canonical=%s, envHasNextLoad=%s",
                              canonical_path, has_next_load ? "true" : "false");
            if (has_next_load) {
                Result nrc = envSetNextLoad(canonical_path, canonical_path);
                update_debug_log("envSetNextLoad(canonical) rc=0x%x", nrc);
                if (R_SUCCEEDED(nrc)) {
                    appletSetAutoSleepDisabled(false);
                    romfsExit();
                    ui_app_shutdown();
                    return 0;
                }
            }
            // Can't auto-relaunch here - the canonical file is already
            // updated on disk, just not what this process is running from
            // anymore. Tell the user and keep going as-is (this process'
            // in-memory copy is still fully functional).
            ui_app_show_message("Actualización completada.\n\nCierra la app y ábrela de nuevo desde el hbmenu "
                                 "para terminar de usar la nueva versión.");
        } else {
            update_debug_log("finish_swap FAILED: %s", swap_err);
            // Couldn't finish the swap - rather than getting the user stuck
            // (the staging file is itself a fully valid, working build,
            // just not at hbmenu's "normal" filename), tell them and keep
            // going as-is.
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "No se pudo completar la actualización: %s\n\nLa app sigue funcionando, pero puede que "
                     "vuelva a pedir la actualización la próxima vez que la abras.",
                     swap_err);
            ui_app_show_message(msg);
        }
    }

    // socketInitializeDefault()'s stock TCP receive buffer (0x40000 max) is
    // the download-speed ceiling for a distant server: TCP throughput is
    // capped at roughly (receive window / round-trip time), and window
    // scaling can only grow the window up to tcp_rx_buf_max_size. At
    // ~150-200ms RTT to a far CDN, a 256-512KB window caps you near
    // ~1-3 MB/s no matter how fast the server or SD card is - which matches
    // the "most downloads sit at ~0.8 MB/s" reports. Raising the max window
    // to 2MB lifts that ceiling to ~10 MB/s+ at those RTTs. sb_efficiency
    // (buffers per socket) bumped too so a single download socket can keep
    // more data in flight. These bigger buffers grow the transfer-memory
    // pool socketInitialize allocates, so if that allocation fails (tighter
    // applet-mode memory), fall back to the modest config that was working
    // before rather than leaving networking dead.
    SocketInitConfig socket_config = *socketGetDefaultInitConfig();
    socket_config.tcp_tx_buf_size = 0x8000;
    socket_config.tcp_rx_buf_size = 0x100000;      // 1 MB initial
    socket_config.tcp_tx_buf_max_size = 0x40000;
    socket_config.tcp_rx_buf_max_size = 0x200000;  // 2 MB max window
    socket_config.sb_efficiency = 8;

    Result rc = socketInitialize(&socket_config);
    if (R_FAILED(rc)) {
        // Fall back to the previous, smaller config (still above the stock
        // defaults) before giving up entirely.
        SocketInitConfig fallback = *socketGetDefaultInitConfig();
        fallback.tcp_tx_buf_size = 0x25000;
        fallback.tcp_rx_buf_size = 0x25000;
        fallback.tcp_tx_buf_max_size = 0x80000;
        fallback.tcp_rx_buf_max_size = 0x80000;
        rc = socketInitialize(&fallback);
    }
    if (R_FAILED(rc)) {
        ui_app_show_message("No se pudo iniciar la red.");
        appletSetAutoSleepDisabled(false);
        romfsExit();
        ui_app_shutdown();
        return 1;
    }

    // Never called anywhere in this codebase before - curl_easy_init()
    // implicitly does a lazy CURL_GLOBAL_DEFAULT init if this is skipped,
    // but that path is documented as not thread-safe and, on constrained
    // SSL backends like mbedtls, can leave the entropy/RNG source set up
    // incorrectly. Plausible root cause of every HTTPS request (any host)
    // hanging until timeout, while the original plain-HTTP LAN setup never
    // exercised the SSL backend at all. Must run once, before any curl call.
    curl_global_init(CURL_GLOBAL_ALL);

    // Best-effort - a failed check is silent (no need to alarm the user
    // every launch over a network hiccup); only a genuinely available
    // update interrupts them, and only with their confirmation before
    // anything is downloaded/replaced.
    char new_version[32];
    char asset_url[512];
    char release_notes[300];
    SelfUpdateCheckResult cres_update = self_update_check(new_version, sizeof(new_version), asset_url,
                                                            sizeof(asset_url), release_notes, sizeof(release_notes),
                                                            NULL, 0);
    update_debug_log("self_update_check = %s (current=%s%s%s)",
                      cres_update == SELF_UPDATE_AVAILABLE ? "AVAILABLE" :
                          (cres_update == SELF_UPDATE_NONE ? "NONE" : "ERR"),
                      CLIENT_VERSION,
                      cres_update == SELF_UPDATE_AVAILABLE ? ", new=" : "",
                      cres_update == SELF_UPDATE_AVAILABLE ? new_version : "");
    if (cres_update == SELF_UPDATE_AVAILABLE) {
        char confirm_msg[512];
        if (release_notes[0]) {
            // GitHub's release "body" text - whatever changelog the release
            // was published with, so the user can see what changed before
            // agreeing to update.
            snprintf(confirm_msg, sizeof(confirm_msg),
                     "Hay una nueva versión disponible: v%s (actual: v%s).\n\n%s\n\n¿Actualizar ahora?",
                     new_version, CLIENT_VERSION, release_notes);
        } else {
            snprintf(confirm_msg, sizeof(confirm_msg),
                     "Hay una nueva versión disponible: v%s (actual: v%s).\n\n¿Actualizar ahora?",
                     new_version, CLIENT_VERSION);
        }
        if (!self_path) {
            snprintf(confirm_msg, sizeof(confirm_msg),
                     "Hay una nueva versión disponible (v%s), pero no se pudo determinar dónde "
                     "está instalada esta app para actualizarla automáticamente.",
                     new_version);
            ui_app_show_message(confirm_msg);
        } else if (ui_app_show_confirm(confirm_msg)) {
            PadState update_pad;
            padConfigureInput(1, HidNpadStyleSet_NpadStandard);
            padInitializeDefault(&update_pad);
            InstallProgressCtx update_ctx = { .pad = &update_pad, .start_tick = 0, .started = false,
                                               .phase = INSTALL_PHASE_DOWNLOADING, .title = "FreeShop" };
            char staging_path[512];
            char update_err[256];
            SelfUpdateApplyResult ares = self_update_apply(self_path, asset_url, staging_path, sizeof(staging_path),
                                                             install_progress_cb, &update_ctx,
                                                             update_err, sizeof(update_err));
            update_debug_log("self_update_apply = %s (%s)",
                              ares == SELF_UPDATE_APPLY_OK ? "OK" : "ERR",
                              ares == SELF_UPDATE_APPLY_OK ? staging_path : update_err);
            if (ares == SELF_UPDATE_APPLY_OK) {
                // Prefer chain-loading straight into the staging copy: it
                // finishes the update (see the staging-copy handling right
                // after romfsInit above) and relaunches itself into the
                // canonical path on its own, so the user just sees a couple
                // of quick screen flashes rather than having to manually
                // close and reopen the app. Only actually works in launch
                // environments nx-hbloader chain-loading is available in
                // (plain hbmenu launches - some NSP-forwarder-based setups
                // don't support it) - check both envHasNextLoad() and
                // envSetNextLoad()'s own Result rather than assuming, and
                // fall back to the old direct in-place overwrite if not.
                bool has_next_load = envHasNextLoad();
                update_debug_log("envHasNextLoad = %s", has_next_load ? "true" : "false");
                if (has_next_load) {
                    Result nrc = envSetNextLoad(staging_path, staging_path);
                    update_debug_log("envSetNextLoad(staging) rc=0x%x", nrc);
                    if (R_SUCCEEDED(nrc)) {
                        curl_global_cleanup();
                        socketExit();
                        appletSetAutoSleepDisabled(false);
                        romfsExit();
                        ui_app_shutdown();
                        return 0;
                    }
                }
                char inplace_err[256];
                SelfUpdateSwapResult ires = self_update_swap_in_place(staging_path, self_path,
                                                                       inplace_err, sizeof(inplace_err));
                update_debug_log("swap_in_place = %s%s%s",
                                  ires == SELF_UPDATE_SWAP_OK ? "OK" : "ERR",
                                  ires == SELF_UPDATE_SWAP_OK ? "" : ": ", ires == SELF_UPDATE_SWAP_OK ? "" : inplace_err);
                if (ires == SELF_UPDATE_SWAP_OK) {
                    ui_app_show_message("Actualización descargada.\n\nCierra la app y ábrela de nuevo desde el "
                                         "hbmenu para usar la nueva versión.");
                } else {
                    char err_msg[400];
                    snprintf(err_msg, sizeof(err_msg),
                             "No se pudo actualizar: %s\n\nSigues usando la versión actual.", inplace_err);
                    ui_app_show_message(err_msg);
                }
            } else {
                char err_msg[320];
                snprintf(err_msg, sizeof(err_msg), "No se pudo actualizar: %s\n\nSigues usando la versión actual.",
                         update_err);
                ui_app_show_message(err_msg);
            }
        }
    }

    SourceList sources;
    sources_load(&sources);

    AppEntry *entries = NULL;
    int count = 0;

    CatalogResult cres = fetch_merged_catalog(&sources, &entries, &count, err_buf, sizeof(err_buf));
    if (cres != CATALOG_OK) {
        // Not fatal: the explorer, cleanup, and sources screens (all
        // reachable from the same list screen ui_show_list draws below -
        // see UI_LIST_OPEN_EXPLORER etc.) work entirely off the SD card and
        // need no network at all. Bailing out here used to mean "no
        // internet" also meant "the app doesn't open", which made those
        // offline-only tools unreachable exactly when there's no connection
        // to retry the catalog with. `entries`/`count` are left at their
        // NULL/0 initializers - ui_show_list already renders that as
        // "(el catálogo está vacío)" rather than crashing, and Stick L
        // (UI_LIST_RELOAD_CATALOG) or the Fuentes screen let the catalog
        // load once a connection is available, with no restart needed.
        char msg[640];
        snprintf(msg, sizeof(msg), tr(STR_MAIN_CATALOG_LOAD_ERROR_TEMPLATE), err_buf);
        ui_app_show_message(msg);
    }

    int root_count = 0;
    AppEntry *root_entries = build_root_entries(entries, count, &root_count);

    bool running = true;
    while (running && appletMainLoop()) {
        int selected = ui_show_list(root_entries, root_count);

        if (selected == UI_LIST_OPEN_SOURCES) {
            bool changed = ui_show_sources(&sources);
            if (changed) {
                sources_save(&sources);
                // Fetch into fresh locals first and only swap them in on
                // success - mirrors UI_LIST_RELOAD_CATALOG below. Freeing
                // the old catalog unconditionally (as this used to) meant
                // toggling a source while offline, or while every source
                // happens to be unreachable, left the app with an empty
                // catalog even though the one it had a second ago was fine.
                AppEntry *new_entries = NULL;
                int new_count = 0;
                CatalogResult cres2 = fetch_merged_catalog(&sources, &new_entries, &new_count,
                                                            err_buf, sizeof(err_buf));
                if (cres2 != CATALOG_OK) {
                    char msg[640];
                    snprintf(msg, sizeof(msg), tr(STR_MAIN_CATALOG_RELOAD_KEEP_OLD_TEMPLATE), err_buf);
                    ui_app_show_message(msg);
                } else {
                    // Ids may now refer to different apps under a new/changed
                    // source - stale cached textures could otherwise be shown
                    // under the wrong entry. Only cleared once the new
                    // catalog is confirmed good, not before.
                    ui_icons_clear();
                    catalog_free(entries);
                    entries = new_entries;
                    count = new_count;
                    free(root_entries);
                    root_entries = build_root_entries(entries, count, &root_count);
                }
            }
            continue;
        }

        if (selected == UI_LIST_RELOAD_CATALOG) {
            // Re-fetches from the same sources as a normal launch - for new
            // apps added to a source since this session started, without
            // needing to fully close and reopen the client to see them.
            // Unlike UI_LIST_OPEN_SOURCES's reload, ids of already-known
            // apps don't change here, so cached icon textures stay valid -
            // no ui_icons_clear() needed, only genuinely new ids will end
            // up fetching anything.
            AppEntry *new_entries = NULL;
            int new_count = 0;
            CatalogResult cres2 = fetch_merged_catalog(&sources, &new_entries, &new_count,
                                                        err_buf, sizeof(err_buf));
            if (cres2 != CATALOG_OK) {
                char msg[640];
                snprintf(msg, sizeof(msg), tr(STR_MAIN_CATALOG_RELOAD_KEEP_OLD_TEMPLATE), err_buf);
                ui_app_show_message(msg);
            } else {
                catalog_free(entries);
                entries = new_entries;
                count = new_count;
                free(root_entries);
                root_entries = build_root_entries(entries, count, &root_count);
            }
            continue;
        }

        if (selected == UI_LIST_OPEN_QUEUE) {
            // The queue can hold DLC/update ids too (queued from the detail
            // screen), which aren't in root_entries - pass the full catalog
            // so ui_show_queue can resolve every queued id. It handles
            // browsing, starting, and installing the whole batch itself.
            ui_show_queue(entries, count);
            continue;
        }

        if (selected == UI_LIST_OPEN_ABOUT) {
            ui_show_about();
            continue;
        }

        if (selected == UI_LIST_OPEN_EXPLORER) {
            char local_path[512];
            bool local_is_xci = false;
            UiExplorerAction eaction = ui_show_explorer(local_path, sizeof(local_path), &local_is_xci);
            if (eaction != UI_EXPLORER_INSTALL) {
                continue;
            }

            char local_msg[512];
            PadState local_pad;
            padConfigureInput(1, HidNpadStyleSet_NpadStandard);
            padInitializeDefault(&local_pad);
            // A local install never has a separate download step - starting
            // the phase at INSTALLING (instead of DOWNLOADING, like the
            // catalog install path below) makes the progress screen show
            // "Instalando..." from the first frame instead of briefly
            // (and wrongly) claiming to be downloading.
            // No catalog title for a local install - its filename, without
            // the directory part, is the closest thing to one.
            const char *local_title = strrchr(local_path, '/');
            local_title = local_title ? local_title + 1 : local_path;

            InstallProgressCtx local_ctx = { .pad = &local_pad, .start_tick = 0, .started = false,
                                              .phase = INSTALL_PHASE_INSTALLING, .title = local_title };

            InstallLocalResult lres = local_is_xci
                ? install_xci_from_local_file(local_path, install_progress_cb, on_install_phase, &local_ctx,
                                               err_buf, sizeof(err_buf))
                : install_nsp_from_local_file(local_path, install_progress_cb, on_install_phase, &local_ctx,
                                               err_buf, sizeof(err_buf));

            if (lres == INSTALL_LOCAL_OK) {
                ui_app_show_message("Instalado correctamente.\n\nVuelve al hbmenu para iniciarlo.");
            } else if (lres == INSTALL_LOCAL_ERR_CANCELED) {
                ui_app_show_message("Instalación cancelada.");
            } else {
                snprintf(local_msg, sizeof(local_msg), "Error de instalación: %s", err_buf);
                ui_app_show_message(local_msg);
            }
            continue;
        }

        if (selected < 0) {
            running = false;
            break;
        }

        AppEntry dlc_entries[APP_DLC_MAX];
        int dlc_count = find_dlc_entries(entries, count, root_entries[selected].id, dlc_entries);

        const AppEntry *install_target = NULL;
        UiDetailAction action = ui_show_detail(&root_entries[selected], dlc_entries, dlc_count, &install_target);
        if (action != UI_DETAIL_INSTALL && action != UI_DETAIL_INSTALL_DBI) {
            continue;
        }

        char msg[512];

        PadState install_pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&install_pad);
        InstallProgressCtx progress_ctx = { .pad = &install_pad, .start_tick = 0, .started = false,
                                             .phase = INSTALL_PHASE_DOWNLOADING,
                                             .title = install_target->title };

        // Entries from different enabled sources need their own base URL,
        // not a single global one - see AppEntry.source_base_url.
        const char *base_url = install_target->source_base_url;

        // Drop every cached icon texture before installing. A full grid of
        // them is by far this app's largest memory consumer (up to
        // ICON_CACHE_MAX 256x256 RGBA textures - see ui_icons.c), and an
        // install is precisely when that memory is needed elsewhere: NSZ
        // decompression asks zstd for a multi-MB window buffer and was
        // failing outright with "not enough memory" on real hardware. The
        // install screen draws no icons anyway, and the list re-populates
        // from the on-SD icon cache (no refetch) on the way back.
        ui_icons_clear();

        // UI_DETAIL_INSTALL_DBI is the explicit manual fallback offered for
        // both NSP and XCI (see ui_detail.c) - only that goes through the
        // DBI hand-off now.
        if (action == UI_DETAIL_INSTALL_DBI) {
            NspHandoffResult nres = install_nsp_and_launch_dbi(install_target, base_url,
                                                                 install_progress_cb, &progress_ctx,
                                                                 err_buf, sizeof(err_buf));
            if (nres == NSP_HANDOFF_OK) {
                snprintf(msg, sizeof(msg),
                         "\"%s\" descargado.\n\nAbriendo DBI - en DBI: Explorar tarjeta SD "
                         "> DBI > nsp-repo, selecciona el archivo e instálalo.",
                         install_target->title);
                ui_app_show_message(msg);
                // Chain-load is armed (envSetNextLoad) - only takes effect once this
                // process exits normally, so stop the loop and fall through to cleanup.
                running = false;
            } else if (nres == NSP_HANDOFF_ERR_CANCELED) {
                ui_app_show_message("Descarga cancelada.");
            } else {
                snprintf(msg, sizeof(msg), "Error de instalación: %s", err_buf);
                ui_app_show_message(msg);
            }
            continue;
        }

        InstallOneResult ires = install_one_entry(install_target, install_progress_cb, on_install_phase,
                                                  &progress_ctx, err_buf, sizeof(err_buf));
        if (ires == INSTALL_ONE_OK) {
            snprintf(msg, sizeof(msg), "\"%s\" instalado correctamente.\n\nVuelve al hbmenu para iniciarlo.",
                     install_target->title);
            ui_app_show_message(msg);
        } else if (ires == INSTALL_ONE_CANCELED) {
            ui_app_show_message("Descarga cancelada.");
        } else if (install_suggests_dbi_fallback(install_target->file_type)) {
            snprintf(msg, sizeof(msg),
                     "Error de instalación: %s\n\nSi el problema persiste, prueba \"Instalar vía DBI\" (botón X) desde esta misma pantalla.",
                     err_buf);
            ui_app_show_message(msg);
        } else {
            snprintf(msg, sizeof(msg), "Error de instalación: %s", err_buf);
            ui_app_show_message(msg);
        }
    }

    catalog_free(entries);
    free(root_entries);
    ui_icons_clear();
    ui_fx_shutdown();
    ui_sound_shutdown();
    curl_global_cleanup();
    socketExit();
    // Restore normal auto-sleep behavior before handing control back to
    // hbmenu (or chain-loading into DBI/a self-update, which each get a
    // fresh applet session anyway, but this is cheap and leaves nothing to
    // chance either way).
    appletSetAutoSleepDisabled(false);
    romfsExit();
    ui_app_shutdown();
    return 0;
}
