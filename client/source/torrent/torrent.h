#pragma once
// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/core/torrent.c/.h, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
//
// The orchestrator: ties metainfo/tracker/peer/piece/dht_engine together
// into one poll-driven session, matching the mtp_step()/ftp_step() shape
// used everywhere else in this client. torrent_tick() does one pass -
// service peer sockets, refill request pipelines, drain a finished tracker
// announce, drain DHT-found peers - and returns without blocking (aside
// from a short poll() timeout), so the caller drives it once per frame.
//
// Trimmed from the original: no μTP (see peer.h's doc comment - this
// responder is TCP-only), no async piece-hash worker (see piece.h), no
// app-specific telemetry framework.
#include "metainfo.h"
#include "piece.h"
#include "dht_engine.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct torrent torrent_t;

typedef struct {
    const storage_file_config_t *files; // metainfo.num_files entries
    int strict_piece_order;
    const uint32_t *piece_order;
    uint32_t piece_order_count;
    int (*request_allowed)(void *user, uint32_t piece);
    void *request_allowed_user;
    uint32_t strict_order_lookahead; // 0 = default
    uint32_t request_pipeline_limit; // per peer, 0 = MAX_PIPELINE
    uint32_t hedge_after_ms; // duplicate critical requests after this age
    int strict_fill_pending_first;
    // Fast resume: have-bitfield saved by torrent_copy_have_bitfield at a
    // previous orderly teardown. Used only when have_bitfield_len equals
    // (num_pieces+7)/8; copied during create, need not outlive the call.
    // Skips the startup hash scan entirely; the final verification pass at
    // completion still re-hashes everything, so a wrong bitfield self-heals
    // at the cost of serving unverified pieces until then.
    const uint8_t *have_bitfield;
    uint32_t have_bitfield_len;
    // Skips reading+hashing every piece of the wanted (DISK-mode) file(s)
    // at startup - see piece.h's piece_mgr_t.skip_disk_verify. Only safe
    // when the caller knows for certain nothing has been downloaded there
    // yet (a torrent that always starts a fresh download and never
    // resumes - see install_torrent.c); ignored when have_bitfield is set,
    // since fast-resume already skips this scan for a different reason
    // (trusting a saved bitfield instead of not needing to check at all).
    int fresh_download;
} torrent_options_t;

typedef struct {
    uint32_t num_pieces_done;
    uint32_t num_pieces;
    uint32_t num_peers;        // occupied peer slots, incl. connecting
    uint32_t num_active_peers; // peers past handshake (PS_ACTIVE)
    uint32_t dht_good;
    uint32_t dht_dubious;
    uint64_t downloaded;  // bytes received during this session
    uint64_t completed_bytes;
    uint64_t total_bytes;
    // Bytes never wanted: pieces over SKIP files (and the consumed prefix of
    // resumed SINK files) are pre-marked done at startup, so completed_bytes
    // already includes them. The UI progress denominators must subtract
    // skipped_bytes or a partial selection shows a pre-inflated percentage.
    uint64_t skipped_bytes;
    uint64_t speed_bps;   // bytes/sec, updated ~1/sec
    uint64_t last_payload_ms; // monotonic time of last accepted payload
    uint32_t num_pieces_verified;
    int      verifying;   // startup or final verification is in progress
    int      complete;    // 1 when all pieces verified
} torrent_stat_t;

// Creates the torrent engine. Starts network (tracker, DHT) automatically.
// listen_port: port for incoming peers and DHT.
// outdir: where to write downloaded files.
torrent_t *torrent_create(const metainfo_t *mi,
                          uint16_t listen_port,
                          const char *outdir);
torrent_t *torrent_create_ex(const metainfo_t *mi,
                             uint16_t listen_port,
                             const char *outdir,
                             const torrent_options_t *options);
void        torrent_destroy(torrent_t *t);

// Queues compact IPv4 endpoints (4-byte address + 2-byte port, network
// order) before the first tick. Returns the number accepted after
// validation and deduplication.
uint32_t torrent_add_initial_peers(torrent_t *t, const uint8_t *compact,
                                   uint32_t count);

// Runs one tick of the event loop. Returns 0 when download is complete.
int  torrent_tick(torrent_t *t);

// Web-seed (BEP-19) hooks. The application fetches whole pieces over HTTP
// and hands them to the engine, which verifies and stores them exactly
// like peer blocks. Both functions touch the piece manager and MUST be
// called from the same thread as torrent_tick.

// Non-zero if the piece is already downloaded and verified.
int torrent_piece_done(const torrent_t *t, uint32_t piece);

// Submits a whole piece fetched from a web seed. `len` must equal the
// piece's length. Returns 2 if the piece is now complete and verified, 1 if
// stored, 0 if ignored (already done / bad args), <0 on storage error.
int torrent_submit_web_piece(torrent_t *t, uint32_t piece,
                             const uint8_t *data, uint32_t len);

// Snapshots the have-bitfield for fast-resume persistence. Torrent-thread
// only. Returns the required byte count when out is NULL, the bytes copied
// otherwise. Returns 0 while startup verification is still running - the
// bitfield is incomplete then and MUST NOT be persisted. Note the storage
// layer never fsyncs, so a "clean" snapshot still trusts the OS cache to
// reach disk.
uint32_t torrent_copy_have_bitfield(torrent_t *t, uint8_t *out,
                                    uint32_t out_len);

// Fills stats for UI.
void torrent_stat(const torrent_t *t, torrent_stat_t *s);
const char *torrent_last_error(const torrent_t *t);

// True when every piece covering [offset, offset+len) of the torrent's
// flat byte space is downloaded and verified - i.e. those bytes are
// readable from the output file right now. Lets a caller install a
// container while it is still downloading by waiting on the byte ranges
// it is about to read (see install_torrent.c and install_local.h's
// InstallLocalGate).
bool torrent_range_complete(const torrent_t *t, uint64_t offset, uint64_t len);

// Bytes within [offset, offset+len) that are currently verified and stored -
// the byte-accurate, partial-progress counterpart of torrent_range_complete.
// Lets a caller show download progress scoped to one file out of a
// multi-file torrent (see install_torrent.c) instead of the whole swarm's
// total, which is what torrent_stat's completed_bytes measures.
uint64_t torrent_range_downloaded_bytes(const torrent_t *t, uint64_t offset,
                                        uint64_t len);

// Fetches the pieces covering [offset, offset+len) ahead of everything
// else - for a caller blocked waiting on exactly that range (see
// torrent_range_complete and install_local.h's InstallLocalGate). Pass
// len 0 to clear. See piece.h's piece_mgr_set_priority for why this
// matters: without it, where the container's packer happened to put the
// metadata dictates whether the install can overlap the download at all.
void torrent_prioritize_range(torrent_t *t, uint64_t offset, uint64_t len);

// Commits buffered writes for every DISK-backed output file, so bytes
// torrent_range_complete just reported as present are actually visible to
// a separate reader handle on the same file. See torrent_storage.h's
// storage_commit for why this closes/reopens rather than just flushing.
bool torrent_flush(torrent_t *t);

// Resolves the real on-disk path for file `index` (see
// torrent_storage.h's storage_file_path - callers that need to read a
// completed download back MUST go through this, not reconstruct the
// "natural" mi->name/mf->path layout themselves: a long/exotic release
// name can make storage_open_ex fall back to a different, sanitized path).
// Returns 1 on success, 0 if index is out of range or not DISK-backed.
int torrent_file_path(const torrent_t *t, uint32_t index, char *out, size_t out_size);

// Resizes the strict-order lookahead window at runtime. No-op unless the
// torrent runs in strict piece order; lookahead 0 is ignored.
void torrent_set_strict_lookahead(torrent_t *t, uint32_t lookahead);

// Freezes or resumes per-peer download-rate sampling. While frozen, dl_rate
// EMAs hold their last value and intervals are discarded: used when the
// application's install gate curtails new requests, so peers idling
// through no fault of their own keep their pipeline depth.
void torrent_set_rate_freeze(torrent_t *t, int freeze);
