#include "install_torrent.h"
#include "install_local.h"
#include "../torrent/magnet_resolver.h"
#include "../torrent/metainfo.h"
#include "../torrent/torrent.h"
#include "../torrent/torrent_storage.h"
#include "../torrent/torrent_log.h"
#include "../torrent/torrent_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TORRENT_WORK_DIR "sdmc:/switch/freeshop/torrents"

static TorrentInstallStats g_stats;

TorrentInstallStats install_torrent_last_stats(void) {
    return g_stats;
}

static bool has_ext_ci(const char *name, const char *ext) {
    size_t nlen = strlen(name), elen = strlen(ext);
    if (elen > nlen) return false;
    const char *tail = name + (nlen - elen);
    for (size_t i = 0; i < elen; i++)
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)ext[i]))
            return false;
    return true;
}

// Maximum number of installable files (base game + DLC) handled from one
// torrent. Comfortably above any real release - even a 55-DLC bundle like
// the one that motivated this - without the unbounded allocation a torrent
// with a hostile file count could otherwise force.
#define MAX_INSTALLABLE_FILES 512

// Collects every file in the torrent matching entry->file_type's extension -
// not just the largest one. Torrent releases commonly bundle a handful of
// small extras (readme, cover art, NFO) alongside the installable
// containers, which is filtered out here the same way it always was, but a
// "base game + N DLC" release packs the DLC as their own separate
// NSP/NSZ/XCI/XCZ files too - keeping only the single largest one silently
// dropped every DLC. Returns indices sorted by torrent offset, so the
// caller installs (and the swarm fills) them in the same front-to-back
// order.
static int collect_installable_files(const metainfo_t *mi, AppFileType file_type,
                                      int *out_indices, int max_out) {
    const char *want_ext = file_type == APP_FILE_TYPE_XCI ? ".xci" : ".nsp";
    // NSZ/XCZ share their parent format's install path (ncm_install.c
    // detects .ncz content by CNMT entry, not by the container's own
    // extension), so also accept that suffix directly.
    const char *want_ext2 = file_type == APP_FILE_TYPE_XCI ? ".xcz" : ".nsz";
    int count = 0;
    for (uint32_t i = 0; i < mi->num_files && count < max_out; i++) {
        const mi_file_t *mf = &mi->files[i];
        if (!has_ext_ci(mf->path, want_ext) && !has_ext_ci(mf->path, want_ext2)) continue;
        out_indices[count++] = (int)i;
    }
    // Insertion sort by torrent offset - count is realistically a couple
    // dozen at most, so O(n^2) beats pulling in qsort's comparator ceremony.
    for (int i = 1; i < count; i++) {
        int key = out_indices[i];
        int64_t key_off = mi->files[key].offset;
        int j = i;
        while (j > 0 && mi->files[out_indices[j - 1]].offset > key_off) {
            out_indices[j] = out_indices[j - 1];
            j--;
        }
        out_indices[j] = key;
    }
    return count;
}

// Aggregates progress across every file being installed from one torrent
// (base game + any DLC - see collect_installable_files) into a single
// running total, the same way install_common.c's install_agg_progress_cb
// aggregates one NSP's CNMT + content pieces into that NSP's own total.
// Both TorrentGate (download-phase progress) and
// install_nsp/xci_from_local_file_ex (install-phase progress) report
// file-scoped (0..this file's own length) numbers through this, so the UI
// sees one consistently growing byte count for the whole selection instead
// of the total resetting to a single file's size at every file boundary.
typedef struct {
    InstallProgressCallback cb;
    void *userdata;
    uint64_t done_before; // bytes accounted for by fully-finished earlier files
    uint64_t grand_total; // sum of every installable file's length
} MultiFileProgressCtx;

static bool multi_file_progress_cb(long total, long now, void *userdata) {
    MultiFileProgressCtx *m = (MultiFileProgressCtx *)userdata;
    if (!m->cb) return true;
    if (m->grand_total == 0) return m->cb(total, now, m->userdata);
    uint64_t overall = m->done_before + (now > 0 ? (uint64_t)now : 0);
    if (overall > m->grand_total) overall = m->grand_total;
    return m->cb((long)m->grand_total, (long)overall, m->userdata);
}

// Drives the download from inside the installer: whenever the installer
// needs bytes the swarm has not delivered yet, this ticks the torrent
// until they land. That inversion - install in charge, download pumped
// on demand - is what makes the two overlap instead of running one after
// the other, without needing the strictly-sequential delivery
// install_stream.c would (torrent pieces only complete *roughly* in
// order; see install_local.h's InstallLocalGate).
typedef struct {
    torrent_t *t;
    uint64_t file_start; // current file's offset in the torrent's flat space
    uint64_t file_len;   // current file's length - for file-scoped progress
    InstallProgressCallback cb; // typically multi_file_progress_cb
    void *userdata;              // typically a MultiFileProgressCtx*
    InstallPhaseCallback phase_cb;
    bool download_failed;
} TorrentGate;

// How long a gate call that is NOT blocked still spends servicing the
// swarm. torrent_tick's own poll() has a 10ms timeout, so an idle swarm
// self-limits to ~2 ticks here while a busy one (poll returns immediately
// when data is waiting) gets as many drain passes as fit.
#define GATE_PUMP_BUDGET_MS 20

// Services the swarm while the installer is doing its own work - reading
// the container off the SD, decompressing NCZ, writing to NCM.
//
// Without this the torrent is only ever ticked while the gate is *blocked*
// waiting for bytes, which means every stretch of install work is a stretch
// of completely idle network. That costs far more than the idle time
// itself: the per-peer request pipeline drains while nobody refills it, so
// when ticking resumes there is a full round-trip of dead air before data
// flows again, and peers that see no requests for long enough choke or drop
// us outright. Pumping here is what actually makes download and install
// concurrent rather than merely interleaved.
//
// Safe with respect to the Switch's "a file open for writing can't be
// opened for reading" rule: install_local.c's gate_ensure has already
// closed its read handle before calling into here (see its comment), so
// the downloader is free to take the handle back.
static void torrent_gate_pump(TorrentGate *g) {
    uint64_t start = now_ms();
    do {
        int r = torrent_tick(g->t);
        if (r <= 0) break; // finished, or failed - the next blocking wait reports it

        torrent_stat_t stat;
        torrent_stat(g->t, &stat);
        g_stats.resolving = false;
        g_stats.peers = stat.num_peers;
        g_stats.active_peers = stat.num_active_peers;
        g_stats.dht_good = stat.dht_good;
        g_stats.dht_dubious = stat.dht_dubious;
        g_stats.speed_bps = stat.speed_bps;
    } while (now_ms() - start < GATE_PUMP_BUDGET_MS);
}

static bool torrent_gate_ensure(void *user, uint64_t offset, uint64_t len) {
    TorrentGate *g = (TorrentGate *)user;
    uint64_t abs_offset = g->file_start + offset;

    if (torrent_range_complete(g->t, abs_offset, len)) {
        torrent_gate_pump(g);
        // torrent_gate_pump can write newly-arrived pieces (torrent_tick ->
        // cb_block -> storage_write), which reopens a DISK file's write
        // handle if storage_commit had closed it (see
        // torrent_storage.c's open_disk_file). Left open, that write handle
        // is exactly what install_local.c's gate_ensure() then races against
        // - it fopen(path, "rb")s the same file right after this call
        // returns, and the Switch's filesystem refuses to open a file for
        // reading while it's still open for writing elsewhere, silently
        // failing that fopen. gate_ensure has no way to tell that apart from
        // a real cancel, so this surfaced as "Descarga cancelada" on an
        // install that was actually still fine - and only once the download
        // was already racing ahead of the installer (this fast path is only
        // taken once a range is already complete), which is why it took a
        // few pieces to start happening, not every gate call. The slow path
        // below already flushes via torrent_flush before returning true;
        // this mirrors that so both paths hand back a file the installer
        // can actually reopen.
        torrent_flush(g->t);
        return true;
    }

    // Tell the swarm what we are actually blocked on. Without this the
    // picker just walks the file front-to-back, so an NSP whose .cnmt.nca
    // sits in its last few KB (common - the packer decides) makes the
    // installer wait out the entire download before it can even read the
    // content list.
    torrent_prioritize_range(g->t, abs_offset, len);

    // Waiting on the swarm now, not writing to NCM - say so, otherwise the
    // screen claims "Instalando" through what can be a long stall.
    if (g->phase_cb) g->phase_cb(INSTALL_PHASE_DOWNLOADING, g->userdata);

    while (!torrent_range_complete(g->t, abs_offset, len)) {
        int r = torrent_tick(g->t);
        if (r < 0) {
            g->download_failed = true;
            return false;
        }
        if (r == 0) break; // whole torrent finished; range must be in by now

        torrent_stat_t stat;
        torrent_stat(g->t, &stat);
        g_stats.resolving = false;
        g_stats.peers = stat.num_peers;
        g_stats.active_peers = stat.num_active_peers;
        g_stats.dht_good = stat.dht_good;
        g_stats.dht_dubious = stat.dht_dubious;
        g_stats.speed_bps = stat.speed_bps;

        // Scoped to the file being installed right now, not the whole
        // torrent - install-phase progress (install_agg_progress_cb, via
        // multi_file_progress_cb) is file-scoped too, and both must agree
        // on what "total" means or the aggregated number swings wildly
        // every time the source switches (this used to be torrent-wide
        // completed_bytes, which raced ahead of the file actually being
        // installed since peers pipeline requests past the gated range).
        long total = g->file_len > 0 ? (long)g->file_len : 1;
        long now = (long)torrent_range_downloaded_bytes(g->t, g->file_start, g->file_len);
        if (g->cb && !g->cb(total, now, g->userdata)) return false; // user canceled

        // Deliberately no sleep here: torrent_tick's own poll() already
        // blocks up to 10ms when nothing is ready, which is what paces this
        // loop on an idle swarm. When peers *are* sending, poll returns
        // immediately - and sleeping then just leaves the sockets
        // undrained and the request pipelines unfilled for 10ms out of
        // every ~10ms, roughly halving achievable download speed.
    }

    // The pieces are accounted for; make sure the writer's buffered bytes
    // are actually on disk before the installer's separate read handle
    // goes looking for them.
    torrent_flush(g->t);
    torrent_prioritize_range(g->t, 0, 0); // no longer blocked - back to normal order

    bool ok = torrent_range_complete(g->t, abs_offset, len);
    torrent_debug_log("[install] gate file_off=%llu len=%llu abs=%llu -> %s",
                      (unsigned long long)offset, (unsigned long long)len,
                      (unsigned long long)abs_offset, ok ? "ready" : "INCOMPLETE");

    if (g->phase_cb) g->phase_cb(INSTALL_PHASE_INSTALLING, g->userdata);
    return ok;
}

// Shared by install_torrent_preview() and install_torrent_impl() below:
// resolves entry->download_url's magnet down to a loaded metainfo_t, the
// step both need before they diverge (one lists files and stops, the other
// goes on to actually download/install). `torrent_path` is left on disk
// pointing at the resolved .torrent on TORRENT_MAGNET_OK - the caller owns
// cleaning it (and freeing `out_mi`) up from there; every other outcome
// cleans up after itself.
typedef enum {
    TORRENT_MAGNET_OK,
    TORRENT_MAGNET_CANCELED,
    TORRENT_MAGNET_ERR,
} TorrentMagnetResult;

static TorrentMagnetResult resolve_magnet_and_load(const AppEntry *entry,
                                                    InstallProgressCallback cb, void *userdata,
                                                    char *torrent_path, size_t torrent_path_size,
                                                    metainfo_t *out_mi,
                                                    char *err_buf, size_t err_buf_size) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/freeshop", 0777);
    mkdir(TORRENT_WORK_DIR, 0777);

    snprintf(torrent_path, torrent_path_size, "%s/%s.torrent", TORRENT_WORK_DIR, entry->id);

    magnet_resolve_t *resolve = magnet_resolve_start(entry->download_url, torrent_path);
    if (!resolve) {
        snprintf(err_buf, err_buf_size, "No se pudo iniciar la resolucion del magnet.");
        return TORRENT_MAGNET_ERR;
    }
    while (!magnet_resolve_done(resolve)) {
        magnet_progress_t p;
        magnet_resolve_progress(resolve, &p);
        long total = p.total_pieces ? (long)p.total_pieces : 1;
        long now = (long)p.completed_pieces;
        g_stats.resolving = true;
        g_stats.resolve_peer_index = p.peer_index;
        g_stats.resolve_peer_count = p.peer_count;
        if (cb && !cb(total, now, userdata)) {
            magnet_resolve_cancel(resolve);
            magnet_resolve_free(resolve);
            snprintf(err_buf, err_buf_size, "Cancelado.");
            return TORRENT_MAGNET_CANCELED;
        }
        usleep(50000); // 50ms - this thread only polls, no work of its own to do
    }
    int resolve_ok = magnet_resolve_ok(resolve);
    char resolve_err[MAGNET_ERROR_MAX];
    snprintf(resolve_err, sizeof(resolve_err), "%s", magnet_resolve_error(resolve));
    magnet_resolve_free(resolve);
    if (!resolve_ok) {
        remove(torrent_path);
        snprintf(err_buf, err_buf_size, "%s", resolve_err);
        return TORRENT_MAGNET_ERR;
    }

    memset(out_mi, 0, sizeof(*out_mi));
    if (!metainfo_load(torrent_path, out_mi)) {
        remove(torrent_path);
        snprintf(err_buf, err_buf_size, "El .torrent resuelto no se pudo leer.");
        return TORRENT_MAGNET_ERR;
    }
    return TORRENT_MAGNET_OK;
}

static TorrentPreviewResult install_torrent_preview_impl(const AppEntry *entry,
                                                          TorrentFileEntry *out_files, int max_files, int *out_count,
                                                          InstallProgressCallback cb, void *userdata,
                                                          char *err_buf, size_t err_buf_size) {
    *out_count = 0;

    char torrent_path[600];
    metainfo_t mi;
    TorrentMagnetResult mr = resolve_magnet_and_load(entry, cb, userdata, torrent_path, sizeof(torrent_path),
                                                     &mi, err_buf, err_buf_size);
    if (mr != TORRENT_MAGNET_OK)
        return mr == TORRENT_MAGNET_CANCELED ? TORRENT_PREVIEW_ERR_CANCELED : TORRENT_PREVIEW_ERR;

    int indices[MAX_INSTALLABLE_FILES];
    int num_files = collect_installable_files(&mi, entry->file_type, indices, MAX_INSTALLABLE_FILES);
    if (num_files <= 0) {
        metainfo_free(&mi);
        remove(torrent_path);
        snprintf(err_buf, err_buf_size, "El torrent no contiene un archivo %s instalable.",
                 entry->file_type == APP_FILE_TYPE_XCI ? "XCI" : "NSP/NSZ");
        return TORRENT_PREVIEW_ERR;
    }

    int n = num_files < max_files ? num_files : max_files;
    for (int k = 0; k < n; k++) {
        int idx = indices[k];
        const char *path = mi.files[idx].path;
        // Filename only - the container path's directory part (release
        // group folder names, "55 DLC/" subfolders) is noise on a screen
        // this narrow; the file's own name is what actually distinguishes
        // one row from the next.
        const char *slash = strrchr(path, '/');
        snprintf(out_files[k].name, sizeof(out_files[k].name), "%s", slash ? slash + 1 : path);
        out_files[k].size = (int64_t)mi.files[idx].length;
        out_files[k].file_index = idx;
    }
    *out_count = n;

    metainfo_free(&mi);
    remove(torrent_path); // just a preview - nothing downloaded yet to keep
    return TORRENT_PREVIEW_OK;
}

TorrentPreviewResult install_torrent_preview(const AppEntry *entry,
                                             TorrentFileEntry *out_files, int max_files, int *out_count,
                                             InstallProgressCallback cb, void *userdata,
                                             char *err_buf, size_t err_buf_size) {
    // Same g_stats lifecycle as install_torrent() below, so
    // install_torrent_last_stats() (what main.c's progress callback reads
    // for the "Resolviendo magnet..." detail line) reports this call's own
    // resolve progress instead of whatever a previous install left behind.
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.active = true;
    g_stats.resolving = true;
    TorrentPreviewResult r = install_torrent_preview_impl(entry, out_files, max_files, out_count,
                                                           cb, userdata, err_buf, err_buf_size);
    memset(&g_stats, 0, sizeof(g_stats));
    return r;
}

static TorrentInstallResult install_torrent_impl(const AppEntry *entry,
                                                  const int *selected_file_indices, int selected_count,
                                                  InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                                  void *userdata, char *err_buf, size_t err_buf_size) {
    if (phase_cb) phase_cb(INSTALL_PHASE_DOWNLOADING, userdata);

    char torrent_path[600];
    metainfo_t mi;
    TorrentMagnetResult mr = resolve_magnet_and_load(entry, cb, userdata, torrent_path, sizeof(torrent_path),
                                                     &mi, err_buf, err_buf_size);
    if (mr != TORRENT_MAGNET_OK)
        return mr == TORRENT_MAGNET_CANCELED ? TORRENT_INSTALL_ERR_CANCELED : TORRENT_INSTALL_ERR;

    int auto_indices[MAX_INSTALLABLE_FILES];
    const int *indices;
    int num_files;
    if (selected_file_indices && selected_count > 0) {
        // A user-made selection (see install_torrent_preview() /
        // ui_torrent_select.h) - install exactly these files, nothing else
        // in the torrent is ever touched. Indices come from a screen shown
        // after a PREVIOUS, separate magnet resolution - re-validated
        // against this resolution's actual file count rather than trusted
        // outright, since it's user-selection state that outlived a screen
        // transition (a stale/tampered index would otherwise index
        // mi.files[]/configs[] out of bounds below).
        for (int i = 0; i < selected_count; i++) {
            if (selected_file_indices[i] < 0 || (uint32_t)selected_file_indices[i] >= mi.num_files) {
                metainfo_free(&mi);
                remove(torrent_path);
                snprintf(err_buf, err_buf_size, "Seleccion de archivos invalida.");
                return TORRENT_INSTALL_ERR;
            }
        }
        indices = selected_file_indices;
        num_files = selected_count;
    } else {
        num_files = collect_installable_files(&mi, entry->file_type, auto_indices, MAX_INSTALLABLE_FILES);
        indices = auto_indices;
    }
    if (num_files <= 0) {
        metainfo_free(&mi);
        remove(torrent_path);
        snprintf(err_buf, err_buf_size, "El torrent no contiene un archivo %s instalable.",
                 entry->file_type == APP_FILE_TYPE_XCI ? "XCI" : "NSP/NSZ");
        return TORRENT_INSTALL_ERR;
    }

    char outdir[512];
    snprintf(outdir, sizeof(outdir), "%s/%s", TORRENT_WORK_DIR, entry->id);

    uint64_t grand_total = 0;
    for (int k = 0; k < num_files; k++)
        grand_total += (uint64_t)mi.files[indices[k]].length;

    g_stats.total_files = (uint32_t)num_files;
    MultiFileProgressCtx multi_ctx = {
        .cb = cb, .userdata = userdata, .done_before = 0, .grand_total = grand_total,
    };

    TorrentInstallResult overall_result = TORRENT_INSTALL_OK;

    // One torrent session per installable file, strictly one at a time -
    // NOT one session with every file marked DISK. storage_open_ex opens
    // and preallocates every DISK file up front (see
    // torrent_storage.c's open_disk_file), so marking all of a "base game
    // + 55 DLC" release DISK meant opening 56 files at once and reserving
    // the full combined size on the SD before a single byte was
    // downloaded - which crashed on hardware. Doing them one at a time
    // keeps each session the exact shape that already works for a
    // single-file release: one open handle, one file's worth of SD space,
    // one preallocation. The cost is re-finding peers per file (a fresh
    // tracker announce and DHT bootstrap each time); the swarm is the same
    // info-hash, so this is slower to ramp up, not a different download.
    for (int k = 0; k < num_files && overall_result == TORRENT_INSTALL_OK; k++) {
        int file_index = indices[k];
        g_stats.current_file_index = (uint32_t)(k + 1);

        storage_file_config_t *configs =
            (storage_file_config_t*)calloc(mi.num_files, sizeof(storage_file_config_t));
        if (!configs) {
            snprintf(err_buf, err_buf_size, "Memoria insuficiente.");
            overall_result = TORRENT_INSTALL_ERR;
            break;
        }
        for (uint32_t i = 0; i < mi.num_files; i++)
            configs[i].mode = STORAGE_FILE_SKIP;
        configs[file_index].mode = STORAGE_FILE_DISK;

        torrent_options_t options;
        memset(&options, 0, sizeof(options));
        options.files = configs;
        options.strict_piece_order = 1; // download roughly front-to-back
        options.request_pipeline_limit = 64;
        // Every install here starts a brand-new torrent_create_ex call
        // against a just-created output file - never a resumed one - so the
        // startup scan can skip its read+hash entirely (see torrent.h's
        // fresh_download).
        options.fresh_download = 1;

        // listen_port is only ever announced to peers/DHT (BEP-10 "p", DHT
        // port field) - this client never binds a listener and only dials
        // out (see torrent.c: no accept path, no seeding), so its exact
        // value doesn't affect anything except what a peer's failed
        // connect-back attempt targets.
        torrent_t *t = torrent_create_ex(&mi, 6969, outdir, &options);
        free(configs);
        if (!t) {
            snprintf(err_buf, err_buf_size, "No se pudo iniciar la descarga por torrent.");
            overall_result = TORRENT_INSTALL_ERR;
            break;
        }

        // Resolve the REAL path storage_open_ex chose for this file - it
        // may not be the "natural" mi->name/mf->path layout: RuTracker
        // release names routinely exceed FAT path-length limits, and
        // storage_open_ex falls back to a shorter, sanitized path when that
        // happens (see torrent_storage.c's build_fallback_path).
        char local_path[1200];
        if (!torrent_file_path(t, (uint32_t)file_index, local_path, sizeof(local_path))) {
            torrent_destroy(t);
            snprintf(err_buf, err_buf_size, "No se pudo ubicar el archivo descargado.");
            overall_result = TORRENT_INSTALL_ERR;
            break;
        }

        torrent_debug_log("[install] file[%d] (%d/%d) '%s' off=%lld len=%lld | torrent: %u files, "
                          "%u pieces of %lld, total=%lld | path='%s'",
                          file_index, k + 1, num_files, mi.files[file_index].path,
                          (long long)mi.files[file_index].offset,
                          (long long)mi.files[file_index].length,
                          mi.num_files, mi.num_pieces, (long long)mi.piece_length,
                          (long long)mi.total_length, local_path);

        // Install and download run together from here: the installer walks
        // the container in ascending file order and blocks in
        // torrent_gate_ensure whenever it reaches bytes the swarm has not
        // delivered yet, which is where torrent_tick actually gets driven
        // from. See install_local.h's InstallLocalGate.
        TorrentGate gate_ctx = {
            .t = t,
            .file_start = (uint64_t)mi.files[file_index].offset,
            .file_len = (uint64_t)mi.files[file_index].length,
            .cb = multi_file_progress_cb,
            .userdata = &multi_ctx,
            .phase_cb = phase_cb,
        };
        InstallLocalGate gate = { .ensure_range = torrent_gate_ensure, .user = &gate_ctx };

        InstallLocalResult lr = entry->file_type == APP_FILE_TYPE_XCI
            ? install_xci_from_local_file_ex(local_path, &gate, multi_file_progress_cb, phase_cb, &multi_ctx, err_buf, err_buf_size)
            : install_nsp_from_local_file_ex(local_path, &gate, multi_file_progress_cb, phase_cb, &multi_ctx, err_buf, err_buf_size);

        // A download failure surfaces as an install failure (the gate gave
        // up); report the torrent's own reason, which is the useful one.
        // install_local.c's gate_ensure()==false path always labels this
        // INSTALL_LOCAL_ERR_CANCELED - it has no way to tell "the user held
        // B" apart from "the torrent gave up for a real reason" (a fatal
        // piece/storage error - see piece.c's got_block, or peer/DHT/tracker
        // exhaustion), since both reach it as the exact same "gate refused"
        // signal. gate_ctx.download_failed is that distinction (only set on
        // torrent.c's own r<0 path, never on the user-canceled one - see
        // torrent_gate_ensure), so reclassify here: a real torrent failure
        // must not go on to read as "Descarga cancelada." to the user, which
        // reported an install as self-canceling when it had actually failed.
        if (lr != INSTALL_LOCAL_OK && gate_ctx.download_failed) {
            const char *terr = torrent_last_error(t);
            snprintf(err_buf, err_buf_size, "%s", terr[0] ? terr : "La descarga por torrent fallo.");
            if (lr == INSTALL_LOCAL_ERR_CANCELED) lr = INSTALL_LOCAL_ERR_NCM;
        }

        torrent_destroy(t);

        // On failure, record what the container's first bytes actually look
        // like. Runs after torrent_destroy so the downloader's own handle is
        // definitely gone - otherwise this probe hits the same "already open
        // for writing" refusal it is meant to be diagnosing.
        if (lr != INSTALL_LOCAL_OK) {
            FILE *probe = fopen(local_path, "rb");
            if (probe) {
                unsigned char head[16];
                size_t got = fread(head, 1, sizeof(head), probe);
                fclose(probe);
                char hex[3 * sizeof(head) + 1];
                for (size_t i = 0; i < got; i++) snprintf(hex + i * 3, 4, "%02x ", head[i]);
                hex[got ? got * 3 - 1 : 0] = '\0';
                torrent_debug_log("[install] failed (%d) on file %d/%d; first %u bytes of '%s': %s",
                                  (int)lr, k + 1, num_files, (unsigned)got, local_path, hex);
            } else {
                torrent_debug_log("[install] failed (%d) on file %d/%d; could not reopen '%s'",
                                  (int)lr, k + 1, num_files, local_path);
            }
        }

        // Deleted as soon as this file is done, not at the very end: the
        // payload is not meant to be kept (this client doesn't seed), and
        // freeing it now is what keeps peak SD usage at one file's size
        // instead of the whole release's.
        remove(local_path);

        if (lr != INSTALL_LOCAL_OK) {
            overall_result = (lr == INSTALL_LOCAL_ERR_CANCELED)
                ? TORRENT_INSTALL_ERR_CANCELED : TORRENT_INSTALL_ERR;
            break;
        }

        multi_ctx.done_before += (uint64_t)mi.files[file_index].length;
    }

    metainfo_free(&mi);
    remove(torrent_path);
    remove(outdir); // best-effort; only succeeds once every payload above is gone

    return overall_result;
}

TorrentInstallResult install_torrent(const AppEntry *entry,
                                     const int *selected_file_indices, int selected_count,
                                     InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                     void *userdata, char *err_buf, size_t err_buf_size) {
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.active = true;
    g_stats.resolving = true;
    TorrentInstallResult r = install_torrent_impl(entry, selected_file_indices, selected_count,
                                                  cb, phase_cb, userdata, err_buf, err_buf_size);
    memset(&g_stats, 0, sizeof(g_stats));
    return r;
}
