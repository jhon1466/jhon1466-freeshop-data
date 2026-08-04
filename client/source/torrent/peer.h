#pragma once
// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/core/peer.c/.h, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
//
// BEP-3 peer wire protocol + BEP-10 extension protocol (ut_pex) responder.
// Non-blocking, single-owner state machine: the caller (torrent.c) drives
// peer_recv()/peer_process() from its own poll loop, same shape as every
// other transport in this client (mtp_step/ftp_step).
//
// Trimmed from the original: pipensx also carries a μTP transport
// (TRANSPORT_UTP, backed by vendored libutp, a C++ library) as a fallback
// for TCP-unreachable peers. That is not ported here - vendoring libutp is
// a substantial separate effort - so this responder is TCP-only for now.
#include "net.h"
#include "mse.h"
#include <stdint.h>
#include <stddef.h>

#define MAX_PIPELINE   256     // one 4 MiB piece in flight per peer
#define MAX_PEERS     96
#define BT_HANDSHAKE_LEN 68
#define PEER_BUF_SIZE (4 + 1 + (1<<14) + 9)  // enough for one piece msg
#define PEER_RECV_BUFFER_SIZE ((256 * 1024) + 4) // max payload + length

// BT message IDs
#define MSG_CHOKE        0
#define MSG_UNCHOKE      1
#define MSG_INTERESTED   2
#define MSG_NOT_INTEREST 3
#define MSG_HAVE         4
#define MSG_BITFIELD     5
#define MSG_REQUEST      6
#define MSG_PIECE        7
#define MSG_CANCEL       8
#define MSG_PORT         9
#define MSG_EXTENDED    20

// BEP10 extension IDs (local mapping)
#define EXT_HANDSHAKE_ID  0
#define EXT_PEX_ID        1   // we assign this to ut_pex

typedef enum {
    PS_CONNECTING = 0,
    PS_MSE,        // MSE/PE encryption handshake in progress
    PS_HANDSHAKE,
    PS_EXTENSION,
    PS_ACTIVE,
    PS_DEAD
} peer_state_t;

typedef struct {
    int          index;   // piece
    int          offset;  // byte offset within piece
    int          length;  // block size
    uint64_t     requested_ms;
} block_req_t;

typedef struct peer {
    socket_t     fd;
    peer_state_t state;

    struct sockaddr_in addr;

    // Send/recv buffers. rbuf is a linear buffer with a read cursor: valid
    // unprocessed bytes are rbuf[rbuf_head .. rbuf_len). Consuming a message
    // advances rbuf_head instead of memmoving the tail to the front, so the
    // common case costs no copy. The tail is compacted to the front only when
    // it runs out of room (see peer_recv).
    // Heap-allocated (PEER_RECV_BUFFER_SIZE) on first receive rather than
    // embedded: at 256 KiB x MAX_PEERS slots, embedding would cost ~24 MiB
    // up front and re-zero it on every dial, including the many that fail.
    uint8_t *rbuf;
    uint32_t rbuf_head;
    uint32_t rbuf_len;
    uint8_t  sbuf[PEER_BUF_SIZE];
    uint32_t sbuf_len;

    // BT state
    int      am_choked;
    int      am_interested;
    int      peer_choked;
    int      peer_interested;

    uint8_t *bitfield;
    uint32_t bf_bytes;

    // Pending requests
    block_req_t pipeline[MAX_PIPELINE];
    int         pipeline_len;

    // BEP10 extensions
    int      ext_handshake_sent;
    uint8_t  peer_ext_pex;    // peer's ut_pex extension ID
    int      supports_ext;    // peer set extension bit

    uint64_t connect_time_ms;
    uint64_t last_recv_ms;
    uint64_t last_piece_ms;
    uint64_t downloaded;

    // Request scheduler health (single-owner torrent loop).
    uint64_t request_cooldown_until_ms;
    // Download-rate estimate driving the adaptive request pipeline.
    // dl_rate_bps is an EMA of bytes/sec sampled once per second by the
    // torrent loop from the cumulative `downloaded` counter;
    // rate_last_downloaded is the previous sample's snapshot.
    uint64_t dl_rate_bps;
    uint64_t rate_last_downloaded;
    // EMA of block round-trip latency (request sent -> piece received), ms.
    // 0 until the first block arrives; never returns to 0 afterwards.
    uint32_t block_lat_ema_ms;
    uint64_t telemetry_piece_bytes;
    uint32_t timeout_strikes;
    // Last time any of this peer's requests expired.
    uint64_t last_expiry_ms;
    uint32_t telemetry_expired_requests;
    uint32_t telemetry_hedged_requests;
    uint32_t telemetry_cancelled_requests;
    uint32_t telemetry_released_requests;

    // PEX data received (raw bencode, owned)
    uint8_t *pex_buf;
    uint32_t pex_len;

    // MSE/PE encryption. mse_enabled: attempt the encrypted handshake on
    // connect. mse_active: handshake done, wrap all traffic in RC4.
    int          mse_enabled;
    int          mse_active;
    mse_client_t mse;
} peer_t;

typedef struct {
    const uint8_t *info_hash;
    const uint8_t *peer_id;
    uint32_t       num_pieces;
    uint32_t       bf_bytes;   // (num_pieces+7)/8
    const uint8_t *our_bf;     // our have-bitfield
    uint16_t       listen_port;
    int            use_mse;    // attempt MSE/PE on outgoing connections
} peer_ctx_t;

// Allocate/free a peer slot around an already-connecting TCP socket.
peer_t *peer_create(socket_t fd, struct sockaddr_in addr,
                    const peer_ctx_t *ctx);
void    peer_destroy(peer_t *p);

// Transport connect completed: advance PS_CONNECTING -> PS_HANDSHAKE (or
// PS_MSE) and send our handshake. Returns 0 on send failure (peer is dead).
int peer_connected(peer_t *p, const peer_ctx_t *ctx);

// callback signature bundle reused by peer_recv / peer_process
typedef void (*peer_on_block_fn)(void *ud, uint32_t idx, uint32_t off,
                                 const uint8_t *data, uint32_t len);
typedef void (*peer_on_have_fn)(void *ud, uint32_t idx);
typedef void (*peer_on_peers_fn)(void *ud, const uint8_t *compact, uint32_t cnt);

// Called when a TCP socket is readable: drain the socket and dispatch
// messages. Returns 0 = ok, -1 = peer dead (close & destroy).
int peer_recv(peer_t *p, const peer_ctx_t *ctx,
              peer_on_block_fn on_block, peer_on_have_fn on_have,
              peer_on_peers_fn on_peers, void *ud);

// Runs the receive state machine over bytes already sitting in rbuf
// (transport-agnostic core of peer_recv). Returns 0 ok, -1 dead.
int peer_process(peer_t *p, const peer_ctx_t *ctx,
                 peer_on_block_fn on_block, peer_on_have_fn on_have,
                 peer_on_peers_fn on_peers, void *ud);

// Free space remaining in rbuf.
uint32_t peer_rbuf_space(const peer_t *p);

// Appends bytes into rbuf, compacting as needed. Returns 0 on success, -1 if
// the receive buffer is full (peer must be dropped).
int peer_rbuf_append(peer_t *p, const uint8_t *data, uint32_t len);

// Sends handshake immediately after connect. Returns 0 on failure.
int peer_send_handshake(peer_t *p, const peer_ctx_t *ctx);

// Queues a block request. Returns 0 if pipeline full.
int peer_request_block(peer_t *p, uint32_t piece, uint32_t offset, uint32_t len);
int peer_cancel_block(peer_t *p, uint32_t piece, uint32_t offset, uint32_t len);

// Drops requests a peer has not answered before the deadline.
int peer_expire_requests(peer_t *p, uint64_t now, uint64_t timeout_ms,
                         void (*on_expired)(void*, const block_req_t*),
                         void *ud);

// Flushes any queued sends.
int peer_flush(peer_t *p);

// Sends bitfield.
int peer_send_bitfield(peer_t *p, const uint8_t *bf, uint32_t bf_bytes);

// Sends interested.
int peer_send_interested(peer_t *p);

// Sends BEP10 extension handshake.
int peer_send_ext_handshake(peer_t *p, uint16_t listen_port);

// Sends ut_pex (BEP11) with an added-peers list.
int peer_send_pex(peer_t *p, const uint8_t *compact6, uint32_t cnt);
