// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/app/magnet_resolver.cpp, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
// Ported from C++ (std::thread/std::vector/std::mutex) to C (pthread +
// fixed-capacity buffers) - see magnet_resolver.h's doc comment for why
// this doesn't reuse peer.h/peer.c.
#include "magnet_resolver.h"
#include "bencode.h"
#include "net.h"
#include "mse.h"
#include "tracker.h"
#include "dht_engine.h"
#include "metainfo.h"
#include "torrent_sha1.h"
#include "torrent_util.h"
#include "torrent_log.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>

#define MAGNET_LOCAL_UT_METADATA_ID 1
#define MAGNET_LOCAL_UT_PEX_ID      2
#define MAGNET_METADATA_PIECE_SIZE  (16*1024)
#define MAGNET_METADATA_LIMIT       (8*1024*1024)
#define MAGNET_OVERALL_TIMEOUT_MS   (90*1000)
#define MAGNET_IO_TIMEOUT_MS        4000
#define MAGNET_MAX_PEERS_PER_TRACKER 64
#define MAGNET_MAX_MERGED_PEERS      192
#define MAGNET_MAX_CONCURRENT_PEERS  12
#define MAGNET_REQUEST_PIPELINE      8
#define MAGNET_REANNOUNCE_BACKOFF_MS 3000
#define MAGNET_MAX_EMPTY_REANNOUNCES 2
#define MAGNET_DHT_SEARCH_TIMEOUT_MS (25*1000)
#define MAGNET_DHT_TARGET_PEERS      32
#define MAGNET_DHT_POLL_INTERVAL_MS  250
#define MAGNET_PEX_THIN_THRESHOLD    3
#define MAGNET_PEX_PEER_TIMEOUT_MS   5000
#define MAGNET_MAX_FRAME  (MAGNET_METADATA_PIECE_SIZE + 4096)
#define MAGNET_MAX_TRACKERS 5

// ---- shared resolve state (definition needed early: report_progress and
// most helpers below take a magnet_resolve_t*) ----
struct magnet_resolve {
    pthread_t thread;
    char out_path[512];
    char uri[700];
    atomic_int cancel_requested;
    atomic_int done;
    int ok;
    char error[MAGNET_ERROR_MAX];
    pthread_mutex_t progress_mutex;
    magnet_progress_t progress;
};

static void report_progress(magnet_resolve_t *r, magnet_stage_t stage,
                            uint32_t completed, uint32_t total,
                            uint32_t peer_index, uint32_t peer_count) {
    pthread_mutex_lock(&r->progress_mutex);
    r->progress.stage = stage;
    r->progress.completed_pieces = completed;
    r->progress.total_pieces = total;
    r->progress.peer_index = peer_index;
    r->progress.peer_count = peer_count;
    pthread_mutex_unlock(&r->progress_mutex);
}

// ---- fixed-capacity peer set (append-only under lock; never reallocates,
// so a captured count + pointer stays valid even while another thread
// keeps appending past it) ----
typedef struct { uint8_t b[6]; } magnet_peer_t;

typedef struct {
    pthread_mutex_t mutex;
    magnet_peer_t peers[MAGNET_MAX_MERGED_PEERS];
    uint32_t count;
} magnet_peer_set_t;

static int peer_set_contains_locked(const magnet_peer_set_t *set, const uint8_t *compact) {
    for (uint32_t i = 0; i < set->count; i++)
        if (memcmp(set->peers[i].b, compact, 6) == 0) return 1;
    return 0;
}

static uint32_t peer_set_append_unique(magnet_peer_set_t *set, const uint8_t *compact, uint32_t count) {
    uint32_t added = 0;
    pthread_mutex_lock(&set->mutex);
    for (uint32_t i = 0; i < count && set->count < MAGNET_MAX_MERGED_PEERS; i++) {
        const uint8_t *item = compact + i * 6;
        if (peer_set_contains_locked(set, item)) continue;
        memcpy(set->peers[set->count].b, item, 6);
        set->count++;
        added++;
    }
    pthread_mutex_unlock(&set->mutex);
    return added;
}

static uint32_t peer_set_count(magnet_peer_set_t *set) {
    pthread_mutex_lock(&set->mutex);
    uint32_t n = set->count;
    pthread_mutex_unlock(&set->mutex);
    return n;
}

// Copies every entry currently in `src` into `dst` (dedup, append-only in
// both, so reading src's count under its own lock and then copying without
// holding it is safe - nothing already counted ever moves or is removed).
static uint32_t merge_peer_set(magnet_peer_set_t *dst, magnet_peer_set_t *src) {
    pthread_mutex_lock(&src->mutex);
    uint32_t count = src->count;
    pthread_mutex_unlock(&src->mutex);
    if (!count) return 0;
    return peer_set_append_unique(dst, (const uint8_t*)src->peers, count);
}

// ---- small string helpers ----
static int hex_nibble(char c, uint8_t *value) {
    if (c >= '0' && c <= '9') { *value = (uint8_t)(c - '0'); return 1; }
    if (c >= 'a' && c <= 'f') { *value = (uint8_t)(c - 'a' + 10); return 1; }
    if (c >= 'A' && c <= 'F') { *value = (uint8_t)(c - 'A' + 10); return 1; }
    return 0;
}

static void url_decode(const char *input, size_t input_len, char *out, size_t out_cap) {
    size_t o = 0;
    for (size_t i = 0; i < input_len && o + 1 < out_cap; i++) {
        if (input[i] == '%' && i + 2 < input_len) {
            uint8_t hi, lo;
            if (hex_nibble(input[i+1], &hi) && hex_nibble(input[i+2], &lo)) {
                out[o++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[o++] = input[i] == '+' ? ' ' : input[i];
    }
    out[o] = '\0';
}

static int strstr_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return 0;
    size_t hn = strlen(haystack), nn = strlen(needle);
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        for (; j < nn; j++)
            if (tolower((unsigned char)haystack[i+j]) != tolower((unsigned char)needle[j])) break;
        if (j == nn) return 1;
    }
    return 0;
}

// This client's actual catalog data (see client/source/catalog/) is
// scraped from RuTracker, so every magnet in practice already carries one
// of these four mirrors as its tr= param. Restricting to them (rather than
// accepting any http(s) host from an untrusted catalog entry) keeps this
// resolver from being pointed at arbitrary hosts.
static int allowed_tracker(const char *url) {
    const char *prefix = "http://";
    size_t prefix_len = 7;
    if (strncmp(url, prefix, prefix_len) != 0) return 0;
    const char *host_start = url + prefix_len;
    const char *slash = strchr(host_start, '/');
    size_t host_len = slash ? (size_t)(slash - host_start) : strlen(host_start);
    char host[128];
    if (host_len >= sizeof(host)) return 0;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    for (size_t i = 0; host[i]; i++) host[i] = (char)tolower((unsigned char)host[i]);
    return strcmp(host, "bt.t-ru.org") == 0 || strcmp(host, "bt2.t-ru.org") == 0 ||
           strcmp(host, "bt3.t-ru.org") == 0 || strcmp(host, "bt4.t-ru.org") == 0;
}

// Bakes every known RuTracker mirror into the resolved torrent as an
// announce-list, not just the single tr= the magnet carried - otherwise
// the resolved torrent only ever announces to one mirror, needlessly
// starving the swarm if that one is briefly down.
static uint32_t build_tracker_candidates(const char *original, char out[MAGNET_MAX_TRACKERS][512]) {
    static const char *mirrors[] = {
        "http://bt.t-ru.org/ann?magnet",
        "http://bt2.t-ru.org/ann?magnet",
        "http://bt3.t-ru.org/ann?magnet",
        "http://bt4.t-ru.org/ann?magnet",
    };
    uint32_t count = 0;
    if (allowed_tracker(original)) {
        snprintf(out[count], 512, "%s", original);
        count++;
    }
    for (size_t i = 0; i < sizeof(mirrors)/sizeof(mirrors[0]) && count < MAGNET_MAX_TRACKERS; i++) {
        int dup = 0;
        for (uint32_t j = 0; j < count; j++)
            if (strcmp(out[j], mirrors[i]) == 0) { dup = 1; break; }
        if (dup) continue;
        snprintf(out[count], 512, "%s", mirrors[i]);
        count++;
    }
    return count;
}

// ---- blocking socket helpers (this resolver is a short-lived, one-shot
// operation - see magnet_resolver.h - so blocking-with-timeout sockets on
// a background thread are simpler than a non-blocking state machine here) ----
static int wait_fd(socket_t fd, short events, int timeout_ms) {
    struct pollfd item; item.fd = fd; item.events = events; item.revents = 0;
    int result;
    do {
        result = poll(&item, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (item.revents & events) != 0;
}

static int send_all(socket_t fd, const uint8_t *data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        if (!wait_fd(fd, POLLOUT, MAGNET_IO_TIMEOUT_MS)) return 0;
        ssize_t n = send(fd, data + sent, size - sent, 0);
        if (n <= 0) return 0;
        sent += (size_t)n;
    }
    return 1;
}

static int recv_all(socket_t fd, uint8_t *data, size_t size, int timeout_ms) {
    size_t received = 0;
    while (received < size) {
        if (!wait_fd(fd, POLLIN, timeout_ms)) return 0;
        ssize_t n = recv(fd, data + received, size - received, 0);
        if (n <= 0) return 0;
        received += (size_t)n;
    }
    return 1;
}

// A peer socket with an optional MSE/PE (RC4) encryption layer. Many
// RuTracker peers require encryption and silently drop a plaintext BEP-3
// handshake, so all post-handshake traffic must be able to ride through
// RC4. When `encrypted` is 0 this is a thin passthrough over the raw socket.
typedef struct {
    socket_t fd;
    int encrypted;
    rc4_t send_key;
    rc4_t recv_key;
    uint8_t backlog[1024]; // decrypted bytes read past the MSE handshake
    size_t backlog_len;    // that belong to the peer stream
    size_t backlog_pos;
} peer_wire_t;

static void peer_wire_init(peer_wire_t *w) {
    memset(w, 0, sizeof(*w));
    w->fd = INVALID_SOCK;
}

static int wire_send_all(peer_wire_t *w, const uint8_t *data, size_t size) {
    if (!w->encrypted) return send_all(w->fd, data, size);
    uint8_t chunk[4096];
    size_t offset = 0;
    while (offset < size) {
        size_t count = size - offset < sizeof(chunk) ? size - offset : sizeof(chunk);
        rc4_crypt(&w->send_key, data + offset, chunk, count);
        if (!send_all(w->fd, chunk, count)) return 0;
        offset += count;
    }
    return 1;
}

static int wire_recv_all(peer_wire_t *w, uint8_t *data, size_t size, int timeout_ms) {
    size_t got = 0;
    if (w->encrypted && w->backlog_pos < w->backlog_len) {
        size_t take = w->backlog_len - w->backlog_pos;
        if (take > size) take = size;
        memcpy(data, w->backlog + w->backlog_pos, take);
        w->backlog_pos += take;
        got += take;
    }
    if (got == size) return 1;
    if (!recv_all(w->fd, data + got, size - got, timeout_ms)) return 0;
    if (w->encrypted)
        rc4_crypt(&w->recv_key, data + got, data + got, size - got);
    return 1;
}

static int send_frame(peer_wire_t *w, const uint8_t *payload, uint32_t len) {
    uint8_t header[4] = { (uint8_t)(len>>24), (uint8_t)(len>>16), (uint8_t)(len>>8), (uint8_t)len };
    return wire_send_all(w, header, 4) && (len == 0 || wire_send_all(w, payload, len));
}

static int recv_frame(peer_wire_t *w, uint8_t *buf, size_t cap, size_t *out_len) {
    uint8_t header[4];
    if (!wire_recv_all(w, header, 4, MAGNET_IO_TIMEOUT_MS)) return 0;
    uint32_t size = ((uint32_t)header[0]<<24)|((uint32_t)header[1]<<16)|((uint32_t)header[2]<<8)|(uint32_t)header[3];
    if (size > MAGNET_METADATA_PIECE_SIZE + 4096 || size > cap) return 0;
    *out_len = size;
    return size == 0 || wire_recv_all(w, buf, size, MAGNET_IO_TIMEOUT_MS);
}

// What one peer attempt achieved, and where it died - telling "we never
// reached it" apart from "it answered but the handshake did not take" is
// the difference between a network that blocks BitTorrent and a protocol
// problem, and it's what a bug report can't reconstruct from a bare "failed".
typedef struct {
    int connected;
    int handshake_verified;
    const char *failure;
    int error;
} peer_attempt_t;

static socket_t magnet_tcp_connect(const uint8_t *compact, peer_attempt_t *attempt) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    memcpy(&addr.sin_addr.s_addr, compact, 4);
    memcpy(&addr.sin_port, compact + 4, 2);
    socket_t fd = net_tcp_connect(&addr);
    if (fd == INVALID_SOCK) {
        attempt->failure = "connect";
        attempt->error = errno;
        return INVALID_SOCK;
    }
    if (!wait_fd(fd, POLLOUT, MAGNET_IO_TIMEOUT_MS)) {
        net_close(fd);
        attempt->failure = "connect timed out";
        attempt->error = 0;
        return INVALID_SOCK;
    }
    int sock_err = 0;
    socklen_t elen = sizeof(sock_err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &elen) != 0 || sock_err != 0) {
        net_close(fd);
        attempt->failure = "connect refused";
        attempt->error = sock_err;
        return INVALID_SOCK;
    }
    return fd;
}

static void build_bt_handshake(uint8_t hs[68], const uint8_t info_hash[20], const uint8_t peer_id[20]) {
    memset(hs, 0, 68);
    hs[0] = 19;
    memcpy(hs+1, "BitTorrent protocol", 19);
    hs[25] = 0x10; // extension protocol (BEP-10)
    memcpy(hs+28, info_hash, 20);
    memcpy(hs+48, peer_id, 20);
}

static int read_bt_handshake_reply(peer_wire_t *w, const uint8_t info_hash[20]) {
    uint8_t response[68];
    return wire_recv_all(w, response, sizeof(response), MAGNET_IO_TIMEOUT_MS) &&
           response[0] == 19 &&
           memcmp(response+1, "BitTorrent protocol", 19) == 0 &&
           memcmp(response+28, info_hash, 20) == 0 &&
           (response[25] & 0x10) != 0;
}

// Drives the MSE initiator handshake on a freshly connected socket. The BT
// handshake rides inside the encrypted request as the IA payload, so on
// success the peer's (encrypted) BT handshake reply is left for
// read_bt_handshake_reply.
static int mse_handshake(socket_t fd, const uint8_t info_hash[20], const uint8_t peer_id[20], peer_wire_t *w) {
    mse_client_t client;
    uint8_t priv[MSE_DH_LEN];
    mse_dh_private(priv);
    uint8_t ia[68];
    build_bt_handshake(ia, info_hash, peer_id);
    uint8_t out[512];
    size_t produced = 0;
    if (mse_client_start(&client, info_hash, priv, NULL, 0, ia, sizeof(ia), out, sizeof(out), &produced) != MSE_CONTINUE)
        return 0;
    if (!send_all(fd, out, produced)) return 0;

    uint8_t inbuf[2048];
    size_t inbuf_len = 0;
    uint64_t deadline = now_ms() + 2 * MAGNET_IO_TIMEOUT_MS;
    while (now_ms() < deadline) {
        if (!wait_fd(fd, POLLIN, MAGNET_IO_TIMEOUT_MS)) return 0;
        if (inbuf_len >= sizeof(inbuf)) return 0;
        ssize_t n = recv(fd, inbuf + inbuf_len, sizeof(inbuf) - inbuf_len, 0);
        if (n <= 0) return 0;
        inbuf_len += (size_t)n;

        size_t consumed = 0;
        produced = 0;
        mse_status_t status = mse_client_feed(&client, inbuf, inbuf_len, &consumed, out, sizeof(out), &produced);
        if (produced && !send_all(fd, out, produced)) return 0;
        if (consumed > 0) {
            memmove(inbuf, inbuf + consumed, inbuf_len - consumed);
            inbuf_len -= consumed;
        }
        if (status == MSE_FAIL) return 0;
        if (status == MSE_DONE) {
            w->fd = fd;
            w->encrypted = 1;
            w->send_key = client.send_rc4;
            w->recv_key = client.recv_rc4;
            // Bytes past the handshake are the peer's encrypted stream;
            // decrypt them now so wire_recv_all can serve them as
            // plaintext backlog.
            if (inbuf_len > 0) {
                size_t take = inbuf_len > sizeof(w->backlog) ? sizeof(w->backlog) : inbuf_len;
                rc4_crypt(&w->recv_key, inbuf, w->backlog, take);
                w->backlog_len = take;
                w->backlog_pos = 0;
            }
            return 1;
        }
    }
    return 0;
}

// Connects, then completes the BT handshake. Plaintext is tried first; if
// the peer drops it (encryption-only), reconnects and retries over MSE/PE.
// `w` owns the socket and carries any encryption state on success.
static int connect_peer(const uint8_t *compact, const uint8_t info_hash[20], const uint8_t peer_id[20],
                        peer_wire_t *w, peer_attempt_t *attempt) {
    socket_t fd = magnet_tcp_connect(compact, attempt);
    if (fd == INVALID_SOCK) return 0;
    attempt->connected = 1;

    uint8_t handshake[68];
    build_bt_handshake(handshake, info_hash, peer_id);
    if (send_all(fd, handshake, sizeof(handshake))) {
        peer_wire_init(w);
        w->fd = fd;
        w->encrypted = 0;
        if (read_bt_handshake_reply(w, info_hash)) {
            net_set_tcp_receive_buffer(fd);
            return 1;
        }
    }
    // Plaintext refused or dropped - retry the peer with MSE/PE encryption.
    net_close(fd);
    peer_wire_init(w);

    fd = magnet_tcp_connect(compact, attempt);
    if (fd == INVALID_SOCK) return 0;
    if (!mse_handshake(fd, info_hash, peer_id, w) || !read_bt_handshake_reply(w, info_hash)) {
        net_close(fd);
        peer_wire_init(w);
        attempt->failure = "plaintext and MSE handshake";
        attempt->error = 0;
        return 0;
    }
    net_set_tcp_receive_buffer(fd);
    return 1;
}

static int negotiate_metadata(peer_wire_t *w, uint8_t *peer_extension, size_t *metadata_size) {
    static const char handshake[] = "d1:md11:ut_metadatai1eee";
    uint8_t request[2 + sizeof(handshake) - 1];
    request[0] = 20; request[1] = 0;
    memcpy(request + 2, handshake, sizeof(handshake) - 1);
    if (!send_frame(w, request, (uint32_t)sizeof(request))) return 0;

    uint8_t frame[MAGNET_MAX_FRAME];
    for (int message = 0; message < 32; message++) {
        size_t frame_len = 0;
        if (!recv_frame(w, frame, sizeof(frame), &frame_len)) return 0;
        if (frame_len < 3 || frame[0] != 20 || frame[1] != 0) continue;
        const char *begin = (const char*)frame + 2;
        const char *end = (const char*)frame + frame_len;
        be_node_t root;
        const char *cursor = begin;
        if (!be_decode(&cursor, end, &root) || root.type != BE_DICT) return 0;
        be_node_t map, extension, size;
        if (!be_dict_get(root.buf, root.buf + root.raw_len, "m", 1, &map) || map.type != BE_DICT ||
            !be_dict_get(map.buf, map.buf + map.raw_len, "ut_metadata", 11, &extension) ||
            extension.type != BE_INT || extension.ival <= 0 || extension.ival > 255 ||
            !be_dict_get(root.buf, root.buf + root.raw_len, "metadata_size", 13, &size) ||
            size.type != BE_INT || size.ival <= 0 || size.ival > (int64_t)MAGNET_METADATA_LIMIT)
            return 0;
        *peer_extension = (uint8_t)extension.ival;
        *metadata_size = (size_t)size.ival;
        return 1;
    }
    return 0;
}

static int send_metadata_request(peer_wire_t *w, uint8_t extension, uint32_t piece) {
    char body[64];
    int blen = snprintf(body, sizeof(body), "d8:msg_typei0e5:piecei%ue", piece);
    if (blen < 0 || (size_t)blen >= sizeof(body)) return 0;
    uint8_t frame[2 + 64];
    frame[0] = 20; frame[1] = extension;
    memcpy(frame + 2, body, (size_t)blen);
    return send_frame(w, frame, (uint32_t)(2 + blen));
}

static int receive_metadata_piece(peer_wire_t *w, uint8_t *frame_buf, size_t frame_cap,
                                  uint32_t *piece, const uint8_t **data, size_t *data_size) {
    for (int message = 0; message < 32; message++) {
        size_t frame_len = 0;
        if (!recv_frame(w, frame_buf, frame_cap, &frame_len)) {
            torrent_debug_log("[magnet] peer frame receive timed out");
            return 0;
        }
        if (frame_len < 3 || frame_buf[0] != 20 || frame_buf[1] != MAGNET_LOCAL_UT_METADATA_ID)
            continue;
        const char *begin = (const char*)frame_buf + 2;
        const char *end = (const char*)frame_buf + frame_len;
        const char *cursor = begin;
        be_node_t header;
        if (!be_decode(&cursor, end, &header) || header.type != BE_DICT) {
            torrent_debug_log("[magnet] invalid metadata message header");
            return 0;
        }
        be_node_t type, index;
        if (!be_dict_get(header.buf, header.buf + header.raw_len, "msg_type", 8, &type) ||
            !be_dict_get(header.buf, header.buf + header.raw_len, "piece", 5, &index) ||
            type.type != BE_INT || index.type != BE_INT || index.ival < 0) {
            torrent_debug_log("[magnet] incomplete metadata message header");
            return 0;
        }
        if (type.ival == 2) {
            torrent_debug_log("[magnet] peer rejected metadata piece %lld", (long long)index.ival);
            return 0;
        }
        if (type.ival != 1) continue;
        *piece = (uint32_t)index.ival;
        *data = (const uint8_t*)cursor;
        *data_size = (size_t)(end - cursor);
        return 1;
    }
    return 0;
}

// Fetches the whole metadata (info dict) from one peer. On success,
// *out_metadata is a malloc'd buffer the caller owns.
static int fetch_metadata_from_peer(magnet_resolve_t *r, const uint8_t *compact, const magnet_spec_t *spec,
                                    const uint8_t peer_id[20], uint32_t peer_index, uint32_t peer_count,
                                    uint64_t deadline, atomic_int *stop_workers,
                                    uint8_t **out_metadata, size_t *out_metadata_len,
                                    peer_attempt_t *attempt) {
    memset(attempt, 0, sizeof(*attempt));
    attempt->failure = "";
    if (atomic_load(&r->cancel_requested) || atomic_load(stop_workers) || now_ms() >= deadline)
        return 0;
    report_progress(r, MAGNET_STAGE_CONNECTING, 0, 0, peer_index + 1, peer_count);

    peer_wire_t wire;
    peer_wire_init(&wire);
    if (!connect_peer(compact, spec->info_hash, peer_id, &wire, attempt)) {
        torrent_debug_log("[magnet] peer %u/%u failed at %s (errno %d)",
                          peer_index+1, peer_count, attempt->failure, attempt->error);
        net_close(wire.fd);
        return 0;
    }
    torrent_debug_log("[magnet] peer %u/%u BitTorrent handshake ok%s",
                      peer_index+1, peer_count, wire.encrypted ? " (MSE)" : "");
    attempt->handshake_verified = 1;

    uint8_t extension = 0;
    size_t metadata_size = 0;
    if (!negotiate_metadata(&wire, &extension, &metadata_size)) {
        torrent_debug_log("[magnet] peer %u/%u has no usable ut_metadata", peer_index+1, peer_count);
        net_close(wire.fd);
        return 0;
    }
    torrent_debug_log("[magnet] peer %u/%u ut_metadata=%u size=%zu",
                      peer_index+1, peer_count, extension, metadata_size);

    uint8_t *local = (uint8_t*)malloc(metadata_size);
    uint32_t num_pieces = (uint32_t)((metadata_size + MAGNET_METADATA_PIECE_SIZE - 1) / MAGNET_METADATA_PIECE_SIZE);
    uint8_t *received = (uint8_t*)calloc(num_pieces, 1);
    uint8_t *requested = (uint8_t*)calloc(num_pieces, 1);
    if (!local || !received || !requested) {
        free(local); free(received); free(requested);
        net_close(wire.fd);
        return 0;
    }
    uint32_t completed = 0;
    uint32_t in_flight = 0;
    uint8_t *frame = (uint8_t*)malloc(MAGNET_MAX_FRAME);
    int ok = frame != NULL;

    while (ok && !atomic_load(&r->cancel_requested) && !atomic_load(stop_workers) &&
           now_ms() < deadline && completed < num_pieces) {
        for (uint32_t piece = 0; piece < num_pieces && in_flight < MAGNET_REQUEST_PIPELINE; piece++) {
            if (received[piece] || requested[piece]) continue;
            if (!send_metadata_request(&wire, extension, piece)) { ok = 0; break; }
            requested[piece] = 1;
            in_flight++;
        }
        if (!ok) break;

        uint32_t piece = 0;
        const uint8_t *bytes = NULL;
        size_t byte_count = 0;
        if (!receive_metadata_piece(&wire, frame, MAGNET_MAX_FRAME, &piece, &bytes, &byte_count)) {
            torrent_debug_log("[magnet] peer %u/%u metadata receive failed", peer_index+1, peer_count);
            ok = 0;
            break;
        }
        if (piece >= num_pieces) continue;
        if (requested[piece] && in_flight) in_flight--;
        requested[piece] = 0;
        size_t offset = (size_t)piece * MAGNET_METADATA_PIECE_SIZE;
        size_t remain = metadata_size - offset;
        size_t expected = remain < MAGNET_METADATA_PIECE_SIZE ? remain : MAGNET_METADATA_PIECE_SIZE;
        if (byte_count != expected) {
            torrent_debug_log("[magnet] peer %u/%u wrong metadata piece size %zu/%zu",
                              peer_index+1, peer_count, byte_count, expected);
            ok = 0;
            break;
        }
        if (!received[piece]) {
            memcpy(local + offset, bytes, byte_count);
            received[piece] = 1;
            completed++;
            report_progress(r, MAGNET_STAGE_FETCHING_METADATA, completed, num_pieces, peer_index+1, peer_count);
        }
    }

    free(frame);
    net_close(wire.fd);
    free(received);
    free(requested);
    if (!ok || completed != num_pieces) {
        free(local);
        return 0;
    }
    *out_metadata = local;
    *out_metadata_len = metadata_size;
    return 1;
}

// Advertises both ut_metadata and ut_pex so the peer pushes its swarm view
// to us on our advertised ut_pex id.
static int send_pex_handshake(peer_wire_t *w) {
    static const char handshake[] = "d1:md11:ut_metadatai1e6:ut_pexi2eee";
    uint8_t request[2 + sizeof(handshake) - 1];
    request[0] = 20; request[1] = 0;
    memcpy(request + 2, handshake, sizeof(handshake) - 1);
    return send_frame(w, request, (uint32_t)sizeof(request));
}

// Connects to one thin-list peer, advertises ut_pex, and harvests the
// compact peers it pushes in any ut_pex "added" field. Best effort: peers
// emit PEX on their own cadence, so an empty result is normal.
static void harvest_pex_from_peer(const uint8_t *compact, const magnet_spec_t *spec,
                                  const uint8_t peer_id[20], atomic_int *cancelled,
                                  magnet_peer_set_t *out) {
    peer_wire_t wire;
    peer_wire_init(&wire);
    peer_attempt_t attempt;
    memset(&attempt, 0, sizeof(attempt));
    if (!connect_peer(compact, spec->info_hash, peer_id, &wire, &attempt)) {
        net_close(wire.fd);
        return;
    }
    if (!send_pex_handshake(&wire)) {
        net_close(wire.fd);
        return;
    }
    uint64_t deadline = now_ms() + MAGNET_PEX_PEER_TIMEOUT_MS;
    uint8_t *frame = (uint8_t*)malloc(MAGNET_MAX_FRAME);
    if (frame) {
        while (!atomic_load(cancelled) && now_ms() < deadline) {
            size_t frame_len = 0;
            if (!recv_frame(&wire, frame, MAGNET_MAX_FRAME, &frame_len)) break;
            if (frame_len < 3 || frame[0] != 20 || frame[1] != MAGNET_LOCAL_UT_PEX_ID) continue;
            const char *begin = (const char*)frame + 2;
            const char *end = (const char*)frame + frame_len;
            be_node_t root;
            const char *cursor = begin;
            if (!be_decode(&cursor, end, &root) || root.type != BE_DICT) break;
            be_node_t added;
            if (be_dict_get(root.buf, root.buf + root.raw_len, "added", 5, &added) &&
                added.type == BE_STR && added.slen >= 6) {
                peer_set_append_unique(out, (const uint8_t*)added.sval, (uint32_t)(added.slen / 6));
            }
        }
        free(frame);
    }
    net_close(wire.fd);
}

static int write_torrent_atomic(const char *path, const uint8_t *data, size_t len, char error[MAGNET_ERROR_MAX]) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        snprintf(error, MAGNET_ERROR_MAX, "Unable to create the resolved torrent file.");
        return 0;
    }
    size_t written = fwrite(data, 1, len, f);
    int close_ok = fclose(f) == 0;
    if (written != len || !close_ok) {
        remove(tmp);
        snprintf(error, MAGNET_ERROR_MAX, "Unable to write the resolved torrent file.");
        return 0;
    }
    if (rename(tmp, path) != 0) {
        // FAT (sdmc) may refuse rename-over-existing; retry after unlink.
        remove(path);
        if (rename(tmp, path) != 0) {
            remove(tmp);
            snprintf(error, MAGNET_ERROR_MAX, "Unable to replace the resolved torrent file.");
            return 0;
        }
    }
    return 1;
}

// ---- DHT search (runs in parallel with the tracker announces) ----
typedef struct {
    magnet_peer_set_t set;
} magnet_dht_search_t;

typedef struct {
    const uint8_t *info_hash;
    atomic_int *cancelled;
    atomic_int *stop;
    magnet_dht_search_t *search;
} dht_search_ctx_t;

static void *run_dht_search(void *arg) {
    dht_search_ctx_t *ctx = (dht_search_ctx_t*)arg;
    if (atomic_load(ctx->cancelled) || atomic_load(ctx->stop)) return NULL;
    // Pure lookup (announce port 0) on the shared engine: joins the
    // existing routing table if a download is already running instead of
    // racing to create a second one.
    dht_session_t *session = dht_attach(ctx->info_hash, 0);
    if (!session) {
        torrent_debug_log("[magnet] dht unavailable, resolving without it");
        return NULL;
    }
    uint64_t deadline = now_ms() + MAGNET_DHT_SEARCH_TIMEOUT_MS;
    while (!atomic_load(ctx->cancelled) && !atomic_load(ctx->stop) && now_ms() < deadline &&
           peer_set_count(&ctx->search->set) < MAGNET_DHT_TARGET_PEERS) {
        uint8_t found[32][6];
        int count = dht_session_poll(session, found, 32);
        if (count > 0)
            peer_set_append_unique(&ctx->search->set, &found[0][0], (uint32_t)count);
        else
            usleep(MAGNET_DHT_POLL_INTERVAL_MS * 1000);
    }
    dht_detach(session);
    return NULL;
}

static int magnet_tracker_cancelled(void *user) {
    return atomic_load((atomic_int*)user);
}

int magnet_parse(const char *uri, magnet_spec_t *spec, char error[MAGNET_ERROR_MAX]) {
    memset(spec, 0, sizeof(*spec));
    error[0] = '\0';
    if (strncmp(uri, "magnet:?", 8) != 0) {
        snprintf(error, MAGNET_ERROR_MAX, "Catalog entry has an invalid magnet URI.");
        return 0;
    }
    int have_hash = 0;
    size_t start = 8;
    size_t uri_len = strlen(uri);
    while (start <= uri_len) {
        const char *amp = strchr(uri + start, '&');
        size_t end = amp ? (size_t)(amp - uri) : uri_len;
        const char *pair = uri + start;
        size_t pair_len = end - start;
        const char *eq = (const char*)memchr(pair, '=', pair_len);
        size_t key_len = eq ? (size_t)(eq - pair) : pair_len;
        char key[16];
        if (key_len >= sizeof(key)) key_len = sizeof(key) - 1;
        memcpy(key, pair, key_len);
        key[key_len] = '\0';

        char value[600];
        value[0] = '\0';
        if (eq) {
            const char *val_start = eq + 1;
            size_t val_len = (size_t)(pair + pair_len - val_start);
            url_decode(val_start, val_len, value, sizeof(value));
        }

        if (strcmp(key, "xt") == 0 && strncmp(value, "urn:btih:", 9) == 0) {
            const char *hash = value + 9;
            size_t hash_len = strlen(hash);
            if (hash_len != 40 || have_hash) {
                snprintf(error, MAGNET_ERROR_MAX, "Only one hexadecimal BitTorrent v1 hash is supported.");
                return 0;
            }
            for (size_t i = 0; i < 20; i++) {
                uint8_t hi, lo;
                if (!hex_nibble(hash[i*2], &hi) || !hex_nibble(hash[i*2+1], &lo)) {
                    snprintf(error, MAGNET_ERROR_MAX, "The magnet info hash is not hexadecimal.");
                    return 0;
                }
                spec->info_hash[i] = (uint8_t)((hi << 4) | lo);
            }
            for (size_t i = 0; i < 40; i++)
                spec->info_hash_hex[i] = (char)toupper((unsigned char)hash[i]);
            spec->info_hash_hex[40] = '\0';
            have_hash = 1;
        } else if (strcmp(key, "tr") == 0 && spec->tracker_url[0] == '\0' && allowed_tracker(value)) {
            snprintf(spec->tracker_url, sizeof(spec->tracker_url), "%s", value);
        }

        if (!amp) break;
        start = end + 1;
    }
    if (!have_hash) {
        snprintf(error, MAGNET_ERROR_MAX, "The magnet does not contain a BitTorrent v1 info hash.");
        return 0;
    }
    if (spec->tracker_url[0] == '\0') {
        snprintf(error, MAGNET_ERROR_MAX, "The magnet does not contain a supported RuTracker tracker.");
        return 0;
    }
    return 1;
}

// Wraps a fetched info dict into a full .torrent (announce + announce-list
// + info) and validates it: parses clean, and its SHA-1 matches the
// magnet's info hash. *out_torrent is malloc'd; caller frees.
static int magnet_build_torrent(const magnet_spec_t *spec, const uint8_t *info, size_t info_len,
                                uint8_t **out_torrent, size_t *out_len, char error[MAGNET_ERROR_MAX]) {
    if (info_len == 0 || info_len > MAGNET_METADATA_LIMIT) {
        snprintf(error, MAGNET_ERROR_MAX, "Peer metadata is empty or too large.");
        return 0;
    }
    const char *cursor = (const char*)info;
    const char *end = cursor + info_len;
    be_node_t root;
    if (!be_decode(&cursor, end, &root) || root.type != BE_DICT || cursor != end) {
        snprintf(error, MAGNET_ERROR_MAX, "Peer metadata is not a valid info dictionary.");
        return 0;
    }
    uint8_t digest[20];
    sha1(info, info_len, digest);
    if (memcmp(digest, spec->info_hash, 20) != 0) {
        snprintf(error, MAGNET_ERROR_MAX, "Peer metadata does not match the magnet info hash.");
        return 0;
    }

    char trackers[MAGNET_MAX_TRACKERS][512];
    uint32_t tracker_count = build_tracker_candidates(spec->tracker_url, trackers);

    size_t cap = info_len + 4096;
    uint8_t *buf = (uint8_t*)malloc(cap);
    if (!buf) {
        snprintf(error, MAGNET_ERROR_MAX, "Out of memory building the torrent.");
        return 0;
    }
    // Dict keys stay sorted (bencode canonical form): announce <
    // announce-list < info.
    size_t off = 0;
    off += (size_t)snprintf((char*)buf + off, cap - off, "d8:announce%zu:%s",
                            strlen(spec->tracker_url), spec->tracker_url);
    off += (size_t)snprintf((char*)buf + off, cap - off, "13:announce-listl");
    for (uint32_t i = 0; i < tracker_count; i++)
        off += (size_t)snprintf((char*)buf + off, cap - off, "l%zu:%se",
                                strlen(trackers[i]), trackers[i]);
    off += (size_t)snprintf((char*)buf + off, cap - off, "e4:info");
    memcpy(buf + off, info, info_len);
    off += info_len;
    buf[off++] = 'e';

    metainfo_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    if (!metainfo_parse(buf, off, &parsed)) {
        free(buf);
        snprintf(error, MAGNET_ERROR_MAX, "Resolved metadata is not a supported safe torrent.");
        return 0;
    }
    int hash_matches = memcmp(parsed.info_hash, spec->info_hash, 20) == 0;
    metainfo_free(&parsed);
    if (!hash_matches) {
        free(buf);
        snprintf(error, MAGNET_ERROR_MAX, "Generated torrent failed info-hash validation.");
        return 0;
    }
    *out_torrent = buf;
    *out_len = off;
    return 1;
}

// ---- peer worker pool (races up to MAGNET_MAX_CONCURRENT_PEERS peers per
// round; first to deliver valid metadata wins) ----
typedef struct {
    magnet_resolve_t *r;
    magnet_peer_set_t *peers;
    const magnet_spec_t *spec;
    const uint8_t *peer_id;
    uint32_t round_end;
    uint64_t deadline;
    atomic_int *stop_workers;
    pthread_mutex_t *pick_mutex; // guards next_peer/resolved/out_metadata*
    uint32_t *next_peer;
    int *resolved;
    uint8_t **out_metadata;
    size_t *out_metadata_len;
    magnet_peer_set_t *verified;
    atomic_uint *reached_peers;
} peer_worker_ctx_t;

static void *peer_worker_fn(void *arg) {
    peer_worker_ctx_t *ctx = (peer_worker_ctx_t*)arg;
    magnet_resolve_t *r = ctx->r;
    for (;;) {
        if (atomic_load(&r->cancel_requested) || atomic_load(ctx->stop_workers) || now_ms() >= ctx->deadline)
            return NULL;
        uint32_t peer_index;
        pthread_mutex_lock(ctx->pick_mutex);
        if (*ctx->resolved || *ctx->next_peer >= ctx->round_end) {
            pthread_mutex_unlock(ctx->pick_mutex);
            return NULL;
        }
        peer_index = (*ctx->next_peer)++;
        pthread_mutex_unlock(ctx->pick_mutex);

        const uint8_t *compact = ctx->peers->peers[peer_index].b;
        peer_attempt_t attempt;
        uint8_t *metadata = NULL;
        size_t metadata_len = 0;
        int fetched = fetch_metadata_from_peer(r, compact, ctx->spec, ctx->peer_id, peer_index,
                                               ctx->round_end, ctx->deadline, ctx->stop_workers,
                                               &metadata, &metadata_len, &attempt);
        if (attempt.connected) atomic_fetch_add(ctx->reached_peers, 1);
        if (attempt.handshake_verified)
            peer_set_append_unique(ctx->verified, compact, 1);
        if (!fetched) continue;

        uint8_t *torrent_probe = NULL;
        size_t torrent_len = 0;
        char probe_error[MAGNET_ERROR_MAX];
        int built = magnet_build_torrent(ctx->spec, metadata, metadata_len, &torrent_probe, &torrent_len, probe_error);
        if (!built) {
            torrent_debug_log("[magnet] peer %u/%u metadata rejected: %s", peer_index+1, ctx->round_end, probe_error);
            free(metadata);
            continue;
        }
        free(torrent_probe); // just a validity probe; the winner is rebuilt for real once

        pthread_mutex_lock(ctx->pick_mutex);
        if (!*ctx->resolved) {
            *ctx->out_metadata = metadata;
            *ctx->out_metadata_len = metadata_len;
            *ctx->resolved = 1;
            atomic_store(ctx->stop_workers, 1);
            metadata = NULL; // ownership transferred
        }
        pthread_mutex_unlock(ctx->pick_mutex);
        free(metadata); // NULL if transferred, otherwise this lost the race
        return NULL;
    }
}

static void magnet_resolve_run(magnet_resolve_t *r) {
    magnet_spec_t spec;
    if (!magnet_parse(r->uri, &spec, r->error)) {
        r->ok = 0;
        return;
    }

    uint8_t peer_id[20];
    memcpy(peer_id, "-FS0001-", 8);
    rand_bytes(peer_id + 8, 12);
    report_progress(r, MAGNET_STAGE_FINDING_PEERS, 0, 0, 0, 0);

    magnet_dht_search_t dht_search;
    memset(&dht_search, 0, sizeof(dht_search));
    pthread_mutex_init(&dht_search.set.mutex, NULL);
    atomic_int dht_stop = 0;
    dht_search_ctx_t dht_ctx = { spec.info_hash, &r->cancel_requested, &dht_stop, &dht_search };
    pthread_t dht_thread;
    int dht_thread_started = (pthread_create(&dht_thread, NULL, run_dht_search, &dht_ctx) == 0);
    int dht_joined = 0;

    magnet_peer_set_t peers;
    memset(&peers, 0, sizeof(peers));
    pthread_mutex_init(&peers.mutex, NULL);

    char trackers[MAGNET_MAX_TRACKERS][512];
    uint32_t tracker_count = build_tracker_candidates(spec.tracker_url, trackers);

    char first_failure[128] = "";
    int saw_tracker_failure = 0, saw_not_registered = 0;
    for (uint32_t i = 0; i < tracker_count; i++) {
        if (atomic_load(&r->cancel_requested)) break;
        uint8_t batch[MAGNET_MAX_PEERS_PER_TRACKER * 6];
        tracker_announce_result_t result;
        memset(&result, 0, sizeof(result));
        uint32_t count = tracker_announce_url_ex_cancel(trackers[i], spec.info_hash, peer_id, 6881, 0, 0,
                                                         batch, MAGNET_MAX_PEERS_PER_TRACKER, &result,
                                                         magnet_tracker_cancelled, &r->cancel_requested);
        if (atomic_load(&r->cancel_requested)) break;
        if (result.tracker_failure) {
            saw_tracker_failure = 1;
            if (!first_failure[0]) snprintf(first_failure, sizeof(first_failure), "%s", result.failure_reason);
            if (strstr_ci(result.failure_reason, "not registered")) saw_not_registered = 1;
            torrent_debug_log("[magnet] tracker %s rejected hash: %s", trackers[i], result.failure_reason);
            if (saw_not_registered) break;
        }
        peer_set_append_unique(&peers, batch, count);
        if (peer_set_count(&peers) >= MAGNET_MAX_MERGED_PEERS) break;
    }

    if (saw_not_registered) atomic_store(&dht_stop, 1);
    if (peer_set_count(&peers) == 0 || saw_not_registered) {
        if (dht_thread_started) { pthread_join(dht_thread, NULL); dht_joined = 1; }
    }
    merge_peer_set(&peers, &dht_search.set);
    uint32_t peer_count = peer_set_count(&peers);
    if (!peer_count) {
        if (saw_not_registered)
            snprintf(r->error, MAGNET_ERROR_MAX,
                    "RuTracker says this torrent is not registered anymore. The catalog entry is stale.");
        else if (saw_tracker_failure && first_failure[0])
            snprintf(r->error, MAGNET_ERROR_MAX, "RuTracker rejected this torrent: %s", first_failure);
        else
            snprintf(r->error, MAGNET_ERROR_MAX, "RuTracker trackers and the DHT returned no usable peers.");
        r->ok = 0;
        goto cleanup;
    }

    // PEX amplification: a blocked-tracker/DHT-only resolve can surface
    // just 1-3 peers. Before committing the metadata-fetch workers, ask
    // those few peers for their swarm view and grow the set.
    if (peer_count <= MAGNET_PEX_THIN_THRESHOLD && !atomic_load(&r->cancel_requested)) {
        magnet_peer_set_t pex_peers;
        memset(&pex_peers, 0, sizeof(pex_peers));
        pthread_mutex_init(&pex_peers.mutex, NULL);
        for (uint32_t i = 0; i < peer_count && !atomic_load(&r->cancel_requested); i++)
            harvest_pex_from_peer(peers.peers[i].b, &spec, peer_id, &r->cancel_requested, &pex_peers);
        uint32_t added = merge_peer_set(&peers, &pex_peers);
        if (added) torrent_debug_log("[magnet] pex added %u peers, total=%u", added, peer_set_count(&peers));
        pthread_mutex_destroy(&pex_peers.mutex);
        peer_count = peer_set_count(&peers);
    }

    {
        uint64_t deadline = now_ms() + MAGNET_OVERALL_TIMEOUT_MS;
        pthread_mutex_t pick_mutex;
        pthread_mutex_init(&pick_mutex, NULL);
        uint32_t next_peer = 0;
        int resolved = 0;
        atomic_int stop_workers = 0;
        atomic_uint reached_peers = 0;
        uint8_t *metadata = NULL;
        size_t metadata_len = 0;
        magnet_peer_set_t verified;
        memset(&verified, 0, sizeof(verified));
        pthread_mutex_init(&verified.mutex, NULL);

        int empty_reannounces = 0;
        while (!atomic_load(&r->cancel_requested) && !resolved && now_ms() < deadline) {
            uint32_t round_end = peer_count;
            if (next_peer >= round_end) {
                // Every known peer was tried once without metadata. Back
                // off briefly, pull a rotated peer set plus what the DHT
                // has found meanwhile, and keep trying until the deadline.
                uint64_t backoff_until = now_ms() + MAGNET_REANNOUNCE_BACKOFF_MS;
                while (!atomic_load(&r->cancel_requested) && now_ms() < backoff_until && now_ms() < deadline)
                    usleep(100000);
                if (atomic_load(&r->cancel_requested) || now_ms() >= deadline) break;
                uint32_t added = merge_peer_set(&peers, &dht_search.set);
                if (empty_reannounces < MAGNET_MAX_EMPTY_REANNOUNCES) {
                    for (uint32_t i = 0; i < tracker_count; i++) {
                        if (atomic_load(&r->cancel_requested) || now_ms() >= deadline ||
                            peer_set_count(&peers) >= MAGNET_MAX_MERGED_PEERS)
                            break;
                        uint8_t batch[MAGNET_MAX_PEERS_PER_TRACKER * 6];
                        tracker_announce_result_t result;
                        memset(&result, 0, sizeof(result));
                        uint32_t count = tracker_announce_url_ex_cancel(
                            trackers[i], spec.info_hash, peer_id, 6881, 0, 0,
                            batch, MAGNET_MAX_PEERS_PER_TRACKER, &result,
                            magnet_tracker_cancelled, &r->cancel_requested);
                        if (atomic_load(&r->cancel_requested)) break;
                        added += peer_set_append_unique(&peers, batch, count);
                    }
                }
                peer_count = peer_set_count(&peers);
                if (added) {
                    empty_reannounces = 0;
                } else {
                    // Nothing new anywhere: sweep the peers we already know
                    // again instead of giving up - one that dropped our SYN
                    // or was mid-disconnect can answer next time.
                    empty_reannounces++;
                    next_peer = 0;
                }
                continue;
            }

            uint32_t pending = round_end - next_peer;
            uint32_t worker_count = pending < MAGNET_MAX_CONCURRENT_PEERS ? pending : MAGNET_MAX_CONCURRENT_PEERS;
            pthread_t workers[MAGNET_MAX_CONCURRENT_PEERS];
            peer_worker_ctx_t ctx;
            ctx.r = r; ctx.peers = &peers; ctx.spec = &spec; ctx.peer_id = peer_id;
            ctx.round_end = round_end; ctx.deadline = deadline; ctx.stop_workers = &stop_workers;
            ctx.pick_mutex = &pick_mutex; ctx.next_peer = &next_peer; ctx.resolved = &resolved;
            ctx.out_metadata = &metadata; ctx.out_metadata_len = &metadata_len;
            ctx.verified = &verified; ctx.reached_peers = &reached_peers;
            for (uint32_t i = 0; i < worker_count; i++)
                pthread_create(&workers[i], NULL, peer_worker_fn, &ctx);
            for (uint32_t i = 0; i < worker_count; i++)
                pthread_join(workers[i], NULL);
        }

        if (atomic_load(&r->cancel_requested) && !resolved) {
            snprintf(r->error, MAGNET_ERROR_MAX, "Metadata resolution was cancelled.");
            r->ok = 0;
        } else if (!metadata) {
            if (atomic_load(&reached_peers) == 0)
                snprintf(r->error, MAGNET_ERROR_MAX,
                    "Found %u peers but could not connect to any of them. This network appears to "
                    "block BitTorrent - try another Wi-Fi or a phone hotspot.", peer_count);
            else
                snprintf(r->error, MAGNET_ERROR_MAX,
                    "Peers were found, but none returned torrent metadata. Try this catalog item again later.");
            r->ok = 0;
        } else {
            report_progress(r, MAGNET_STAGE_VALIDATING, 1, 1, 0, 0);
            uint8_t *torrent = NULL;
            size_t torrent_len = 0;
            if (magnet_build_torrent(&spec, metadata, metadata_len, &torrent, &torrent_len, r->error)) {
                r->ok = write_torrent_atomic(r->out_path, torrent, torrent_len, r->error);
                free(torrent);
            } else {
                r->ok = 0;
            }
        }
        free(metadata);
        pthread_mutex_destroy(&pick_mutex);
        pthread_mutex_destroy(&verified.mutex);
    }

cleanup:
    atomic_store(&dht_stop, 1);
    if (dht_thread_started && !dht_joined)
        pthread_join(dht_thread, NULL);
    pthread_mutex_destroy(&dht_search.set.mutex);
    pthread_mutex_destroy(&peers.mutex);
}

// ---- public async wrapper ----
static void *resolve_thread_main(void *arg) {
    magnet_resolve_t *r = (magnet_resolve_t*)arg;
    magnet_resolve_run(r);
    atomic_store(&r->done, 1);
    return NULL;
}

magnet_resolve_t *magnet_resolve_start(const char *uri, const char *out_path) {
    magnet_resolve_t *r = (magnet_resolve_t*)calloc(1, sizeof(*r));
    if (!r) return NULL;
    snprintf(r->uri, sizeof(r->uri), "%s", uri);
    snprintf(r->out_path, sizeof(r->out_path), "%s", out_path);
    pthread_mutex_init(&r->progress_mutex, NULL);
    if (pthread_create(&r->thread, NULL, resolve_thread_main, r) != 0) {
        pthread_mutex_destroy(&r->progress_mutex);
        free(r);
        return NULL;
    }
    return r;
}

int magnet_resolve_done(magnet_resolve_t *r) {
    return r ? atomic_load(&r->done) : 1;
}

void magnet_resolve_progress(const magnet_resolve_t *r, magnet_progress_t *out) {
    if (!r || !out) return;
    magnet_resolve_t *mutable_r = (magnet_resolve_t*)r;
    pthread_mutex_lock(&mutable_r->progress_mutex);
    *out = mutable_r->progress;
    pthread_mutex_unlock(&mutable_r->progress_mutex);
}

int magnet_resolve_ok(const magnet_resolve_t *r) {
    return r ? r->ok : 0;
}

const char *magnet_resolve_error(const magnet_resolve_t *r) {
    return r ? r->error : "";
}

void magnet_resolve_cancel(magnet_resolve_t *r) {
    if (r) atomic_store(&r->cancel_requested, 1);
}

void magnet_resolve_free(magnet_resolve_t *r) {
    if (!r) return;
    pthread_join(r->thread, NULL);
    pthread_mutex_destroy(&r->progress_mutex);
    free(r);
}
