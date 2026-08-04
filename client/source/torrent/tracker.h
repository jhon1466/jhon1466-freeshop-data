#pragma once
// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/core/tracker.c/.h, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
#include "metainfo.h"
#include <stdint.h>
#include <stddef.h>

typedef int (*tracker_cancel_cb)(void *user);

// Announces to every tracker in `mi` and collects a compact peer list.
// Blocking, with a short per-tracker timeout - called from torrent_step(),
// so this is only ever invoked from a state where blocking briefly is fine
// (matching how mtp_ptp.c/ftp_server.c's own blocking-during-a-transfer
// calls work), not from the middle of servicing many peers at once.
// Returns the number of peers found; fills compact_out (6 bytes each:
// 4-byte IP + 2-byte port, big-endian - BitTorrent's "compact" peer format).
// `max_peers` bounds how many entries fit in compact_out.
//
// `cancel` is polled before each tracker and during each transfer, so
// tearing down mid-announce doesn't have to wait out every tracker in the
// list. May be NULL.
uint32_t tracker_announce(const metainfo_t *mi, const uint8_t *peer_id, uint16_t listen_port,
                           int64_t downloaded, int64_t left, uint8_t *compact_out, uint32_t max_peers,
                           tracker_cancel_cb cancel, void *cancel_user);

// Announces a bare info hash to one tracker - used while resolving a
// magnet, before a metainfo dict (and so a real metainfo_t) exists.
uint32_t tracker_announce_url(const char *url, const uint8_t *info_hash, const uint8_t *peer_id,
                               uint16_t listen_port, int64_t downloaded, int64_t left, uint8_t *compact_out,
                               uint32_t max_peers);

typedef struct tracker_announce_result {
    uint32_t peers;
    int request_ok;
    int tracker_failure;
    char failure_reason[128];
} tracker_announce_result_t;

uint32_t tracker_announce_url_ex(const char *url, const uint8_t *info_hash, const uint8_t *peer_id,
                                  uint16_t listen_port, int64_t downloaded, int64_t left, uint8_t *compact_out,
                                  uint32_t max_peers, tracker_announce_result_t *result);

uint32_t tracker_announce_url_ex_cancel(const char *url, const uint8_t *info_hash, const uint8_t *peer_id,
                                         uint16_t listen_port, int64_t downloaded, int64_t left,
                                         uint8_t *compact_out, uint32_t max_peers,
                                         tracker_announce_result_t *result, tracker_cancel_cb cancel,
                                         void *cancel_user);
