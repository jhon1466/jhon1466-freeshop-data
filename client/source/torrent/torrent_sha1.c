// Adapted from pipensx (https://github.com/i3sey/pipensx,
// src/core/sha1.c, GPL-3.0) - see ../../THIRD_PARTY_NOTICES.md.
#include "torrent_sha1.h"

void sha1_init(sha1_ctx_t *ctx) {
    sha1ContextCreate(ctx);
}

void sha1_update(sha1_ctx_t *ctx, const void *data, size_t len) {
    sha1ContextUpdate(ctx, data, len);
}

void sha1_final(sha1_ctx_t *ctx, uint8_t digest[20]) {
    sha1ContextGetHash(ctx, digest);
}

void sha1(const void *data, size_t len, uint8_t digest[20]) {
    sha1CalculateHash(digest, data, len);
}
