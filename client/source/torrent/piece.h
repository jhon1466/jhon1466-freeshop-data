#pragma once
// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/core/piece.c/.h, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
//
// Piece bookkeeping: which blocks/pieces are done, request-count tracking
// so peer.c's picker doesn't over-request the same block, SHA-1 piece
// verification, and dispatch into torrent_storage.c on completion.
//
// Trimmed from the original: pipensx hashes completed pieces on a
// background pthread (an async ring buffer feeding a worker thread) to keep
// the network loop from stalling during a hash. This client hashes inline
// instead - it already relies on hardware SHA-1 (torrent_sha1.h, ARMv8
// crypto extensions) so a piece hash costs low single-digit milliseconds,
// and every other transport here (mtp_step/ftp_step) already does its own
// inline work per poll tick, so this matches the rest of the codebase
// instead of introducing the only real OS thread in the client.
#include <stdint.h>
#include <stddef.h>
#include "metainfo.h"
#include "torrent_storage.h"

#define BLOCK_SIZE  (16*1024)  // 16 KB - standard BitTorrent block
// Completed pieces hand their buffer back to a small recycle pool instead of
// free(): piece buffers are piece_length (typically MiBs) and churn at piece
// rate, so pooling avoids a large alloc+zero per piece and heap
// fragmentation from the constant same-size malloc/free.
#define PIECE_BUF_POOL_MAX 4

typedef enum {
    PS_EMPTY    = 0,
    PS_PENDING  = 1,  // requested from at least one peer
    PS_DONE     = 2,  // verified and written
} piece_state_t;

typedef struct {
    piece_state_t state;
    uint8_t      *buf;         // piece_length bytes, NULL until first request
    uint8_t      *have_blocks; // bitmap of received 16KB blocks
    uint8_t      *request_counts; // number of peers requesting each block
    uint32_t      num_blocks;
    uint32_t      num_blocks_done;
} piece_slot_t;

typedef struct {
    const metainfo_t *mi;
    storage_t        *store;
    piece_slot_t     *slots;  // [num_pieces]
    uint32_t          num_done;
    uint32_t          num_pieces;
    uint64_t          completed_bytes;
    uint8_t          *have_bf; // bitfield of completed pieces (num_pieces bits)
    uint8_t          *available_bf; // pieces retained on disk for upload
    int               strict_order;
    uint32_t          strict_order_lookahead;
    int               strict_fill_pending_first;
    uint32_t         *piece_order;
    uint32_t          piece_order_count;
    uint8_t          *verify_buf; // piece_length scratch, lazily allocated
    uint8_t          *buf_pool[PIECE_BUF_POOL_MAX]; // recycled piece buffers
    uint32_t          buf_pool_count;
    // Position in piece_order (or the identity order) of the first piece that
    // is not DONE. Advanced when pieces complete, rewound by reset_piece, so
    // the strict picker and head-piece queries start at the download frontier
    // instead of rescanning every completed piece.
    uint32_t          order_cursor;
    uint32_t         *order_pos;  // piece index -> piece_order position
    int             (*request_allowed)(void *user, uint32_t piece);
    void             *request_allowed_user;
    // When set, piece_mgr_check_existing skips the read+hash for any piece
    // that isn't fully in a skipped storage range - it just reports "not
    // present" without touching disk. For a download that's known to be
    // starting fresh (see torrent_options_t's own field), reading and
    // hashing every piece of a multi-GB target file before the first block
    // is ever requested is real, user-visible time spent proving something
    // already known: a freshly-opened DISK file is all zeros, which will
    // never pass a hash check anyway. Skipped-range detection itself stays
    // on (storage_range_skipped costs no I/O), since that's what makes a
    // SKIP-mode file (the other pieces of a multi-file torrent - see
    // install_torrent.c) not get downloaded at all.
    int               skip_disk_verify;
    // Piece range a blocked consumer is waiting on (see
    // piece_mgr_set_priority). Picked ahead of the normal order.
    uint32_t          priority_first;
    uint32_t          priority_last;
    int               priority_active;
} piece_mgr_t;

piece_mgr_t *piece_mgr_create(const metainfo_t *mi, storage_t *store);
piece_mgr_t *piece_mgr_create_ex(const metainfo_t *mi, storage_t *store,
                                 int strict_order,
                                 const uint32_t *piece_order,
                                 uint32_t piece_order_count);
void         piece_mgr_destroy(piece_mgr_t *pm);
void         piece_mgr_set_strict_policy(piece_mgr_t *pm,
                                         uint32_t lookahead,
                                         int fill_pending_first);

// Marks a block as requested by a peer (sets PS_PENDING).
void piece_mgr_mark_pending(piece_mgr_t *pm, uint32_t idx);

// Receives a block. Returns:
//   2 = piece complete, verified and written
//   1 = block stored, piece not yet complete
//   0 = piece complete but hash mismatch (piece reset)
//  -1 = error (bad params, alloc/write failure)
int piece_mgr_got_block(piece_mgr_t *pm, uint32_t idx, uint32_t offset,
                        const uint8_t *data, uint32_t len);

// Verifies one completed piece by reading it back from storage.
int piece_mgr_verify_piece(piece_mgr_t *pm, uint32_t idx);

// Checks a piece already present in storage and updates the have bitfield.
// Returns 1 when valid, 0 when absent/corrupt, and -1 on invalid arguments.
int piece_mgr_check_existing(piece_mgr_t *pm, uint32_t idx);

// Fast resume: marks the pieces set in a previously saved have-bitfield as
// DONE without hashing them. bf_len must be exactly (num_pieces+7)/8 or the
// call is a no-op.
void piece_mgr_preset_have(piece_mgr_t *pm, const uint8_t *bf,
                           uint32_t bf_len);

// Verifies every completed piece from storage. Corrupt pieces are reset for
// downloading again. Returns 1 when all pieces verify, 0 otherwise.
int piece_mgr_verify_all(piece_mgr_t *pm);

// Returns non-zero when the block has already been received.
int piece_mgr_has_block(const piece_mgr_t *pm, uint32_t idx, uint32_t block);
int piece_mgr_block_requested(const piece_mgr_t *pm, uint32_t idx,
                              uint32_t block);
uint32_t piece_mgr_block_request_count(const piece_mgr_t *pm, uint32_t idx,
                                       uint32_t block);
void piece_mgr_mark_block_requested(piece_mgr_t *pm, uint32_t idx,
                                    uint32_t block);
void piece_mgr_clear_block_requested(piece_mgr_t *pm, uint32_t idx,
                                     uint32_t block);
void piece_mgr_clear_all_block_requests(piece_mgr_t *pm, uint32_t idx,
                                        uint32_t block);

// Marks [first, last] as the range a consumer is currently blocked on:
// piece_mgr_pick hands those out before anything else. Pass first > last
// to clear.
//
// Without this, a consumer reading the download as it arrives (see
// install_local.h's InstallLocalGate) is at the mercy of where the
// container's packer happened to put things - an NSP whose .cnmt.nca sits
// in the last few KB of the file forces the entire torrent to download
// before the install can begin, which is exactly the "download first,
// install after" behavior the overlap is meant to remove. Peers that hold
// none of the priority pieces still fall through to the normal order, so
// this narrows what gets requested first without idling the swarm.
void piece_mgr_set_priority(piece_mgr_t *pm, uint32_t first, uint32_t last);

// Picks the next piece index to request from a peer.
// peer_bf = peer's have-bitfield (same format as have_bf).
// Returns piece index or (uint32_t)-1 if nothing to request.
uint32_t piece_mgr_pick(const piece_mgr_t *pm,
                        const uint8_t *peer_bf, uint32_t bf_bytes);

// First piece in download order that is not yet DONE, or UINT32_MAX when the
// torrent is complete. O(1) thanks to the maintained order cursor.
uint32_t piece_mgr_head_piece(const piece_mgr_t *pm);

// Size of the last (possibly short) piece.
int64_t piece_len(const piece_mgr_t *pm, uint32_t idx);

// Number of 16KB blocks in a piece.
uint32_t piece_num_blocks(const piece_mgr_t *pm, uint32_t idx);
