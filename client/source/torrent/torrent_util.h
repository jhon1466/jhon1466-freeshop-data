#pragma once
// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/core/util.c/.h, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
//
// Trimmed to what the torrent engine itself actually needs - pipensx's own
// util.c also carries its app's logging/telemetry framework, which this
// doesn't port: this project already has an equivalent debug-log
// convention (net/http.h's download_debug_log) to follow instead, and
// byte/speed formatting for display belongs in ui_torrent.c (matching how
// ui_mtp.c/ui_ftp.c format their own progress text, not something the
// protocol layer does).
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// Monotonic time - since console boot, not wall-clock. Every timeout/rate
// calculation in this engine only ever compares two of these, so what
// epoch it counts from never matters.
uint64_t now_ms(void);
uint64_t now_us(void);
time_t now_sec(void); // for the DHT's relative expiry timers

// Hex-encodes a 20-byte hash (an info hash or peer/node id) into buf[41].
void hex20(char buf[41], const uint8_t hash[20]);

// Cryptographically random bytes - peer ids, DHT node ids, transaction ids.
void rand_bytes(uint8_t *buf, size_t n);

// Bitfield helpers (piece "have" bitfields: MSB-first, per the BitTorrent spec).
static inline int bf_has(const uint8_t *bf, uint32_t idx) {
    return (bf[idx / 8] >> (7 - idx % 8)) & 1;
}
static inline void bf_set(uint8_t *bf, uint32_t idx) {
    bf[idx / 8] |= (1u << (7 - idx % 8));
}
