#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    HTTP_OK = 0,
    HTTP_ERR_INIT,
    HTTP_ERR_REQUEST,
    HTTP_ERR_FILE,
    HTTP_ERR_CANCELED, // cb returned false - transfer was aborted, not a real failure.
} HttpResult;

// Called periodically during http_download_to_file with total/downloaded
// byte counts so the caller can drive a progress bar. May be NULL (transfer
// always continues). Return false to abort the transfer (e.g. the user
// pressed a cancel button) - true to keep going.
typedef bool (*HttpProgressCallback)(long dltotal, long dlnow, void *userdata);

// GETs `url` into a heap buffer allocated by this function. Caller must
// free(*out_buf). *out_buf is NUL-terminated for convenience when treating
// the response as text (e.g. JSON).
HttpResult http_get(const char *url, char **out_buf, size_t *out_len,
                     char *err_buf, size_t err_buf_size);

// GETs `url` and streams the response body directly to `dest_path`
// (overwriting it if present). Does not do any renaming/atomicity - callers
// that need atomic installs should download to a temp path and rename after
// success (see install.c).
HttpResult http_download_to_file(const char *url, const char *dest_path,
                                  HttpProgressCallback cb, void *userdata,
                                  char *err_buf, size_t err_buf_size);

// GETs byte range [offset, offset+length) of `url` into a heap buffer
// allocated by this function (same ownership contract as http_get - caller
// must free(*out_buf)). Only for small, bounded reads (a PFS0/HFS0 header
// prefix, a .tik/.cert pair) where buffering the whole range in memory is
// cheap - see http_get_range_streamed for content too big to ever buffer.
//
// `effective_url_out` (may be NULL) is filled with curl's
// CURLINFO_EFFECTIVE_URL on success - the URL actually fetched from after
// following any redirects. Callers that hit a self-resolving proxy (like
// this catalog's /api/dl/mediafire, which re-resolves MediaFire's page to a
// fresh direct CDN link on every request) can use this to skip the resolve
// step on subsequent calls - see install/install_common.h's ResolvedUrl.
HttpResult http_get_range(const char *url, uint64_t offset, uint64_t length,
                           char **out_buf, size_t *out_len,
                           char *effective_url_out, size_t effective_url_out_size,
                           char *err_buf, size_t err_buf_size);

// Same contract as CURLOPT_WRITEFUNCTION: called with each chunk of the
// response body as it arrives, must return size*nmemb on success (anything
// else aborts the transfer).
typedef size_t (*HttpRangeWriteCallback)(void *ptr, size_t size, size_t nmemb, void *userdata);

// GETs byte range [offset, offset+length) of `url`, handing each chunk of
// the response body to `write_cb` as it arrives instead of buffering it -
// used to install NCA content straight from the network into NCM's content
// storage without ever writing the (potentially multi-GB) source file to
// the SD card first. See install/ncm_install.c's ncm_install_content_from_url.
//
// `effective_url_out` (may be NULL) - same contract as http_get_range's.
HttpResult http_get_range_streamed(const char *url, uint64_t offset, uint64_t length,
                                    HttpRangeWriteCallback write_cb, void *write_userdata,
                                    char *effective_url_out, size_t effective_url_out_size,
                                    char *err_buf, size_t err_buf_size);

// ---- Non-blocking GET (curl's multi interface) ----
//
// http_get() blocks the calling thread for the whole transfer, which is
// fine for one-off catalog/admin requests but not for something driven from
// inside a render loop (e.g. ui_icons.c fetching a grid's worth of icons) -
// a single slow/stalled connection would freeze input handling and drawing
// for as long as it takes. This variant is driven forward a little at a
// time by calling http_async_poll() every frame instead - it never blocks.

typedef struct HttpAsyncRequest HttpAsyncRequest;

typedef enum {
    HTTP_ASYNC_RUNNING,
    HTTP_ASYNC_DONE_OK,
    HTTP_ASYNC_DONE_ERROR,
} HttpAsyncState;

// Starts a GET of `url` without blocking - the request isn't necessarily
// even connected yet by the time this returns. Returns NULL only on
// immediate local failure (curl init/setup), which is rare and doesn't need
// a detailed reason. Call http_async_poll() to drive it forward.
HttpAsyncRequest *http_get_async_start(const char *url);

// Advances the transfer (cheap, non-blocking - safe to call every frame,
// even many times a frame). Once this returns a DONE_* state, call
// http_async_finish() to collect the result; don't poll a request again
// after that.
HttpAsyncState http_async_poll(HttpAsyncRequest *req);

// Only valid once http_async_poll() returned HTTP_ASYNC_DONE_OK or
// HTTP_ASYNC_DONE_ERROR. On DONE_OK, fills *out_buf (caller must
// free(*out_buf), NUL-terminated like http_get())/*out_len. On DONE_ERROR,
// *out_buf is set to NULL and err_buf (may be NULL) gets a message. Either
// way, frees `req` - it must not be touched again after this call.
void http_async_finish(HttpAsyncRequest *req, char **out_buf, size_t *out_len,
                        char *err_buf, size_t err_buf_size);

// Abandons an in-flight request (e.g. its result is no longer needed)
// without waiting for it to finish, and frees `req`. Only valid before
// http_async_finish() has been called on it.
void http_async_cancel(HttpAsyncRequest *req);
