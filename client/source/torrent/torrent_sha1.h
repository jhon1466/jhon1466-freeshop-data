#pragma once
// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/core/sha1.c/.h, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
//
// SHA-1 is used to verify every downloaded piece against the torrent's
// piece hash list - for a multi-GB game, that's a lot of hashing, so this
// wraps libnx's own hardware-accelerated implementation (ARMv8 Crypto
// Extensions) rather than a software one, matching pipensx's own choice
// here specifically (its otherwise-portable core has a software SHA-1
// fallback for non-Switch builds, which this - a Switch-only client -
// never needs).
#include <switch/crypto/sha1.h>
#include <stddef.h>
#include <stdint.h>

typedef Sha1Context sha1_ctx_t;

void sha1_init(sha1_ctx_t *ctx);
void sha1_update(sha1_ctx_t *ctx, const void *data, size_t len);
void sha1_final(sha1_ctx_t *ctx, uint8_t digest[20]);

// One-shot helper.
void sha1(const void *data, size_t len, uint8_t digest[20]);
