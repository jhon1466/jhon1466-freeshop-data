// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/core/torrent.c, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
// Trimmed to TCP-only (no libutp), inline piece hashing (no async worker),
// and no app-specific telemetry framework - see torrent.h's doc comment.
#include "torrent.h"
#include "piece.h"
#include "tracker.h"
#include "net.h"
#include "peer.h"
#include "torrent_util.h"
#include "torrent_log.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

#ifdef __SWITCH__
#  include <miniupnpc/miniupnpc.h>
#  include <miniupnpc/upnpcommands.h>
#endif

#define MAX_PEER_QUEUE 1024
#define CONNECT_INTERVAL_MS 50
// A firewalled peer's TCP SYN just hangs, squatting a slot until this
// fires; a reachable peer answers well inside it. Keep it short so the
// burst dialer can cycle through a big peer list quickly.
#define CONNECT_TIMEOUT_MS  3000
// Keep this many sockets mid-connect at once (dial in bursts instead of one
// peer per CONNECT_INTERVAL_MS) so the reachable peers in a large announce
// are found in ~one pass rather than dribbled out over seconds.
#define CONNECT_IN_FLIGHT   16
// Evict an ACTIVE peer that keeps us choked with no piece for this long -
// but only once peer slots are actually scarce (see PEER_CHOKE_EVICT_FLOOR
// and the eviction site).
//
// A peer that has us choked is NOT a broken peer. BitTorrent clients rotate
// their optimistic-unchoke slot on a ~30s cycle, and a pure leecher (this
// client never seeds) sits at the back of every peer's queue, so waiting
// out several rotations while connected is precisely how a slot is earned.
// The old 20s limit was shorter than a single rotation, and it also
// blocklisted the peer for a minute afterwards - so the swarm was churned
// through instead of accumulated. Observed on hardware as a steady
// "active=1 unchoked=1" against 112 announced peers, with the entire
// download running off one socket.
#define PEER_CHOKE_GIVEUP_MS 180000ULL
// Below this many connected peers, a choked one is never evicted: it costs
// a socket and buys a chance at the next unchoke rotation, and there is no
// one waiting for the slot anyway.
#define PEER_CHOKE_EVICT_FLOOR (MAX_PEERS * 3 / 4)
#define TRACKER_REANNOUNCE_MS (30*60*1000ULL)  // 30 min
// When the swarm is peer-starved, don't wait the full interval - re-announce
// early to pull in a fresh peer set. Rate-limited so we never hammer the
// tracker more than once per this window.
//
// One minute, not the 15s this used to be: an announce returns the same peer
// set for a long while, so re-asking four trackers four times a minute
// yields nothing new and gets the client rate-limited. Seen on hardware -
// after several minutes of 15s re-announces, two of the four trackers
// started answering "Couldn't connect to server" while the other two kept
// returning the identical 28 peers.
#define TRACKER_STARVED_REANNOUNCE_MS 60000ULL
#define TRACKER_STARVED_ACTIVE_PEERS  5
#define PEER_TIMEOUT_MS       60000
#define REQUEST_TIMEOUT_MS    15000
#define MAX_ACTIVE_PEERS      MAX_PEERS
#define MIN_REQUEST_PIPELINE  8
// Adaptive per-peer request pipeline: size the in-flight window to roughly
// PIPELINE_TARGET_MS of the peer's measured download rate. Until a peer has
// a rate sample, assume PIPELINE_BOOTSTRAP_BPS so it can ramp.
#define PIPELINE_TARGET_MS     2000ULL
#define PIPELINE_BOOTSTRAP_BPS (1024ULL * 1024ULL)  // 1 MiB/s
// A peer that has never delivered a block gets only a shallow probe window:
// a bootstrap-rate window (128 requests) on a peer that turns out to be dead
// locks that many head-piece blocks for the whole request timeout.
#define BOOTSTRAP_PIPELINE     16
// Requests to a zero-delivery peer expire on a short fuse so hostage blocks
// return to the pool quickly; the full REQUEST_TIMEOUT_MS applies only once
// the peer has proven it delivers.
#define FIRST_BLOCK_TIMEOUT_MS 4000
// A peer whose requests expired but that delivered a block this recently is
// servicing its queue - release the stale blocks without a strike/cooldown,
// otherwise the cooldown starves a productive peer and its rate EMA (and
// with it the adaptive pipeline) collapses.
#define STRIKE_GRACE_MS        2000
#define TIMEOUT_COOLDOWN_BASE_MS 2000
#define TIMEOUT_COOLDOWN_MAX_MS  10000
#define TIMEOUT_DISCONNECT_STRIKES 3
#define TIMEOUT_DISCONNECT_IDLE_MS (REQUEST_TIMEOUT_MS * 2)
#define MAX_HEDGES_PER_TICK   4
#define MAX_HEDGED_BLOCKS     16
#define HEDGE_INTERVAL_MS     250
// Adaptive hedge threshold: duplicate a head-window request once it is
// HEDGE_LATENCY_MULT times older than the median active-peer block latency,
// instead of waiting for the static hedge_after_ms. The static value stays
// as the upper bound; the floor keeps a noisy estimate from spraying
// duplicates. Below HEDGE_MIN_LATENCY_PEERS sampled peers the static value
// is used - with one peer there is nobody to hedge to anyway.
#define HEDGE_LATENCY_MULT      4
#define HEDGE_ADAPTIVE_MIN_MS   500
#define HEDGE_MIN_LATENCY_PEERS 2
#define PEER_BLOCKLIST_SIZE   64
#define PEER_BLOCKLIST_MS     60000
// Startup verification runs in time-budgeted batches per tick so the tick
// still reaches poll(): sockets stay serviced and dials/handshakes proceed
// while pieces are being checked. Request scheduling stays gated until the
// check finishes, so nothing is downloaded that may already be on disk.
#define STARTUP_VERIFY_BUDGET_MS 50

struct peer_addr {
    uint32_t ip;   // network byte order
    uint16_t port; // network byte order
    uint8_t  no_mse;  // skip MSE on this attempt (a plaintext fallback retry)
};

static uint64_t ema_update(uint64_t previous, uint64_t sample) {
    if (sample >= previous)
        return previous + (sample - previous) * 3 / 10;
    return previous - (previous - sample) * 3 / 10;
}

struct torrent {
    metainfo_t   mi;
    piece_mgr_t *pm;
    storage_t   *store;
    dht_session_t *dht;

    uint8_t peer_id[20];

    peer_t  *peers[MAX_ACTIVE_PEERS];
    int      num_peers;

    struct peer_addr queue[MAX_PEER_QUEUE];
    int      qhead, qtail, qsize;

    // O(1) membership test for the queue above (open addressing, tombstone
    // deletion). Sized 2x MAX_PEER_QUEUE so a free slot always exists; a
    // 200-peer announce used to cost ~200k linear-scan comparisons here.
    struct {
        uint32_t ip;
        uint16_t port;
        uint8_t  state; // QH_EMPTY / QH_USED / QH_DEAD
    } qhash[2 * MAX_PEER_QUEUE];
    uint32_t qhash_tombstones;

    struct {
        uint32_t ip;
        uint16_t port;
        uint64_t until_ms;
    } blocklist[PEER_BLOCKLIST_SIZE];
    uint32_t blocklist_next;

    uint16_t listen_port;

    // Stats
    uint64_t downloaded;
    uint64_t speed_bytes; // accumulated since last speed update
    uint64_t speed_bps;
    uint64_t speed_time_ms;
    uint64_t last_payload_ms;
    uint64_t last_health_ms;
    uint32_t expired_requests;
    // Cached DHT routing-table counts. dht_shared_nodes() locks the shared
    // engine's global mutex and walks the whole table under it, while the
    // engine thread holds that same mutex across a full receive burst - so
    // reading it every tick (~100 Hz) parks the peer event loop behind the
    // DHT thread. These two numbers only feed a UI counter; sample them on
    // the 1 Hz speed tick instead.
    uint32_t dht_good;
    uint32_t dht_dubious;

    uint32_t request_pipeline_limit;
    // Requests are curtailed by the application's install gate: peers idle
    // through no fault of their own, so per-peer rate EMAs hold instead of
    // decaying to bootstrap.
    int      rate_freeze;
    uint32_t hedge_after_ms;
    uint32_t hedge_effective_ms; // last adaptive threshold
    uint32_t schedule_cursor;
    uint64_t last_hedge_ms;

    uint64_t last_tracker_ms;
    uint64_t last_connect_ms;

    // Async tracker announce: tracker_announce() blocks up to 5 s per
    // tracker, so it runs on a worker thread instead of freezing the event
    // loop (same justification as dht_engine.h's background thread). The
    // loop launches a run (announce_start), keeps servicing peers, and
    // drains results on a later tick (announce_collect). async_ok is 0
    // only if the mutex failed to init, in which case announces fall back
    // to synchronous.
    pthread_t       announce_thread;
    pthread_mutex_t announce_mutex;
    int             async_ok;
    int             announce_active;  // thread launched, not yet collected
    int             announce_done;    // guarded by announce_mutex
    uint32_t        announce_count;   // guarded by announce_mutex
    uint8_t         announce_compact[200*6];
    int64_t         announce_downloaded;
    int64_t         announce_left;
    // Set by torrent_destroy before it joins the announce thread, read by
    // that thread between trackers and from curl's progress callback.
    atomic_int      announce_stop;
    uint32_t startup_verify_index;
    int      startup_verifying;
    // Sum of piece lengths lying entirely in skipped storage ranges (SKIP
    // files, consumed SINK prefixes). Constant after startup; computed once
    // the startup scan (or fast-resume preset) has run, because the scan is
    // what pre-marks those pieces done.
    uint64_t skipped_bytes;
    // Set when startup verification finished with every piece already valid
    // on disk: the resume path then skips the final verification pass,
    // which would re-read and re-hash the exact same bytes a second time.
    int      startup_verified_all;
    uint32_t final_verify_index;
    int      final_verifying;
    int      fatal_error;
    char     error[256];

#ifdef __SWITCH__
    pthread_t       upnp_thread;
    int             upnp_thread_active;
    struct UPNPUrls upnp_urls;
    struct IGDdatas upnp_data;
    char            upnp_lanaddr[64];
    char            upnp_port_str[16];
    int             upnp_mapped;
#endif
};

static void blocklist_add(torrent_t *t, uint32_t ip, uint16_t port,
                          uint64_t now) {
    uint32_t index = t->blocklist_next++ % PEER_BLOCKLIST_SIZE;
    t->blocklist[index].ip = ip;
    t->blocklist[index].port = port;
    t->blocklist[index].until_ms = now + PEER_BLOCKLIST_MS;
}

static int blocklist_blocked(const torrent_t *t, uint32_t ip, uint16_t port,
                             uint64_t now) {
    for (uint32_t i = 0; i < PEER_BLOCKLIST_SIZE; ++i) {
        if (t->blocklist[i].ip == ip && t->blocklist[i].port == port &&
            t->blocklist[i].until_ms > now)
            return 1;
    }
    return 0;
}

// ---- peer queue ----
#define QUEUE_HASH_CAP (2u * MAX_PEER_QUEUE)
enum { QH_EMPTY = 0, QH_USED = 1, QH_DEAD = 2 };

static uint32_t queue_hash_index(uint32_t ip, uint16_t port) {
    uint32_t h = ip * 2654435761u;
    h ^= (uint32_t)port * 40503u;
    return h & (QUEUE_HASH_CAP - 1);
}

static int queue_hash_contains(const torrent_t *t, uint32_t ip,
                               uint16_t port) {
    uint32_t idx = queue_hash_index(ip, port);
    for (uint32_t probe = 0; probe < QUEUE_HASH_CAP; probe++) {
        uint32_t slot = (idx + probe) & (QUEUE_HASH_CAP - 1);
        if (t->qhash[slot].state == QH_EMPTY)
            return 0;
        if (t->qhash[slot].state == QH_USED &&
            t->qhash[slot].ip == ip && t->qhash[slot].port == port)
            return 1;
    }
    return 0;
}

static void queue_hash_add_raw(torrent_t *t, uint32_t ip, uint16_t port) {
    uint32_t idx = queue_hash_index(ip, port);
    for (uint32_t probe = 0; probe < QUEUE_HASH_CAP; probe++) {
        uint32_t slot = (idx + probe) & (QUEUE_HASH_CAP - 1);
        if (t->qhash[slot].state != QH_USED) {
            if (t->qhash[slot].state == QH_DEAD && t->qhash_tombstones)
                t->qhash_tombstones--;
            t->qhash[slot].state = QH_USED;
            t->qhash[slot].ip    = ip;
            t->qhash[slot].port  = port;
            return;
        }
    }
}

// Compacts away tombstones by rehashing the live queue. Rare: fires only
// after QUEUE_HASH_CAP/4 deletions have accumulated. Keeping used+dead
// below capacity guarantees probes always hit an empty slot and terminate.
static void queue_hash_rebuild(torrent_t *t) {
    memset(t->qhash, 0, sizeof(t->qhash));
    t->qhash_tombstones = 0;
    for (int i = 0; i < t->qsize; i++) {
        const struct peer_addr *a =
            &t->queue[(t->qhead + i) % MAX_PEER_QUEUE];
        queue_hash_add_raw(t, a->ip, a->port);
    }
}

static void queue_hash_add(torrent_t *t, uint32_t ip, uint16_t port) {
    if (t->qhash_tombstones > QUEUE_HASH_CAP / 4)
        queue_hash_rebuild(t);
    queue_hash_add_raw(t, ip, port);
}

static void queue_hash_remove(torrent_t *t, uint32_t ip, uint16_t port) {
    uint32_t idx = queue_hash_index(ip, port);
    for (uint32_t probe = 0; probe < QUEUE_HASH_CAP; probe++) {
        uint32_t slot = (idx + probe) & (QUEUE_HASH_CAP - 1);
        if (t->qhash[slot].state == QH_EMPTY)
            return;
        if (t->qhash[slot].state == QH_USED &&
            t->qhash[slot].ip == ip && t->qhash[slot].port == port) {
            t->qhash[slot].state = QH_DEAD;
            t->qhash_tombstones++;
            return;
        }
    }
}

static int queue_insert(torrent_t *t, uint32_t ip, uint16_t port, int front,
                        int no_mse) {
    uint16_t host_port = ntohs(port);
    if (ip == 0 || ip == INADDR_NONE || host_port < 2)
        return 0;
    uint32_t host_ip = ntohl(ip);
    uint8_t first = (uint8_t)(host_ip >> 24);
    uint8_t second = (uint8_t)(host_ip >> 16);
    if (first == 0 || first == 10 || first == 127 || first >= 224 ||
        (first == 169 && second == 254) ||
        (first == 172 && second >= 16 && second <= 31) ||
        (first == 192 && second == 168))
        return 0;
    if (t->qsize >= MAX_PEER_QUEUE) return 0;
    // Dedup: skip if already queued or connected
    if (queue_hash_contains(t, ip, port))
        return 0;
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        if (t->peers[i] && t->peers[i]->addr.sin_addr.s_addr == ip &&
            t->peers[i]->addr.sin_port == port) return 0;
    }
    if (blocklist_blocked(t, ip, port, now_ms()))
        return 0;
    int index;
    if (front) {
        t->qhead = (t->qhead + MAX_PEER_QUEUE - 1) % MAX_PEER_QUEUE;
        index = t->qhead;
    } else {
        index = t->qtail;
        t->qtail = (t->qtail + 1) % MAX_PEER_QUEUE;
    }
    t->queue[index].ip      = ip;
    t->queue[index].port    = port;
    t->queue[index].no_mse  = (uint8_t)no_mse;
    t->qsize++;
    queue_hash_add(t, ip, port);
    return 1;
}

static int queue_push(torrent_t *t, uint32_t ip, uint16_t port) {
    return queue_insert(t, ip, port, 0, 0);
}

static int queue_pop(torrent_t *t, uint32_t *ip, uint16_t *port,
                     uint8_t *no_mse) {
    if (t->qsize == 0) return 0;
    *ip      = t->queue[t->qhead].ip;
    *port    = t->queue[t->qhead].port;
    *no_mse  = t->queue[t->qhead].no_mse;
    t->qhead = (t->qhead + 1) % MAX_PEER_QUEUE;
    t->qsize--;
    queue_hash_remove(t, *ip, *port);
    return 1;
}

uint32_t torrent_add_initial_peers(torrent_t *t, const uint8_t *compact,
                                   uint32_t count) {
    if (!t || !compact || count == 0)
        return 0;
    uint32_t accepted = 0;
    // Forward walk appends in list order and the queue's hash dedup keeps
    // the earliest occurrence of a repeated endpoint. The verified list
    // must sit ahead of anything already queued, so afterwards the
    // pre-existing entries are rotated to the back (their relative order
    // and dial flags preserved).
    int prior = t->qsize;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t ip;
        uint16_t port;
        const uint8_t *endpoint = compact + i * 6;
        memcpy(&ip, endpoint, sizeof(ip));
        memcpy(&port, endpoint + sizeof(ip), sizeof(port));
        accepted += (uint32_t)queue_push(t, ip, port);
    }
    if (accepted > 0) {
        for (int k = 0; k < prior; ++k) {
            uint32_t ip;
            uint16_t port;
            uint8_t no_mse;
            if (!queue_pop(t, &ip, &port, &no_mse))
                break;
            queue_insert(t, ip, port, 0, no_mse);
        }
    }
    torrent_debug_log("[torrent] queued %u/%u verified initial peers",
            accepted, count);
    return accepted;
}

static void cancel_duplicate_requests(torrent_t *t, uint32_t piece,
                                      uint32_t offset, uint32_t len) {
    for (int i = 0; i < MAX_ACTIVE_PEERS; ++i) {
        peer_t *peer = t->peers[i];
        if (!peer || peer->state != PS_ACTIVE)
            continue;
        peer_cancel_block(peer, piece, offset, len);
    }
}

// ---- callbacks ----
static void cb_block(void *ud, uint32_t idx, uint32_t off,
                     const uint8_t *data, uint32_t len) {
    torrent_t *t = (torrent_t*)ud;
    t->downloaded   += len;
    t->speed_bytes  += len;
    uint32_t block = off / BLOCK_SIZE;
    int already_had_block = piece_mgr_has_block(t->pm, idx, block);
    int duplicated = piece_mgr_block_request_count(t->pm, idx, block) > 1;
    int result = piece_mgr_got_block(t->pm, idx, off, data, len);
    if (result >= 1 && !already_had_block)
        t->last_payload_ms = now_ms();
    if (result >= 1 && duplicated)
        cancel_duplicate_requests(t, idx, off, len);
    if (result < 0) {
        t->fatal_error = 1;
        snprintf(t->error, sizeof(t->error), "%s",
                 storage_error(t->store)[0]
                    ? storage_error(t->store)
                    : "piece processing failed");
    }
}

// ---- web-seed (BEP-19) hand-off ----
// Called on the torrent thread only (see torrent.h). Reuses the same piece
// store/verify path as peer blocks, so a web-seed piece is verified
// against its SHA-1 exactly like anything received from the swarm.
int torrent_piece_done(const torrent_t *t, uint32_t piece) {
    if (!t || !t->pm || piece >= t->pm->num_pieces)
        return 0;
    return bf_has(t->pm->have_bf, piece) ? 1 : 0;
}

int torrent_submit_web_piece(torrent_t *t, uint32_t piece,
                             const uint8_t *data, uint32_t len) {
    if (!t || !t->pm || !data || piece >= t->pm->num_pieces)
        return 0;
    if (bf_has(t->pm->have_bf, piece))
        return 0; // a peer already finished it
    if (len != (uint32_t)piece_len(t->pm, piece))
        return 0;

    piece_mgr_mark_pending(t->pm, piece);
    int result = 1;
    uint64_t received = 0;
    for (uint32_t off = 0; off < len; off += BLOCK_SIZE) {
        uint32_t block = off / BLOCK_SIZE;
        if (piece_mgr_has_block(t->pm, piece, block))
            continue;
        uint32_t blen = (off + BLOCK_SIZE <= len) ? BLOCK_SIZE : (len - off);
        int r = piece_mgr_got_block(t->pm, piece, off, data + off, blen);
        if (r < 0) {
            t->fatal_error = 1;
            snprintf(t->error, sizeof(t->error), "%s",
                     storage_error(t->store)[0]
                        ? storage_error(t->store)
                        : "web-seed piece processing failed");
            return -1;
        }
        t->downloaded  += blen;
        t->speed_bytes += blen;
        received += blen;
        result = r;
    }
    if (received)
        t->last_payload_ms = now_ms();
    return result;
}

static void cb_have(void *ud, uint32_t idx) {
    (void)ud; (void)idx;
}

static void cb_peers(void *ud, const uint8_t *compact, uint32_t cnt) {
    torrent_t *t = (torrent_t*)ud;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t ip   = *(uint32_t*)(compact + i*6);
        uint16_t port = *(uint16_t*)(compact + i*6 + 4);
        queue_push(t, ip, port);
    }
}

static void clear_request(void *ud, const block_req_t *req) {
    torrent_t *t = (torrent_t*)ud;
    if (!t || !req || req->index < 0 || req->offset < 0)
        return;
    piece_mgr_clear_block_requested(t->pm, (uint32_t)req->index,
                                    (uint32_t)req->offset / BLOCK_SIZE);
}

static void clear_peer_requests(torrent_t *t, peer_t *p) {
    if (!t || !p)
        return;
    for (int i = 0; i < p->pipeline_len; i++)
        clear_request(t, &p->pipeline[i]);
    p->pipeline_len = 0;
}

// ---- peer_ctx helper ----
static void fill_ctx(torrent_t *t, peer_ctx_t *ctx) {
    ctx->info_hash   = t->mi.info_hash;
    ctx->peer_id     = t->peer_id;
    ctx->num_pieces  = t->mi.num_pieces;
    ctx->bf_bytes    = (t->mi.num_pieces + 7) / 8;
    ctx->our_bf      = t->pm->available_bf;
    ctx->listen_port = t->listen_port;
    ctx->use_mse     = 1; // try MSE/PE first to reach encryption-required peers
}

// Counts peers that have not reached PS_ACTIVE yet - the in-flight dials
// the burst connector is allowed to top up.
static int count_connecting(const torrent_t *t) {
    int n = 0;
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        peer_t *p = t->peers[i];
        if (p && p->state != PS_ACTIVE && p->state != PS_DEAD)
            n++;
    }
    return n;
}

// Dial-failure fallback: an MSE handshake that was refused mid-handshake
// gets one plaintext retry on the same endpoint; anything else is
// blocklisted for PEER_BLOCKLIST_MS.
static void handle_dial_failure(torrent_t *t, uint32_t ip, uint16_t port,
                                int mse_hs, uint64_t now) {
    if (mse_hs && queue_insert(t, ip, port, 1, /*no_mse*/1))
        return;
    blocklist_add(t, ip, port, now);
}

// ---- connect next peer from queue ----
// Returns 1 if a queue entry was consumed (whether or not the connect
// succeeded), 0 when there is nothing to do (slots full or queue empty) so
// the burst caller can stop.
static int try_connect(torrent_t *t) {
    if (t->num_peers >= MAX_ACTIVE_PEERS) return 0;
    uint32_t ip; uint16_t port; uint8_t no_mse = 0;
    if (!queue_pop(t, &ip, &port, &no_mse)) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip;
    addr.sin_port = port;

    peer_ctx_t ctx; fill_ctx(t, &ctx);

    socket_t fd = net_tcp_connect(&addr);
    if (fd == INVALID_SOCK) {
        blocklist_add(t, ip, port, now_ms());
        return 1;
    }
    peer_t *p = peer_create(fd, addr, &ctx);
    if (!p) {
        net_close(fd);
        blocklist_add(t, ip, port, now_ms());
        return 1;
    }

    p->dl_rate_bps = PIPELINE_BOOTSTRAP_BPS;
    if (no_mse)
        p->mse_enabled = 0; // plaintext fallback retry after MSE was refused

    // Find free slot
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        if (!t->peers[i]) {
            t->peers[i] = p;
            t->num_peers++;
            // No per-dial log: the burst dialer fires up to 16 dials every
            // 50 ms, and failures/timeouts are already logged.
            return 1;
        }
    }
    blocklist_add(t, ip, port, now_ms());
    peer_destroy(p);
    return 1;
}

// ---- schedule block requests ----
static uint32_t peer_pipeline_limit(const torrent_t *t, const peer_t *p) {
    // Bandwidth-delay heuristic: size the in-flight window to
    // ~PIPELINE_TARGET_MS of the peer's measured download rate, so a fast
    // peer gets a deep queue up to the ceiling while a slow peer holds only
    // what it can service.
    // Zero-delivery peers stay on a probe window until the first block
    // arrives, no matter what the bootstrap rate says.
    if (!p->last_piece_ms)
        return BOOTSTRAP_PIPELINE;
    uint64_t want = p->dl_rate_bps * PIPELINE_TARGET_MS / 1000 / BLOCK_SIZE;
    uint32_t ceiling = t->request_pipeline_limit; // per-peer max in flight
    if (want > ceiling)
        want = ceiling;
    // Back off geometrically for peers that keep timing out.
    uint32_t shifts = p->timeout_strikes > 2 ? 2 : p->timeout_strikes;
    want >>= shifts;
    return want < MIN_REQUEST_PIPELINE ? MIN_REQUEST_PIPELINE : (uint32_t)want;
}

static int peer_has_piece(const peer_t *peer, uint32_t piece) {
    return peer && piece / 8 < peer->bf_bytes &&
           bf_has(peer->bitfield, piece);
}

static int peer_has_request(const peer_t *peer, uint32_t piece,
                            uint32_t offset) {
    for (int i = 0; peer && i < peer->pipeline_len; ++i) {
        if (peer->pipeline[i].index == (int)piece &&
            peer->pipeline[i].offset == (int)offset)
            return 1;
    }
    return 0;
}

static void schedule_requests(torrent_t *t, peer_t *p, uint64_t now) {
    if (p->am_choked || p->state != PS_ACTIVE ||
        p->request_cooldown_until_ms > now)
        return;
    uint32_t limit = peer_pipeline_limit(t, p);
    while ((uint32_t)p->pipeline_len < limit) {
        uint32_t pidx = piece_mgr_pick(t->pm, p->bitfield, p->bf_bytes);
        if (pidx == (uint32_t)-1) break;
        piece_mgr_mark_pending(t->pm, pidx);

        piece_slot_t *sl = &t->pm->slots[pidx];
        int64_t plen = piece_len(t->pm, pidx);
        uint32_t nb   = sl->num_blocks;
        int queued = 0;

        for (uint32_t b = 0; b < nb &&
                           (uint32_t)p->pipeline_len < limit; b++) {
            // Skip blocks already received
            if (piece_mgr_has_block(t->pm, pidx, b)) continue;
            if (piece_mgr_block_requested(t->pm, pidx, b)) continue;
            // Skip blocks already in this peer's pipeline
            uint32_t off  = b * BLOCK_SIZE;
            uint32_t blen = ((int64_t)off + BLOCK_SIZE <= plen)
                            ? BLOCK_SIZE : (uint32_t)(plen - off);
            int in_pipe = 0;
            for (int j = 0; j < p->pipeline_len; j++) {
                if (p->pipeline[j].index == (int)pidx &&
                    p->pipeline[j].offset == (int)off) { in_pipe = 1; break; }
            }
            if (in_pipe) continue;
            if (peer_request_block(p, pidx, off, blen)) {
                piece_mgr_mark_block_requested(t->pm, pidx, b);
                queued++;
            }
        }
        if (!queued) break; // nothing left to request for this piece
    }
}

static void schedule_all_peers(torrent_t *t, uint64_t now) {
    uint32_t start = t->schedule_cursor++ % MAX_ACTIVE_PEERS;
    for (uint32_t n = 0; n < MAX_ACTIVE_PEERS; ++n) {
        uint32_t index = (start + n) % MAX_ACTIVE_PEERS;
        peer_t *peer = t->peers[index];
        if (peer)
            schedule_requests(t, peer, now);
    }
}

static uint32_t current_head_piece(const torrent_t *t) {
    return piece_mgr_head_piece(t->pm);
}

static peer_t *pick_hedge_peer(torrent_t *t, const peer_t *primary,
                               uint32_t piece, uint32_t offset,
                               uint64_t now) {
    peer_t *best = NULL;
    for (int i = 0; i < MAX_ACTIVE_PEERS; ++i) {
        peer_t *peer = t->peers[i];
        if (!peer || peer == primary || peer->state != PS_ACTIVE ||
            peer->am_choked || peer->request_cooldown_until_ms > now ||
            !peer_has_piece(peer, piece) ||
            peer_has_request(peer, piece, offset) ||
            (uint32_t)peer->pipeline_len >= peer_pipeline_limit(t, peer))
            continue;
        if (!best || peer->timeout_strikes < best->timeout_strikes ||
            (peer->timeout_strikes == best->timeout_strikes &&
             peer->last_piece_ms > best->last_piece_ms) ||
            (peer->timeout_strikes == best->timeout_strikes &&
             peer->last_piece_ms == best->last_piece_ms &&
             peer->pipeline_len < best->pipeline_len)) {
            best = peer;
        }
    }
    return best;
}

// Hedge threshold derived from the swarm's measured block latency: median
// of per-peer latency EMAs times HEDGE_LATENCY_MULT, clamped to
// [HEDGE_ADAPTIVE_MIN_MS, hedge_after_ms].
static uint32_t adaptive_hedge_after_ms(const torrent_t *t) {
    uint32_t latencies[MAX_ACTIVE_PEERS];
    uint32_t count = 0;
    for (int i = 0; i < MAX_ACTIVE_PEERS; ++i) {
        const peer_t *peer = t->peers[i];
        if (peer && peer->state == PS_ACTIVE && peer->block_lat_ema_ms)
            latencies[count++] = peer->block_lat_ema_ms;
    }
    if (count < HEDGE_MIN_LATENCY_PEERS)
        return t->hedge_after_ms;
    // Insertion sort - count <= MAX_ACTIVE_PEERS, runs at most 1/HEDGE_INTERVAL.
    for (uint32_t i = 1; i < count; ++i) {
        uint32_t value = latencies[i];
        uint32_t j = i;
        while (j > 0 && latencies[j - 1] > value) {
            latencies[j] = latencies[j - 1];
            j--;
        }
        latencies[j] = value;
    }
    uint64_t threshold = (uint64_t)latencies[count / 2] * HEDGE_LATENCY_MULT;
    if (threshold < HEDGE_ADAPTIVE_MIN_MS)
        threshold = HEDGE_ADAPTIVE_MIN_MS;
    if (threshold > t->hedge_after_ms)
        threshold = t->hedge_after_ms;
    return (uint32_t)threshold;
}

static void schedule_hedged_requests(torrent_t *t, uint64_t now) {
    if (!t->hedge_after_ms || !t->pm->strict_order)
        return;
    if (t->last_hedge_ms <= now &&
        now - t->last_hedge_ms < HEDGE_INTERVAL_MS)
        return;
    t->last_hedge_ms = now;
    uint32_t head = current_head_piece(t);
    if (head == UINT32_MAX)
        return;
    uint32_t hedge_after_ms = adaptive_hedge_after_ms(t);
    t->hedge_effective_ms = hedge_after_ms;

    uint32_t outstanding = 0;
    piece_slot_t *headSlot = &t->pm->slots[head];
    for (uint32_t block = 0; block < headSlot->num_blocks; ++block) {
        if (piece_mgr_block_request_count(t->pm, head, block) > 1)
            outstanding++;
    }
    if (outstanding >= MAX_HEDGED_BLOCKS)
        return;
    uint32_t budget = MAX_HEDGED_BLOCKS - outstanding;
    if (budget > MAX_HEDGES_PER_TICK)
        budget = MAX_HEDGES_PER_TICK;
    uint32_t hedged = 0;
    for (int i = 0; i < MAX_ACTIVE_PEERS &&
                    hedged < budget; ++i) {
        peer_t *primary = t->peers[i];
        if (!primary || primary->state != PS_ACTIVE)
            continue;
        for (int j = 0; j < primary->pipeline_len &&
                        hedged < budget; ++j) {
            block_req_t request = primary->pipeline[j];
            if (request.index != (int)head || request.offset < 0 ||
                request.requested_ms > now ||
                now - request.requested_ms < hedge_after_ms)
                continue;
            uint32_t block = (uint32_t)request.offset / BLOCK_SIZE;
            if (piece_mgr_has_block(t->pm, head, block) ||
                piece_mgr_block_request_count(t->pm, head, block) != 1)
                continue;
            peer_t *candidate = pick_hedge_peer(
                t, primary, head, (uint32_t)request.offset, now);
            if (!candidate)
                continue;
            if (peer_request_block(candidate, head,
                                   (uint32_t)request.offset,
                                   (uint32_t)request.length)) {
                piece_mgr_mark_block_requested(t->pm, head, block);
                hedged++;
            }
        }
    }
}

static int check_completion(torrent_t *t) {
    if (t->pm->num_done != t->pm->num_pieces) {
        t->final_verifying = 0;
        t->final_verify_index = 0;
        // Downloading resumed after all - any later completion must go
        // through the real final verification below.
        t->startup_verified_all = 0;
        return 1;
    }

    // Resume of an already-complete torrent: startup verification just read
    // and hashed every piece from disk, so the final pass would produce the
    // same answer at the cost of a second full disk scan.
    if (t->startup_verified_all)
        return 0;

    if (!t->final_verifying) {
        if (!storage_flush(t->store)) {
            torrent_debug_log("[torrent] final storage flush failed");
            return 1;
        }
        t->final_verifying = 1;
        t->final_verify_index = 0;
        torrent_debug_log("[torrent] final verification started");
    }

    if (t->final_verify_index < t->pm->num_pieces) {
        uint32_t idx = t->final_verify_index;
        if (!piece_mgr_verify_piece(t->pm, idx)) {
            t->final_verifying = 0;
            t->final_verify_index = 0;
            return 1;
        }
        t->final_verify_index++;
        return 1;
    }

    return 0;
}

// ---- async tracker announce ----
static void announce_push_results(torrent_t *t, const uint8_t *compact,
                                  uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        queue_push(t, *(uint32_t*)(compact+i*6), *(uint16_t*)(compact+i*6+4));
}

static int announce_cancelled(void *user) {
    return atomic_load(&((torrent_t*)user)->announce_stop);
}

static void *announce_worker(void *arg) {
    torrent_t *t = (torrent_t*)arg;
    // mi/peer_id/listen_port are immutable for the torrent's life;
    // downloaded/left were snapshotted before the thread was launched.
    uint8_t compact[200*6];
    uint32_t n = tracker_announce(&t->mi, t->peer_id, t->listen_port,
                                  t->announce_downloaded, t->announce_left,
                                  compact, 200, announce_cancelled, t);
    pthread_mutex_lock(&t->announce_mutex);
    memcpy(t->announce_compact, compact, (size_t)n*6);
    t->announce_count = n;
    t->announce_done  = 1;
    pthread_mutex_unlock(&t->announce_mutex);
    return NULL;
}

// Kicks off an announce. No-op if one is already in flight. Falls back to a
// synchronous announce if threading is unavailable or spawn fails.
static void announce_start(torrent_t *t, int64_t downloaded, int64_t left) {
    if (t->announce_active)
        return;
    t->announce_downloaded = downloaded;
    t->announce_left       = left;
    if (t->async_ok) {
        t->announce_done  = 0;
        t->announce_count = 0;
        if (pthread_create(&t->announce_thread, NULL, announce_worker, t) == 0) {
            t->announce_active = 1;
            return;
        }
        torrent_debug_log("[torrent] announce thread spawn failed, running inline");
    }
    uint8_t compact[200*6];
    uint32_t n = tracker_announce(&t->mi, t->peer_id, t->listen_port,
                                  downloaded, left, compact, 200,
                                  announce_cancelled, t);
    announce_push_results(t, compact, n);
    torrent_debug_log("[torrent] announce (sync): %u peers", n);
}

// Drains a finished async announce into the peer queue. Called every tick.
static void announce_collect(torrent_t *t) {
    if (!t->announce_active)
        return;
    pthread_mutex_lock(&t->announce_mutex);
    int done = t->announce_done;
    pthread_mutex_unlock(&t->announce_mutex);
    if (!done)
        return;
    pthread_join(t->announce_thread, NULL); // publishes worker's writes
    t->announce_active = 0;
    announce_push_results(t, t->announce_compact, t->announce_count);
    torrent_debug_log("[torrent] announce (async): %u peers", t->announce_count);
}

#ifdef __SWITCH__
// UPnP discover + port mapping blocks ~2 s; run it once in the background
// at start so torrent_create_ex returns immediately. Joined in
// torrent_destroy.
static void *upnp_worker(void *arg) {
    torrent_t *t = (torrent_t*)arg;
    int upnp_err = 0;
    struct UPNPDev *devlist = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &upnp_err);
    if (devlist) {
        int ret = UPNP_GetValidIGD(devlist, &t->upnp_urls, &t->upnp_data,
                                   t->upnp_lanaddr, sizeof(t->upnp_lanaddr));
        if (ret == 1) {
            snprintf(t->upnp_port_str, sizeof(t->upnp_port_str), "%u",
                     (unsigned)t->listen_port);
            int r1 = UPNP_AddPortMapping(t->upnp_urls.controlURL,
                t->upnp_data.first.servicetype,
                t->upnp_port_str, t->upnp_port_str,
                t->upnp_lanaddr, "freeshop", "TCP", NULL, "0");
            int r2 = UPNP_AddPortMapping(t->upnp_urls.controlURL,
                t->upnp_data.first.servicetype,
                t->upnp_port_str, t->upnp_port_str,
                t->upnp_lanaddr, "freeshop", "UDP", NULL, "0");
            t->upnp_mapped = (r1 == UPNPCOMMAND_SUCCESS || r2 == UPNPCOMMAND_SUCCESS);
            torrent_debug_log("[upnp] port %u: TCP=%s UDP=%s", (unsigned)t->listen_port,
                    r1 == UPNPCOMMAND_SUCCESS ? "ok" : "fail",
                    r2 == UPNPCOMMAND_SUCCESS ? "ok" : "fail");
        } else {
            torrent_debug_log("[upnp] no IGD found (ret=%d)", ret);
        }
        freeUPNPDevlist(devlist);
    } else {
        torrent_debug_log("[upnp] discover failed (err=%d)", upnp_err);
    }
    return NULL;
}
#endif

// ---- create ----

// Sum of piece lengths lying entirely in skipped storage ranges. Storage
// configs are immutable for the torrent's life, so this is constant once
// the startup scan (or fast-resume preset) has pre-marked skipped pieces
// done.
static void refresh_skipped_bytes(torrent_t *t) {
    uint64_t skipped = 0;
    const piece_mgr_t *pm = t->pm;
    for (uint32_t idx = 0; idx < pm->num_pieces; idx++) {
        int64_t plen = piece_len(pm, idx);
        int64_t abs_off = (int64_t)idx * pm->mi->piece_length;
        if (storage_range_skipped(pm->store, abs_off, (size_t)plen))
            skipped += (uint64_t)plen;
    }
    t->skipped_bytes = skipped;
}

torrent_t *torrent_create_ex(const metainfo_t *mi,
                             uint16_t listen_port,
                             const char *outdir,
                             const torrent_options_t *options) {
    torrent_t *t = (torrent_t*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    memcpy(&t->mi, mi, sizeof(*mi));
    t->listen_port = listen_port;
    t->startup_verifying = 1;
    t->request_pipeline_limit = options && options->request_pipeline_limit
                              ? options->request_pipeline_limit
                              : MAX_PIPELINE;
    if (t->request_pipeline_limit > MAX_PIPELINE)
        t->request_pipeline_limit = MAX_PIPELINE;
    if (t->request_pipeline_limit < MIN_REQUEST_PIPELINE)
        t->request_pipeline_limit = MIN_REQUEST_PIPELINE;
    t->hedge_after_ms = options ? options->hedge_after_ms : 0;
    t->hedge_effective_ms = t->hedge_after_ms;

    // Peer ID: "-FS0001-" + 12 random bytes
    memcpy(t->peer_id, "-FS0001-", 8);
    rand_bytes(t->peer_id + 8, 12);

    t->store = storage_open_ex(mi, outdir,
                               options ? options->files : NULL);
    if (!t->store) { free(t); return NULL; }

    t->pm = piece_mgr_create_ex(mi, t->store,
                                options ? options->strict_piece_order : 0,
                                options ? options->piece_order : NULL,
                                options ? options->piece_order_count : 0);
    if (!t->pm) { storage_close(t->store); free(t); return NULL; }
    if (options) {
        t->pm->request_allowed = options->request_allowed;
        t->pm->request_allowed_user = options->request_allowed_user;
        piece_mgr_set_strict_policy(t->pm,
                                    options->strict_order_lookahead,
                                    options->strict_fill_pending_first);
        if (options->have_bitfield &&
            options->have_bitfield_len == (mi->num_pieces + 7) / 8) {
            // Fast resume: trust the bitfield saved at the last orderly
            // teardown and skip the startup hash scan. startup_verified_all
            // stays 0, so the final verification pass at completion still
            // re-hashes everything.
            piece_mgr_preset_have(t->pm, options->have_bitfield,
                                  options->have_bitfield_len);
            t->startup_verifying = 0;
            torrent_debug_log("[torrent] fast-resume: %u/%u pieces preset",
                    t->pm->num_done, t->pm->num_pieces);
        } else if (options->fresh_download) {
            // The startup scan still runs (so SKIP-mode files still get
            // marked done for free - see piece.c's piece_mgr_check_existing),
            // but each check becomes O(1) instead of a disk read + SHA-1
            // over the whole piece: a multi-GB DISK-mode target file no
            // longer means minutes spent proving a just-created, all-zero
            // file doesn't match its expected hashes yet.
            t->pm->skip_disk_verify = 1;
        }
    }

    refresh_skipped_bytes(t);

    // DHT: attach to the shared engine; the announce carries this
    // torrent's TCP listen port even though the DHT UDP port is shared.
    t->dht = dht_attach(mi->info_hash, listen_port);

    t->async_ok = (pthread_mutex_init(&t->announce_mutex, NULL) == 0);
    if (!t->async_ok)
        torrent_debug_log("[torrent] announce mutex init failed, announces run inline");

#ifdef __SWITCH__
    // UPnP port mapping - best-effort, one-shot in the background so it
    // does not block startup. Joined in torrent_destroy.
    if (pthread_create(&t->upnp_thread, NULL, upnp_worker, t) == 0)
        t->upnp_thread_active = 1;
    else
        torrent_debug_log("[upnp] worker spawn failed");
#endif

    // Kicks off the first tracker announce off-thread; peers are drained by
    // announce_collect on an upcoming torrent_tick.
    t->last_tracker_ms  = now_ms();
    t->speed_time_ms    = now_ms();
    t->last_health_ms   = t->speed_time_ms;
    announce_start(t, 0, (int64_t)mi->total_length);

    torrent_debug_log("[torrent] started: tracker announce dispatched");
    return t;
}

torrent_t *torrent_create(const metainfo_t *mi,
                          uint16_t listen_port,
                          const char *outdir) {
    return torrent_create_ex(mi, listen_port, outdir, NULL);
}

void torrent_destroy(torrent_t *t) {
    if (!t) return;
    torrent_debug_log("[torrent] destroy begin");
    // Join any in-flight announce before tearing down state it reads. Tell
    // it to give up first: otherwise the join waits out CURLOPT_TIMEOUT for
    // every remaining tracker in the list, and a pause on a torrent with
    // dead trackers hangs the UI for as long as that takes.
    if (t->announce_active) {
        atomic_store(&t->announce_stop, 1);
        pthread_join(t->announce_thread, NULL);
        t->announce_active = 0;
    }
    if (t->async_ok) {
        pthread_mutex_destroy(&t->announce_mutex);
        t->async_ok = 0;
    }
#ifdef __SWITCH__
    // Join the UPnP worker so upnp_mapped/urls are fully written before use.
    if (t->upnp_thread_active) {
        pthread_join(t->upnp_thread, NULL);
        t->upnp_thread_active = 0;
    }
    if (t->upnp_mapped) {
        torrent_debug_log("[torrent] removing UPnP mapping");
        UPNP_DeletePortMapping(t->upnp_urls.controlURL,
            t->upnp_data.first.servicetype,
            t->upnp_port_str, "TCP", NULL);
        UPNP_DeletePortMapping(t->upnp_urls.controlURL,
            t->upnp_data.first.servicetype,
            t->upnp_port_str, "UDP", NULL);
        FreeUPNPUrls(&t->upnp_urls);
    }
#endif
    if (t->dht) {
        dht_detach(t->dht);
        t->dht = NULL;
    }
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++)
        if (t->peers[i]) {
            peer_destroy(t->peers[i]);
            t->peers[i] = NULL;
        }
    piece_mgr_destroy(t->pm);
    t->pm = NULL;
    storage_close(t->store);
    t->store = NULL;
    // NOTE: t->mi is a shallow copy of the caller's metainfo_t - the caller
    // owns the heap members (piece_hashes, files) and must call
    // metainfo_free() on its own copy. Freeing here would cause a
    // double-free on exit.
    free(t);
    torrent_debug_log("[torrent] destroy complete");
}

// Per-peer download-rate EMA feeds the adaptive request pipeline. Sampled
// from the monotonic `downloaded` counter. While the install gate curtails
// requests (rate_freeze) a peer's low throughput says nothing about the
// peer: keep the EMA and discard the interval, so pipelines regain their
// pre-gate depth immediately on resume.
static void sample_peer_rates(torrent_t *t, uint64_t elapsed_ms, uint64_t now) {
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        peer_t *p = t->peers[i];
        if (!p) continue;
        if (t->rate_freeze) {
            p->rate_last_downloaded = p->downloaded;
            continue;
        }
        uint64_t sample = (p->downloaded - p->rate_last_downloaded) * 1000 /
                          (elapsed_ms + 1);
        uint64_t next = ema_update(p->dl_rate_bps, sample);
        // The EMA has a fixed point: pipeline is sized from the rate, so a
        // peer whose window stays full can never measure faster than the
        // window allows. When the window is the binding constraint - kept
        // full, delivering, no strikes, no recent expiries, below the
        // ceiling - grow the estimate multiplicatively so a healthy peer
        // ramps instead of plateauing at its first measured rate.
        uint32_t limit = peer_pipeline_limit(t, p);
        if (sample > 0 && p->timeout_strikes == 0 &&
            limit < t->request_pipeline_limit &&
            (uint32_t)p->pipeline_len * 4 >= limit * 3 &&
            (p->last_expiry_ms == 0 ||
             now - p->last_expiry_ms >= STRIKE_GRACE_MS)) {
            uint64_t grown = p->dl_rate_bps * 3 / 2;
            if (next < grown)
                next = grown;
        }
        p->dl_rate_bps = next;
        p->rate_last_downloaded = p->downloaded;
    }
}

int torrent_tick(torrent_t *t) {
    if (t->fatal_error)
        return -1;
    if (t->startup_verifying) {
        uint64_t verify_start = now_ms();
        while (t->startup_verify_index < t->pm->num_pieces) {
            piece_mgr_check_existing(t->pm, t->startup_verify_index);
            t->startup_verify_index++;
            if (now_ms() - verify_start >= STARTUP_VERIFY_BUDGET_MS)
                break;
        }
        if (t->startup_verify_index >= t->pm->num_pieces) {
            t->startup_verifying = 0;
            t->startup_verified_all =
                t->pm->num_done == t->pm->num_pieces;
            torrent_debug_log("[torrent] startup verification complete: %u/%u pieces",
                    t->pm->num_done, t->pm->num_pieces);
        }
    }

    if (t->pm->num_done == t->pm->num_pieces)
        return check_completion(t);

    uint64_t now = now_ms();

    // Speed update every 1 second
    if (now - t->speed_time_ms >= 1000) {
        uint64_t elapsed_ms = now - t->speed_time_ms;
        uint64_t sample_bps = t->speed_bytes * 1000 / (elapsed_ms + 1);
        t->speed_bps      = ema_update(t->speed_bps, sample_bps);
        t->speed_bytes    = 0;
        sample_peer_rates(t, elapsed_ms, now);
        t->speed_time_ms  = now;
        if (t->dht) {
            int good = 0, dubious = 0;
            dht_shared_nodes(&good, &dubious);
            t->dht_good    = (uint32_t)good;
            t->dht_dubious = (uint32_t)dubious;
        }
    }
    if (now - t->last_health_ms >= 10000) {
        int active = 0;
        int unchoked = 0;
        int inflight = 0;
        for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
            peer_t *p = t->peers[i];
            if (!p || p->state != PS_ACTIVE)
                continue;
            active++;
            if (!p->am_choked)
                unchoked++;
            inflight += p->pipeline_len;
        }
        torrent_debug_log("[torrent] health active=%d unchoked=%d inflight=%d "
                "expired=%u speed=%llu",
                active, unchoked, inflight, t->expired_requests,
                (unsigned long long)t->speed_bps);
        t->expired_requests = 0;
        t->last_health_ms = now;
    }

    // Drains DHT-found peers from this torrent's mailbox on the shared
    // engine - a mutex-guarded memcpy, cheap enough for every tick.
    // queue_push stays on this (the torrent's own) thread.
    if (t->dht) {
        uint8_t found[32][6];
        int nfound = dht_session_poll(t->dht, found, 32);
        for (int i = 0; i < nfound; i++) {
            uint32_t ip_be;
            uint16_t port_be;
            memcpy(&ip_be, found[i], 4);
            memcpy(&port_be, found[i] + 4, 2);
            if (ip_be == 0 || ip_be == INADDR_NONE || ntohs(port_be) < 2)
                continue;
            queue_push(t, ip_be, port_be);
        }
    }

    // Re-announces tracker off-thread. Drains a finished run, then starts
    // the next one if due and none is in flight - never blocks the event
    // loop.
    announce_collect(t);
    if (!t->announce_active) {
        uint64_t since = now - t->last_tracker_ms;
        int due = since >= TRACKER_REANNOUNCE_MS;
        // Not yet due on the normal 30-min interval, but past the starved
        // window: re-announce early if we are short on active peers. The
        // starved-window gate both rate-limits the tracker and keeps this
        // active-peer count off the per-tick hot path.
        if (!due && since >= TRACKER_STARVED_REANNOUNCE_MS) {
            int active = 0;
            for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
                peer_t *p = t->peers[i];
                if (p && p->state == PS_ACTIVE &&
                    ++active >= TRACKER_STARVED_ACTIVE_PEERS)
                    break;
            }
            if (active < TRACKER_STARVED_ACTIVE_PEERS) {
                due = 1;
                torrent_debug_log("[torrent] peer-starved (active=%d), re-announcing early",
                        active);
            }
        }
        if (due) {
            uint64_t announced_downloaded = t->downloaded;
            if (announced_downloaded > (uint64_t)t->mi.total_length)
                announced_downloaded = (uint64_t)t->mi.total_length;
            announce_start(t, (int64_t)announced_downloaded,
                           (int64_t)t->mi.total_length -
                               (int64_t)announced_downloaded);
            t->last_tracker_ms = now;
        }
    }

    // Connect new peers - dial in a burst up to CONNECT_IN_FLIGHT sockets
    // so a large announce is worked through in ~one pass instead of one
    // peer per interval. Stops early when the queue drains or all slots
    // are full.
    if (now - t->last_connect_ms >= CONNECT_INTERVAL_MS) {
        int budget = CONNECT_IN_FLIGHT - count_connecting(t);
        for (int k = 0; k < budget; k++) {
            if (!try_connect(t))
                break;
        }
        t->last_connect_ms = now;
    }

    // Build poll set.
    struct pollfd pfds[MAX_ACTIVE_PEERS];
    int           pfd_peer[MAX_ACTIVE_PEERS];
    int npfd = 0;

    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        peer_t *p = t->peers[i];
        if (!p || p->state == PS_DEAD) continue;
        pfds[npfd].fd     = p->fd;
        pfds[npfd].events = POLLIN |
            ((p->state == PS_CONNECTING || p->sbuf_len > 0) ? POLLOUT : 0);
        pfds[npfd].revents = 0;
        pfd_peer[npfd] = i;
        npfd++;
    }

    int r = poll(pfds, npfd, 10);
    if (r < 0) return 1;

    peer_ctx_t ctx; fill_ctx(t, &ctx);

    // Peer events
    for (int pi = 0; pi < npfd; pi++) {
        if (!(pfds[pi].revents & (POLLIN|POLLOUT|POLLERR|POLLHUP))) continue;
        int slot = pfd_peer[pi];
        peer_t *p = t->peers[slot];
        if (!p) continue;

        int err = 0;
        if (p->state != PS_CONNECTING && (pfds[pi].revents & POLLOUT) &&
            !peer_flush(p))
            err = -1;
        if (err == 0)
            err = peer_recv(p, &ctx, cb_block, cb_have, cb_peers, t);
        if (err < 0) {
            uint32_t ip = p->addr.sin_addr.s_addr;
            uint16_t port = p->addr.sin_port;
            int mse_hs = (p->state == PS_MSE);
            clear_peer_requests(t, p);
            peer_destroy(p);
            t->peers[slot] = NULL;
            t->num_peers--;
            handle_dial_failure(t, ip, port, mse_hs, now);
            continue;
        }
        if (p->am_choked && p->pipeline_len > 0) {
            clear_peer_requests(t, p);
            p->request_cooldown_until_ms = now_ms() +
                                           TIMEOUT_COOLDOWN_BASE_MS;
        }
    }

    // Timeout sweep - separate pass after poll so last_recv_ms is fully updated
    uint64_t now2 = now_ms();
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        peer_t *p = t->peers[i];
        if (!p) continue;
        // Replace unreachable peers quickly so they do not occupy every slot.
        if (p->state == PS_CONNECTING || p->state == PS_MSE ||
            p->state == PS_HANDSHAKE) {
            if (p->connect_time_ms <= now2 &&
                now2 - p->connect_time_ms > CONNECT_TIMEOUT_MS) {
                uint32_t ip = p->addr.sin_addr.s_addr;
                uint16_t port = p->addr.sin_port;
                int mse_hs = (p->state == PS_MSE);
                peer_destroy(p);
                t->peers[i] = NULL;
                t->num_peers--;
                handle_dial_failure(t, ip, port, mse_hs, now2);
            }
            continue;
        }
        if (p->state != PS_ACTIVE) continue;
        // Evict peers that keep us choked and idle only when the table is
        // nearly full and the slot is genuinely worth more to someone else -
        // see PEER_CHOKE_GIVEUP_MS for why an idle choked peer is otherwise
        // worth keeping. Deliberately NOT blocklisted either: it choked us,
        // which every peer does to a leecher, so it stays eligible to be
        // redialed rather than being shut out for PEER_BLOCKLIST_MS.
        if (p->am_choked && t->num_peers >= PEER_CHOKE_EVICT_FLOOR) {
            uint64_t idle_ref = p->last_piece_ms ? p->last_piece_ms
                                                 : p->connect_time_ms;
            if (idle_ref <= now2 && now2 - idle_ref >= PEER_CHOKE_GIVEUP_MS) {
                clear_peer_requests(t, p);
                peer_destroy(p);
                t->peers[i] = NULL;
                t->num_peers--;
                continue;
            }
        }
        uint64_t req_timeout = p->last_piece_ms ? REQUEST_TIMEOUT_MS
                                                : FIRST_BLOCK_TIMEOUT_MS;
        int expired = peer_expire_requests(p, now2, req_timeout,
                                           clear_request, t);
        if (expired > 0) {
            t->expired_requests += (uint32_t)expired;
            p->last_expiry_ms = now2;
            // A peer that delivered a block within the grace window is
            // working through its queue; the expired requests were just
            // queued too deep. Release them without a strike - the
            // cooldown would starve a productive peer.
            int graced = p->last_piece_ms && p->last_piece_ms <= now2 &&
                         now2 - p->last_piece_ms < STRIKE_GRACE_MS;
            if (!graced) {
                if (p->timeout_strikes != UINT32_MAX)
                    p->timeout_strikes++;
                uint64_t cooldown = TIMEOUT_COOLDOWN_BASE_MS *
                           (uint64_t)p->timeout_strikes;
                if (cooldown > TIMEOUT_COOLDOWN_MAX_MS)
                    cooldown = TIMEOUT_COOLDOWN_MAX_MS;
                p->request_cooldown_until_ms = now2 + cooldown;
            }

            uint64_t progress_ms = p->last_piece_ms
                                 ? p->last_piece_ms : p->connect_time_ms;
            if (p->timeout_strikes >= TIMEOUT_DISCONNECT_STRIKES &&
                progress_ms <= now2 &&
                now2 - progress_ms >= TIMEOUT_DISCONNECT_IDLE_MS) {
                blocklist_add(t, p->addr.sin_addr.s_addr,
                              p->addr.sin_port, now2);
                clear_peer_requests(t, p);
                peer_destroy(p);
                t->peers[i] = NULL;
                t->num_peers--;
                continue;
            }
        }
        // Guard against unsigned underflow: only check if last_recv_ms <= now2
        if (p->last_recv_ms <= now2 && now2 - p->last_recv_ms > PEER_TIMEOUT_MS) {
            blocklist_add(t, p->addr.sin_addr.s_addr,
                          p->addr.sin_port, now2);
            clear_peer_requests(t, p);
            peer_destroy(p);
            t->peers[i] = NULL;
            t->num_peers--;
        }
    }

    // Hedging runs before refill so old critical blocks get first choice.
    // Gated during startup verification: unchecked pieces look empty to the
    // picker but may already be valid on disk.
    if (!t->startup_verifying) {
        schedule_hedged_requests(t, now2);
        schedule_all_peers(t, now2);
    }

    return check_completion(t);
}

const char *torrent_last_error(const torrent_t *t) {
    return t && t->error[0] ? t->error : "";
}

int torrent_file_path(const torrent_t *t, uint32_t index, char *out, size_t out_size) {
    if (!t) return 0;
    return storage_file_path(t->store, index, out, out_size);
}

bool torrent_range_complete(const torrent_t *t, uint64_t offset, uint64_t len) {
    if (!t || !t->pm) return false;
    if (len == 0) return true;
    int64_t piece_length = t->mi.piece_length;
    if (piece_length <= 0) return false;

    uint64_t first = offset / (uint64_t)piece_length;
    uint64_t last  = (offset + len - 1) / (uint64_t)piece_length;
    // A range running past the torrent's end is clamped rather than
    // refused: callers derive lengths from container headers, and the last
    // piece is short.
    if (last >= t->pm->num_pieces) {
        if (first >= t->pm->num_pieces) return false;
        last = t->pm->num_pieces - 1;
    }
    for (uint64_t i = first; i <= last; i++) {
        if (!bf_has(t->pm->have_bf, (uint32_t)i)) return false;
    }
    return true;
}

uint64_t torrent_range_downloaded_bytes(const torrent_t *t, uint64_t offset,
                                        uint64_t len) {
    if (!t || !t->pm || len == 0) return 0;
    int64_t piece_length = t->mi.piece_length;
    if (piece_length <= 0) return 0;

    uint64_t range_end = offset + len;
    uint64_t first = offset / (uint64_t)piece_length;
    uint64_t last  = (offset + len - 1) / (uint64_t)piece_length;
    if (first >= t->pm->num_pieces) return 0;
    if (last >= t->pm->num_pieces) last = t->pm->num_pieces - 1;

    uint64_t sum = 0;
    for (uint64_t i = first; i <= last; i++) {
        if (!bf_has(t->pm->have_bf, (uint32_t)i)) continue;
        uint64_t piece_start = i * (uint64_t)piece_length;
        uint64_t piece_end = piece_start + (uint64_t)piece_len(t->pm, (uint32_t)i);
        uint64_t seg_start = piece_start > offset ? piece_start : offset;
        uint64_t seg_end = piece_end < range_end ? piece_end : range_end;
        if (seg_end > seg_start) sum += (seg_end - seg_start);
    }
    return sum;
}

void torrent_prioritize_range(torrent_t *t, uint64_t offset, uint64_t len) {
    if (!t || !t->pm) return;
    int64_t piece_length = t->mi.piece_length;
    if (len == 0 || piece_length <= 0) {
        piece_mgr_set_priority(t->pm, 1, 0); // first > last = clear
        return;
    }
    uint64_t first = offset / (uint64_t)piece_length;
    uint64_t last  = (offset + len - 1) / (uint64_t)piece_length;
    if (first >= t->pm->num_pieces) {
        piece_mgr_set_priority(t->pm, 1, 0);
        return;
    }
    if (last >= t->pm->num_pieces) last = t->pm->num_pieces - 1;
    piece_mgr_set_priority(t->pm, (uint32_t)first, (uint32_t)last);
}

bool torrent_flush(torrent_t *t) {
    if (!t || !t->store) return false;
    return storage_commit(t->store) != 0;
}

void torrent_set_strict_lookahead(torrent_t *t, uint32_t lookahead) {
    if (!t || !t->pm || !t->pm->strict_order || !lookahead)
        return;
    if (t->pm->strict_order_lookahead == lookahead)
        return;
    piece_mgr_set_strict_policy(t->pm, lookahead,
                                t->pm->strict_fill_pending_first);
}

void torrent_set_rate_freeze(torrent_t *t, int freeze) {
    if (!t)
        return;
    t->rate_freeze = freeze ? 1 : 0;
}

uint32_t torrent_copy_have_bitfield(torrent_t *t, uint8_t *out,
                                    uint32_t out_len) {
    if (!t || !t->pm)
        return 0;
    if (t->startup_verifying)
        return 0;
    uint32_t need = (t->pm->num_pieces + 7) / 8;
    if (!out)
        return need;
    if (out_len < need)
        return 0;
    memcpy(out, t->pm->have_bf, need);
    return need;
}

void torrent_stat(const torrent_t *t, torrent_stat_t *s) {
    memset(s, 0, sizeof(*s));
    s->num_pieces_done = t->pm->num_done;
    s->num_pieces      = t->pm->num_pieces;
    s->num_peers       = (uint32_t)t->num_peers;
    for (int i = 0; i < MAX_ACTIVE_PEERS; i++) {
        if (t->peers[i] && t->peers[i]->state == PS_ACTIVE)
            s->num_active_peers++;
    }
    s->dht_good    = t->dht_good;
    s->dht_dubious = t->dht_dubious;
    s->downloaded = t->downloaded;
    s->total_bytes = (uint64_t)t->mi.total_length;
    s->completed_bytes = t->pm->completed_bytes;
    s->skipped_bytes = t->skipped_bytes;
    s->speed_bps  = t->speed_bps;
    s->last_payload_ms = t->last_payload_ms;
    s->num_pieces_verified = t->startup_verifying
                           ? t->startup_verify_index
                           : t->final_verify_index;
    s->verifying = t->startup_verifying || t->final_verifying;
    s->complete = t->final_verifying &&
                  t->final_verify_index == t->pm->num_pieces &&
                  t->pm->num_done == t->pm->num_pieces;
}
