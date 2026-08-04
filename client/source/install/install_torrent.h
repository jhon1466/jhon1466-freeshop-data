#pragma once
#include "install.h"
#include "../catalog/app_entry.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TORRENT_INSTALL_OK = 0,
    TORRENT_INSTALL_ERR_CANCELED,
    TORRENT_INSTALL_ERR,
} TorrentInstallResult;

// Snapshot of whichever install_torrent() call is currently on this
// thread's stack (there is only ever one - installs run strictly serially,
// see install_common.h's scratch-buffer comment). Updated right before
// every InstallProgressCallback invocation, so it's always current as of
// the most recent progress callback; zeroed before/after install_torrent()
// runs. Meant to be read from inside that same callback (see main.c's
// install_progress_cb) to show peer/DHT/speed detail a generic byte
// progress bar can't - install_torrent() itself never renders anything.
typedef struct {
    bool active;    // an install_torrent() call is in progress
    bool resolving; // still resolving the magnet - peers/speed below are stale
    uint32_t resolve_peer_index;
    uint32_t resolve_peer_count;
    uint32_t peers;
    uint32_t active_peers;
    uint32_t dht_good;
    uint32_t dht_dubious;
    uint64_t speed_bps;
    // Which file of a multi-file install (base game + any DLC bundled in
    // the same torrent - see install_torrent.c's collect_installable_files)
    // is downloading/installing right now. current_file_index is 1-based;
    // total_files is 1 for a plain single-file release, so callers that
    // don't care can just ignore both.
    uint32_t current_file_index;
    uint32_t total_files;
} TorrentInstallStats;

TorrentInstallStats install_torrent_last_stats(void);

// Installs a torrent-catalog entry (entry->via_torrent, entry->download_url
// a magnet: URI - see sources.h's SOURCE_KIND_TORRENT_CATALOG and
// catalog.c's catalog_fetch_torrent_json):
//   1. Resolve the magnet to a .torrent via client/source/torrent/magnet_resolver.h.
//   2. Find every file matching entry->file_type - a "base game + N DLC"
//      release bundles each as its own NSP/NSZ/XCI/XCZ inside one torrent,
//      so all of them are installed, not just the largest.
//   3. For each of those files in turn, run a separate torrent session
//      (client/source/torrent/torrent.h) that downloads only that one file
//      in strict piece order (so the container fills front-to-back) and
//      skips everything else, installing it *while it downloads* via
//      install_local.h's gated entry points: the installer walks the
//      container in ascending file order and blocks only when it reaches
//      bytes the swarm has not delivered yet, which is also what drives
//      torrent_tick. So the NCM writes for early NCAs overlap the download
//      of later ones instead of waiting for the whole file.
//
//      One session per file rather than one session with every file
//      selected: storage_open_ex opens and preallocates every DISK-mode
//      file up front, so selecting all of them at once meant dozens of
//      simultaneously open handles and the release's full combined size
//      reserved on the SD before anything downloaded. Sequential sessions
//      keep peak usage at a single file. Progress across files is
//      aggregated into one running total (see install_torrent.c's
//      MultiFileProgressCtx) so the UI shows one consistent byte count for
//      the whole selection instead of resetting at each file boundary.
//
// Not built on install_stream.h (the MTP/FTP push path) even though that
// one also installs while receiving: it requires strictly sequential
// delivery, and BitTorrent piece completion is only roughly ordered - even
// in strict order, pieces within the lookahead window finish out of order.
// Reading from the partially-written file on disk sidesteps that.
//
// Each downloaded payload is removed as soon as its own file finishes (see
// step 3 - that is what bounds peak SD usage), and the working directory
// once the whole install ends, success or failure - this client only
// leeches, never seeds, so nothing is meant to be kept around afterward.
TorrentInstallResult install_torrent(const AppEntry *entry,
                                     InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                     void *userdata, char *err_buf, size_t err_buf_size);
