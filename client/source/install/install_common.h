#pragma once
#include "install.h"
#include "../net/http.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Creates dir if missing. Real failures (e.g. read-only SD, invalid path)
// surface later when the caller tries to open a file inside this directory
// and fails.
void install_common_mkdir_ignore_exists(const char *path);

// Hex-encodes the SHA-256 of the file at `path` into a lowercase 65-byte
// buffer (64 hex chars + NUL). Returns 0 on success, -1 if the file
// couldn't be opened.
int install_common_sha256_file(const char *path, char out_hex[65]);

// Writes the effective download URL into `out`: if `download_url` is already
// absolute (starts with "http://" or "https://" - e.g. an external host like
// MediaFire/Drive), it's copied as-is; otherwise it's joined onto `base_url`
// (the catalog source it came from), matching how relative URLs from the
// FreeShop server/data repo have always worked.
void install_common_resolve_url(const char *base_url, const char *download_url,
                                 char *out, size_t out_size);

// Writes the URL to actually download from into `out`: a link resolved by
// this console when `url` is one the catalog's MediaFire proxy wraps (see
// resolved_url_ensure_direct for why that matters), or `url` unchanged
// otherwise. For the whole-file download paths - the streaming installers
// go through ResolvedUrl instead, which does this itself and additionally
// keeps the proxy URL around to re-resolve from mid-install.
void install_common_direct_download_url(const char *url, char *out, size_t out_size);

// Some libnx/sdmc filesystem driver versions don't support rename() the way
// POSIX callers expect (e.g. failing when the destination already exists,
// or failing outright across certain FS states). Used as a fallback so an
// install doesn't fail outright just because the atomic rename didn't work.
int install_common_copy_file(const char *src, const char *dst);

// One shared 4MB staging buffer for the install paths that batch SD/NCM
// writes (ncm_install.c's local-file and network paths, ncz.c's decompressed
// output). Each of these used to carry its own private `static` buffer,
// which meant ~12MB of BSS resident for the entire run even though installs
// are strictly serial - only ever one of them is in use at a time. That
// mattered: NSZ decompression asks zstd for a window buffer sized by
// whatever compression level the file was made with (8MB at nsz's default,
// far more at --ultra levels), and on a memory-constrained applet-mode
// homebrew heap that allocation was failing outright with "not enough
// memory". Sharing one buffer hands those megabytes back to the heap.
//
// Not reentrant, by design - callers must not hold this across a nested
// install, and nothing in this project does (see install_dispatch.c: one
// entry at a time, and the queue runs them sequentially).
#define INSTALL_SCRATCH_SIZE (4 * 1024 * 1024)
uint8_t *install_common_scratch(void);

// Gates reads on a source file that is still being written - a torrent
// install reading its container as it downloads (see install_local.h's
// InstallLocalGate and install_torrent.c). Handed down into the content
// readers rather than only checked per-NCA, because these containers are
// routinely one huge content piece spanning almost the whole file:
// waiting for that entire byte range before starting is indistinguishable
// from downloading first and installing after.
typedef struct {
    // Blocks until [offset, offset+len) of the source is readable.
    // CLOSES *src for the duration and reopens it before returning - the
    // Switch filesystem refuses to open a file for reading while it is
    // still open for writing elsewhere, so the reader and the downloader
    // have to take turns. Callers must therefore re-read *src afterwards
    // and re-seek, since the reopened handle starts at offset 0.
    // Returns false to abort (canceled, or the download failed).
    bool (*ensure)(void *user, FILE **src, uint64_t offset, uint64_t len);
    void *user;
} InstallReadGate;

// Folds a title's several content pieces into one continuous progress
// readout.
//
// A title is never a single file: NSP/XCI installs transfer each NCA the
// CNMT references as its own request, and every one of them reports its own
// 0..size progress. Passed straight through, that drives the caller's
// progress bar from 0 to 100% once per NCA - which reads, from the user's
// side, as the download completing and then starting over from scratch,
// repeatedly. Reporting `done_before + now` against the sum of every piece
// instead gives one bar that fills once, for the whole title.
//
// `grand_total` is 0 until the CNMT has been read (it's what states the
// piece sizes) - while it is, this passes the per-piece numbers through
// unchanged, which only covers the CNMT's own transfer (a few KB).
typedef struct {
    InstallProgressCallback cb;
    void *userdata;
    uint64_t done_before; // bytes fully transferred by previous pieces
    uint64_t grand_total; // sum across every piece of this title, 0 = not known yet
} InstallAggProgressCtx;

// Pass as the InstallProgressCallback with an InstallAggProgressCtx as
// userdata. Forwards the wrapped callback's bool return (false = cancel).
bool install_agg_progress_cb(long total, long now, void *userdata);

typedef struct {
    InstallProgressCallback cb;
    void *userdata;
} InstallProgressThunkCtx;

// Adapts HttpProgressCallback's (long dltotal, long dlnow, void*) shape to
// InstallProgressCallback's (long total, long now, void*) shape, forwarding
// its bool return (false = cancel) unchanged. Pass as the HttpProgressCallback
// to http_download_to_file with an InstallProgressThunkCtx as userdata.
bool install_common_progress_thunk(long dltotal, long dlnow, void *userdata);

// Wraps a URL that might be a self-resolving proxy (e.g. this catalog's own
// /api/dl/mediafire, which re-resolves MediaFire's page to a fresh direct
// CDN link on every single request it gets) so a multi-request install
// (install_nsp_native.c/install_xci_native.c do one HTTP request per NCA)
// doesn't pay that resolve cost - and, worse, doesn't re-trigger MediaFire's
// own page fetch and a fresh TLS handshake to it - on every single content
// piece. The first successful fetch remembers curl's CURLINFO_EFFECTIVE_URL
// (the actual link reached after following the resolver's redirect) in
// `direct_url`; every later call tries that directly first, only falling
// back to re-resolving through `proxy_url` if the cached direct link stops
// working (e.g. it expired partway through a very large install).
typedef struct {
    char proxy_url[900];
    char direct_url[900]; // empty until the first successful resolve
} ResolvedUrl;

void resolved_url_init(ResolvedUrl *r, const char *proxy_url);

// Resolves `proxy_url` to a direct link from this console rather than
// letting the server do it, when that's possible (currently: the catalog's
// own MediaFire proxy). MediaFire ties a resolved link to the IP that asked
// for it, so a link the server resolved is rejected when the console tries
// to download it - resolving here makes those the same address. See the
// implementation's comment for the full reasoning.
//
// Returns true when direct_url ends up populated. False is not an error:
// it just means the usual proxy path is used, exactly as before.
bool resolved_url_ensure_direct(ResolvedUrl *r);

// Throws away any cached direct link and resolves a fresh one, returning
// the URL to fetch from next. Use this to recover mid-install: MediaFire's
// direct links expire, so one that worked when a multi-GB download started
// can be dead by the time a later piece is requested. Falls back to the
// proxy URL only when nothing can be resolved on-console.
const char *resolved_url_refresh(ResolvedUrl *r);

// Same contract as http_get_range, sourced from `r` instead of a raw URL -
// see ResolvedUrl's doc comment above for the try-direct-then-fall-back
// behavior.
HttpResult resolved_url_get_range(ResolvedUrl *r, uint64_t offset, uint64_t length,
                                   char **out_buf, size_t *out_len,
                                   char *err_buf, size_t err_buf_size);
