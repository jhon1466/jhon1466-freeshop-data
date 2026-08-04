#pragma once
#include "install.h"
#include "install_common.h"

#include <switch.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Installs one NSZ-compressed content piece (a ".ncz" PFS0 entry) straight
// from the network into NCM content storage - the compressed/decompress
// counterpart to ncm_install_content_from_url() in ncm_install.h, which
// handles a plain ".nca" entry the same way. See ncz.c for the full NCZ
// format writeup (checked against nsz's own reference decompressor).
//
// `compressed_size` is the .ncz entry's own size as recorded in the PFS0
// (how many bytes to fetch over the network). `final_size` is the real,
// decompressed NCA size this content piece must end up as, from the CNMT's
// content_infos[i].size (see ncm_read_content_meta) - NOT the PFS0 size,
// which only knows the compressed size.
bool ncm_install_ncz_content_from_url(NcmContentStorage *cs, const NcmContentId *content_id,
                                       ResolvedUrl *ru, uint64_t entry_offset,
                                       uint64_t compressed_size, uint64_t final_size,
                                       InstallProgressCallback cb, void *userdata,
                                       bool *out_registered,
                                       char *err_buf, size_t err_buf_size);

// Local-file counterpart of the above - same contract, same arguments,
// but reads the .ncz entry's compressed bytes out of an already-on-SD
// container (a file the user copied over themselves, or one this app
// downloaded via BitTorrent - see install_torrent.c) instead of pulling
// them over HTTP. Needed because install_local.c's plain-.nca path
// (ncm_install_content) can't handle a compressed entry at all: an NSZ
// stores content the CNMT names "<id>.nca" as "<id>.ncz", so without this
// every NSZ install failed with "el NSP no incluye <id>.nca".
//
// `compressed_size` is the .ncz entry's own size as recorded in the PFS0;
// `final_size` is the real decompressed NCA size from the CNMT. Progress
// is reported in decompressed bytes (scaled from how much of the
// compressed entry has been read), matching what the .nca path reports so
// a mixed NSZ's overall bar stays consistent.
// `gate` (may be NULL) blocks each chunk read until those bytes have
// arrived, for a source still being written - see install_common.h's
// InstallReadGate. It closes/reopens *src, which is why this takes FILE**.
bool ncm_install_ncz_content_from_file(NcmContentStorage *cs, const NcmContentId *content_id,
                                        FILE **src, uint64_t entry_offset,
                                        uint64_t compressed_size, uint64_t final_size,
                                        const InstallReadGate *gate,
                                        InstallProgressCallback cb, void *userdata,
                                        bool *out_registered,
                                        char *err_buf, size_t err_buf_size);

// Push counterpart of the above - for a source that hands over an .ncz
// entry's bytes as they arrive (install_stream.c, for an NSZ/XCZ pushed
// over MTP) instead of one this can pull with HTTP range requests. Same
// decompress -> re-encrypt -> verify -> register pipeline underneath;
// only how the compressed bytes get in differs.
typedef struct NczPushCtx NczPushCtx;

// Begins decompressing one .ncz entry into a fresh NCM placeholder sized to
// `final_size` (the real, decompressed NCA size - the caller has to know
// this upfront, same as ncm_install_ncz_content_from_url does; for a
// caller that only has the container's own on-disk/on-wire byte range to
// go on, that means reading it from the entry's CNMT, which - unlike
// compressed_size - isn't recoverable from the .ncz entry itself).
// `compressed_size` is the .ncz entry's own (compressed) size. Returns
// NULL with a reason in err_buf on failure.
NczPushCtx *ncz_push_begin(NcmContentStorage *cs, const NcmContentId *content_id,
                            uint64_t compressed_size, uint64_t final_size,
                            char *err_buf, size_t err_buf_size);

// Feeds the next consecutive chunk of the .ncz entry's compressed bytes.
// Returns false on the first error (a reason was already left in the
// err_buf passed to ncz_push_begin) - the context must then be abandoned
// with ncz_push_abort.
bool ncz_push_feed(NczPushCtx *ctx, const uint8_t *data, size_t len);

// Completes: flushes remaining output, verifies the decompressed size and
// SHA-256 hash against `content_id`, and registers the content. Frees
// `ctx` either way. Returns false (deleting the placeholder instead of
// registering it) with a reason in err_buf on any mismatch/error.
bool ncz_push_finish(NczPushCtx *ctx, char *err_buf, size_t err_buf_size);

// Abandons a context (feed failed, or the transfer was canceled/aborted),
// deleting its placeholder. Frees `ctx`. NULL-safe.
void ncz_push_abort(NczPushCtx *ctx);
