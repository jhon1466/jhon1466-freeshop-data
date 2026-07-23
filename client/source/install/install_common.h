#pragma once
#include "install.h"
#include <stddef.h>

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

typedef struct {
    InstallProgressCallback cb;
    void *userdata;
} InstallProgressThunkCtx;

// Adapts HttpProgressCallback's (long dltotal, long dlnow, void*) shape to
// InstallProgressCallback's (long total, long now, void*) shape, forwarding
// its bool return (false = cancel) unchanged. Pass as the HttpProgressCallback
// to http_download_to_file with an InstallProgressThunkCtx as userdata.
bool install_common_progress_thunk(long dltotal, long dlnow, void *userdata);
