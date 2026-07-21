#pragma once
#include <stddef.h>

typedef enum {
    HTTP_OK = 0,
    HTTP_ERR_INIT,
    HTTP_ERR_REQUEST,
    HTTP_ERR_FILE,
} HttpResult;

// Called periodically during http_download_to_file with total/downloaded
// byte counts so the caller can drive a progress bar. May be NULL.
typedef void (*HttpProgressCallback)(long dltotal, long dlnow, void *userdata);

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
