#pragma once
// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/app/magnet_resolver.cpp/.hpp, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
//
// BEP-9 (ut_metadata) magnet-to-torrent resolution: given a magnet URI with
// no piece hashes of its own, dial the swarm (tracker + DHT + PEX), fetch
// the bencoded info dictionary from whichever peer answers first, verify it
// against the magnet's info hash, and synthesize a normal .torrent file on
// disk that metainfo_parse can load. This is what lets a catalog entry that
// only carries a magnet URI (this client's actual catalog data - see
// client/source/catalog/ - never carries a pre-resolved info dict) become
// downloadable through torrent.c at all.
//
// Deliberately NOT built on peer.h/peer.c: this is a short-lived, one-shot,
// blocking-socket operation (dial up to a dozen peers in parallel, race for
// whichever answers first, then it's done) rather than an ongoing
// non-blocking swarm session, so it gets its own small self-contained
// implementation - matching pipensx's own separation between this file and
// torrent.c. The whole resolve runs on one background pthread (like
// dht_engine.h/torrent.c's announce thread): it can take up to
// MAGNET_RESOLVE_TIMEOUT_MS, and DNS/tracker/peer I/O throughout would
// stall the render loop if run inline.
//
// Only supports the trackers this client's actual catalog data uses
// (RuTracker's t-ru.org mirrors) - see magnet_resolver.c's allowedTracker
// equivalent. A magnet with no recognized tracker is rejected at parse
// time rather than silently trying arbitrary hosts.
#include <stdint.h>
#include <stddef.h>

#define MAGNET_ERROR_MAX 256

typedef enum {
    MAGNET_STAGE_FINDING_PEERS,
    MAGNET_STAGE_CONNECTING,
    MAGNET_STAGE_FETCHING_METADATA,
    MAGNET_STAGE_VALIDATING,
} magnet_stage_t;

typedef struct {
    magnet_stage_t stage;
    uint32_t completed_pieces;
    uint32_t total_pieces;
    uint32_t peer_index;
    uint32_t peer_count;
} magnet_progress_t;

typedef struct {
    uint8_t info_hash[20];
    char    info_hash_hex[41];
    char    tracker_url[512];
} magnet_spec_t;

// Parses a magnet: URI. Returns 1 on success. On failure, *error holds a
// human-readable reason (safe to show the user directly).
int magnet_parse(const char *uri, magnet_spec_t *spec,
                 char error[MAGNET_ERROR_MAX]);

typedef struct magnet_resolve magnet_resolve_t;

// Starts resolving `uri` into a synthesized .torrent file at `out_path`, on
// a background thread. Returns NULL only if the thread could not be
// started at all (the caller should treat that as an immediate failure,
// nothing to poll). Every other failure surfaces later through
// magnet_resolve_ok()/magnet_resolve_error() once magnet_resolve_done().
magnet_resolve_t *magnet_resolve_start(const char *uri, const char *out_path);

// Polls for completion. Call every frame; cheap (a mutex-guarded flag
// read). Returns 1 once the resolve has finished (successfully or not).
int magnet_resolve_done(magnet_resolve_t *r);

// Best-effort progress snapshot for a UI progress bar. Only meaningful
// while !magnet_resolve_done().
void magnet_resolve_progress(const magnet_resolve_t *r, magnet_progress_t *out);

// Valid once magnet_resolve_done() is true: 1 if out_path now holds a
// verified .torrent file, 0 otherwise (see magnet_resolve_error()).
int magnet_resolve_ok(const magnet_resolve_t *r);
const char *magnet_resolve_error(const magnet_resolve_t *r);

// Requests cancellation. The resolve stops at its next checkpoint (usually
// within a couple seconds) rather than running out the full timeout;
// magnet_resolve_done() still needs to be polled afterwards.
void magnet_resolve_cancel(magnet_resolve_t *r);

// Joins the background thread and frees `r`. Only call once
// magnet_resolve_done() is true (call magnet_resolve_cancel() first and
// keep polling if you need to abandon a resolve early - this does not
// itself block waiting for the timeout, but it does join the thread, which
// won't have exited yet if the resolve is still mid-flight).
void magnet_resolve_free(magnet_resolve_t *r);
