#include <switch.h>
#include <curl/curl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "catalog/catalog.h"
#include "catalog/sources.h"
#include "ui/ui_app.h"
#include "ui/ui_list.h"
#include "ui/ui_detail.h"
#include "ui/ui_sources.h"
#include "ui/ui_icons.h"
#include "install/install.h"
#include "install/install_nsp.h"
#include "install/install_nsp_native.h"
#include "install/install_xci_native.h"
#include "install/install_port.h"
#include "update/self_update.h"

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

    const char *title = (ctx && ctx->phase == INSTALL_PHASE_INSTALLING) ? "Instalando..." : "Descargando...";
    ui_draw_text(g_font_title, 90, 260, COLOR_TEXT, title);

    char line[64];
    float pct = 0.0f;
    if (total > 0) {
        pct = (float)now / (float)total;
        snprintf(line, sizeof(line), "%d%% (%ld / %ld bytes)", (int)(pct * 100), now, total);
    } else {
        snprintf(line, sizeof(line), "%ld bytes descargados", now);
    }
    ui_draw_text(g_font_body, 90, 320, COLOR_TEXT_DIM, line);
    ui_draw_progress_bar(90, 370, 1100, 24, pct, COLOR_ACCENT, COLOR_PANEL);

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
                ui_draw_text(g_font_small, 90, 410, COLOR_TEXT_DIM, speed_line);
            }
        }
    }

    ui_draw_text(g_font_small, 90, 440, COLOR_TEXT_DIM, "B: cancelar");

    SDL_RenderPresent(g_renderer);

    return !cancel;
}

int main(int argc, char **argv) {
    // hbmenu passes the running .nro's own sdmc path as argv[0] - needed for
    // self_update_apply() to know what file to replace. NULL/empty when not
    // launched that way (e.g. nxlink during development) - self-update just
    // skips itself in that case rather than failing outright.
    const char *self_path = (argc > 0 && argv && argv[0] && argv[0][0] != '\0') ? argv[0] : NULL;

    char err_buf[256];

    if (!ui_app_init(err_buf, sizeof(err_buf))) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Error al iniciar los gráficos:\n%s", err_buf);
        fallback_console_error(msg);
        return 1;
    }

    // Without this, the console dims/sleeps on its own inactivity timer
    // regardless of what's happening in the app - including mid-download,
    // where curl keeps running but there's no controller input for
    // however long the transfer takes. Best-effort (ignore failure - worst
    // case the console behaves as if this call was never made).
    appletSetAutoSleepDisabled(true);

    // socketInitializeDefault()'s stock buffer sizes are tuned for small
    // LAN traffic and are a known source of stalls on real hardware once
    // TLS is involved: a big CDN's handshake (cert chain, etc.) can arrive
    // in a burst larger than the default TCP receive buffer, and everything
    // after that stalls until curl's own timeout gives up. Bump them well
    // above the defaults before doing anything HTTPS.
    SocketInitConfig socket_config = *socketGetDefaultInitConfig();
    socket_config.tcp_tx_buf_size = 0x25000;
    socket_config.tcp_rx_buf_size = 0x25000;
    socket_config.tcp_tx_buf_max_size = 0x80000;
    socket_config.tcp_rx_buf_max_size = 0x80000;

    Result rc = socketInitialize(&socket_config);
    if (R_FAILED(rc)) {
        ui_app_show_message("No se pudo iniciar la red.");
        appletSetAutoSleepDisabled(false);
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
    if (self_update_check(new_version, sizeof(new_version), asset_url, sizeof(asset_url),
                           NULL, 0) == SELF_UPDATE_AVAILABLE) {
        char confirm_msg[256];
        snprintf(confirm_msg, sizeof(confirm_msg),
                 "Hay una nueva versión disponible: v%s (actual: v%s).\n\n¿Actualizar ahora?",
                 new_version, CLIENT_VERSION);
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
                                               .phase = INSTALL_PHASE_DOWNLOADING };
            char update_err[256];
            SelfUpdateApplyResult ares = self_update_apply(self_path, asset_url, install_progress_cb, &update_ctx,
                                                            update_err, sizeof(update_err));
            if (ares == SELF_UPDATE_APPLY_OK) {
                ui_app_show_message("Actualización descargada.\n\nCierra la app y ábrela de nuevo desde el "
                                     "hbmenu para usar la nueva versión.");
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
        char msg[640];
        snprintf(msg, sizeof(msg), "No se pudo cargar el catálogo:\n%s", err_buf);
        ui_app_show_message(msg);
        curl_global_cleanup();
        socketExit();
        appletSetAutoSleepDisabled(false);
        ui_app_shutdown();
        return 1;
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
                catalog_free(entries);
                entries = NULL;
                count = 0;
                free(root_entries);
                root_entries = NULL;
                root_count = 0;
                // Ids may now refer to different apps under a new/changed
                // source - stale cached textures could otherwise be shown
                // under the wrong entry.
                ui_icons_clear();
                CatalogResult cres2 = fetch_merged_catalog(&sources, &entries, &count,
                                                            err_buf, sizeof(err_buf));
                if (cres2 != CATALOG_OK) {
                    char msg[640];
                    snprintf(msg, sizeof(msg), "No se pudo cargar el catálogo:\n%s", err_buf);
                    ui_app_show_message(msg);
                } else {
                    root_entries = build_root_entries(entries, count, &root_count);
                }
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
                                             .phase = INSTALL_PHASE_DOWNLOADING };

        // Entries from different enabled sources need their own base URL,
        // not a single global one - see AppEntry.source_base_url.
        const char *base_url = install_target->source_base_url;

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

        if (install_target->file_type == APP_FILE_TYPE_NSP) {
            NspInstallResult nires = install_nsp_native(install_target, base_url,
                                                          install_progress_cb, on_install_phase, &progress_ctx,
                                                          err_buf, sizeof(err_buf));
            if (nires == NSP_INSTALL_OK) {
                snprintf(msg, sizeof(msg), "\"%s\" instalado correctamente.\n\nVuelve al hbmenu para iniciarlo.",
                         install_target->title);
                ui_app_show_message(msg);
            } else if (nires == NSP_INSTALL_ERR_CANCELED) {
                ui_app_show_message("Descarga cancelada.");
            } else {
                snprintf(msg, sizeof(msg),
                         "Error de instalación: %s\n\nSi el problema persiste, prueba \"Instalar vía DBI\" (botón X) desde esta misma pantalla.",
                         err_buf);
                ui_app_show_message(msg);
            }
            continue;
        }

        if (install_target->file_type == APP_FILE_TYPE_XCI) {
            XciInstallResult xires = install_xci_native(install_target, base_url,
                                                          install_progress_cb, on_install_phase, &progress_ctx,
                                                          err_buf, sizeof(err_buf));
            if (xires == XCI_INSTALL_OK) {
                snprintf(msg, sizeof(msg), "\"%s\" instalado correctamente.\n\nVuelve al hbmenu para iniciarlo.",
                         install_target->title);
                ui_app_show_message(msg);
            } else if (xires == XCI_INSTALL_ERR_CANCELED) {
                ui_app_show_message("Descarga cancelada.");
            } else {
                snprintf(msg, sizeof(msg),
                         "Error de instalación: %s\n\nSi el problema persiste, prueba \"Instalar vía DBI\" (botón X) desde esta misma pantalla.",
                         err_buf);
                ui_app_show_message(msg);
            }
            continue;
        }

        if (install_target->file_type == APP_FILE_TYPE_PORT) {
            PortInstallResult pres = install_port(install_target, base_url,
                                                    install_progress_cb, on_install_phase, &progress_ctx,
                                                    err_buf, sizeof(err_buf));
            if (pres == PORT_INSTALL_OK) {
                snprintf(msg, sizeof(msg), "\"%s\" instalado correctamente.\n\nVuelve al hbmenu para iniciarlo.",
                         install_target->title);
                ui_app_show_message(msg);
            } else if (pres == PORT_INSTALL_ERR_CANCELED) {
                ui_app_show_message("Descarga cancelada.");
            } else {
                snprintf(msg, sizeof(msg), "Error de instalación: %s", err_buf);
                ui_app_show_message(msg);
            }
            continue;
        }

        InstallResult ires = install_app(install_target, base_url,
                                          install_progress_cb, &progress_ctx, err_buf, sizeof(err_buf));

        if (ires == INSTALL_OK) {
            snprintf(msg, sizeof(msg), "\"%s\" instalado correctamente.\n\nVuelve al hbmenu para iniciarlo.",
                     install_target->title);
            ui_app_show_message(msg);
        } else if (ires == INSTALL_ERR_CANCELED) {
            ui_app_show_message("Descarga cancelada.");
        } else {
            snprintf(msg, sizeof(msg), "Error de instalación: %s", err_buf);
            ui_app_show_message(msg);
        }
    }

    catalog_free(entries);
    free(root_entries);
    ui_icons_clear();
    curl_global_cleanup();
    socketExit();
    // Restore normal auto-sleep behavior before handing control back to
    // hbmenu (or chain-loading into DBI/a self-update, which each get a
    // fresh applet session anyway, but this is cheap and leaves nothing to
    // chance either way).
    appletSetAutoSleepDisabled(false);
    ui_app_shutdown();
    return 0;
}
