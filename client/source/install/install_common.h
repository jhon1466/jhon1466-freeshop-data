#pragma once
#include "install.h"
#include "../net/http.h"
#include <stddef.h>
#include <stdint.h>

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

// Same contract as http_get_range, sourced from `r` instead of a raw URL -
// see ResolvedUrl's doc comment above for the try-direct-then-fall-back
// behavior.
HttpResult resolved_url_get_range(ResolvedUrl *r, uint64_t offset, uint64_t length,
                                   char **out_buf, size_t *out_len,
                                   char *err_buf, size_t err_buf_size);
