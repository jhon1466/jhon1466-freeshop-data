#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} MemBuffer;

static size_t mem_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    MemBuffer *buf = (MemBuffer *)userdata;
    size_t add = size * nmemb;
    if (buf->len + add + 1 > buf->cap) {
        size_t new_cap = buf->cap == 0 ? 4096 : buf->cap;
        while (new_cap < buf->len + add + 1) new_cap *= 2;
        char *grown = (char *)realloc(buf->data, new_cap);
        if (!grown) return 0; // signals error to curl (short write)
        buf->data = grown;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, add);
    buf->len += add;
    buf->data[buf->len] = '\0';
    return add;
}

static void set_curl_error(char *err_buf, size_t err_buf_size, CURLcode code) {
    if (!err_buf || err_buf_size == 0) return;
    snprintf(err_buf, err_buf_size, "%s", curl_easy_strerror(code));
}

HttpResult http_get(const char *url, char **out_buf, size_t *out_len,
                     char *err_buf, size_t err_buf_size) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err_buf) snprintf(err_buf, err_buf_size, "curl_easy_init falló");
        return HTTP_ERR_INIT;
    }

    MemBuffer buf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // mbedtls's handshake on the Switch's ARM CPU (software crypto, no HW
    // acceleration) against Google's frontend (Firebase Hosting/Cloud Run)
    // can take noticeably longer than a plain HTTP request to a LAN
    // server - 10s was cutting it close and produced CURLE_OPERATION_TIMEDOUT.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    // Some hosts (e.g. MediaFire's free-tier download servers) throttle or
    // otherwise treat requests differently when the User-Agent doesn't look
    // like a real browser - this doesn't claim to be any specific browser
    // (no version-specific behavior implied), just avoids the obvious
    // "this is a script" signal a custom UA string sends.
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                      "Chrome/124.0.0.0 Safari/537.36");
    // Switch homebrew has no system CA trust store, so TLS peer/host
    // verification can't succeed against a real CA-signed cert here. The
    // connection is still encrypted; what's given up is confirming the
    // server's identity (MITM risk). Download integrity doesn't depend on
    // this - see the sha256 check in install/install.c, which runs
    // regardless and rejects any file that doesn't match the catalog.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    // libnx's socket service doesn't reliably support IPv6 - a dual-stack
    // host (like Firebase Hosting, which does publish AAAA records) can
    // make curl try an IPv6 connect() first and fail outright instead of
    // falling back, surfacing as CURLE_COULDNT_CONNECT. Force IPv4.
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    // Pin a minimum TLS version rather than trusting negotiation to land
    // somewhere this mbedtls build and Google's frontend both actually
    // support well.
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf.data);
        set_curl_error(err_buf, err_buf_size, res);
        return HTTP_ERR_REQUEST;
    }

    *out_buf = buf.data ? buf.data : strdup("");
    *out_len = buf.len;
    return HTTP_OK;
}

HttpResult http_get_range(const char *url, uint64_t offset, uint64_t length,
                           char **out_buf, size_t *out_len,
                           char *effective_url_out, size_t effective_url_out_size,
                           char *err_buf, size_t err_buf_size) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err_buf) snprintf(err_buf, err_buf_size, "curl_easy_init falló");
        return HTTP_ERR_INIT;
    }

    MemBuffer buf = {0};
    char range[64];
    snprintf(range, sizeof(range), "%llu-%llu",
             (unsigned long long)offset, (unsigned long long)(offset + length - 1));

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_RANGE, range);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                      "Chrome/124.0.0.0 Safari/537.36");
    // See http_get() above for why peer/host verification is off, IPv4 is
    // forced, and a minimum TLS version is pinned.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        free(buf.data);
        set_curl_error(err_buf, err_buf_size, res);
        return HTTP_ERR_REQUEST;
    }

    if (effective_url_out) {
        char *effective_url = NULL;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
        snprintf(effective_url_out, effective_url_out_size, "%s", effective_url ? effective_url : url);
    }
    curl_easy_cleanup(curl);

    *out_buf = buf.data ? buf.data : strdup("");
    *out_len = buf.len;
    return HTTP_OK;
}

HttpResult http_get_range_streamed(const char *url, uint64_t offset, uint64_t length,
                                    HttpRangeWriteCallback write_cb, void *write_userdata,
                                    char *effective_url_out, size_t effective_url_out_size,
                                    char *err_buf, size_t err_buf_size) {
    if (length == 0) return HTTP_OK;

    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err_buf) snprintf(err_buf, err_buf_size, "curl_easy_init falló");
        return HTTP_ERR_INIT;
    }

    char range[64];
    snprintf(range, sizeof(range), "%llu-%llu",
             (unsigned long long)offset, (unsigned long long)(offset + length - 1));

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_RANGE, range);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, write_userdata);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    // No CURLOPT_TIMEOUT here on purpose - unlike http_get_range's small
    // bounded reads, this streams potentially GB-sized content, exactly
    // like http_download_to_file below.
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512L * 1024L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                      "Chrome/124.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST,
                      "TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256:"
                      "TLS-ECDHE-ECDSA-WITH-CHACHA20-POLY1305-SHA256:"
                      "TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256:"
                      "TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384:"
                      "TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256:"
                      "TLS-ECDHE-ECDSA-WITH-AES-256-GCM-SHA384");

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (res == CURLE_OK && status == 206 && effective_url_out) {
        char *effective_url = NULL;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
        snprintf(effective_url_out, effective_url_out_size, "%s", effective_url ? effective_url : url);
    }
    curl_easy_cleanup(curl);

    if (res == CURLE_WRITE_ERROR) {
        // write_cb returning short is how it signals both "canceled" and
        // "the real failure was on the write_cb side" (e.g.
        // ncm_install_content_from_url's own ncmContentStorageWritePlaceHolder
        // error) - callers that care about *why* get that from their own
        // write_cb context, not from this function's err_buf, so leave it
        // untouched here instead of overwriting it with a generic message.
        return HTTP_ERR_REQUEST;
    }
    if (res != CURLE_OK) {
        set_curl_error(err_buf, err_buf_size, res);
        return HTTP_ERR_REQUEST;
    }
    // A server that ignores Range entirely answers 200 with the WHOLE body
    // instead of 206 with just the requested slice - by the time we get
    // here write_cb has already received (and written) that wrong data, so
    // this is only a best-effort check for hosts that reject Range outright
    // (e.g. a plain 416/404) rather than a hard guarantee. Every host this
    // project actually talks to (the catalog's own /api/dl/mediafire
    // resolver, MediaFire's CDN behind it, GitHub raw, and this server's own
    // static file serving) honors Range.
    if (status != 206) {
        if (err_buf) snprintf(err_buf, err_buf_size, "el servidor no soporta descargas por rango (HTTP %ld)", status);
        return HTTP_ERR_REQUEST;
    }

    return HTTP_OK;
}

typedef struct {
    HttpProgressCallback cb;
    void *userdata;
} ProgressCtx;

static int xfer_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal;
    (void)ulnow;
    ProgressCtx *ctx = (ProgressCtx *)clientp;
    if (ctx->cb && !ctx->cb((long)dltotal, (long)dlnow, ctx->userdata)) {
        return 1; // non-zero aborts the transfer (curl_easy_perform returns CURLE_ABORTED_BY_CALLBACK)
    }
    return 0;
}

HttpResult http_download_to_file(const char *url, const char *dest_path,
                                  HttpProgressCallback cb, void *userdata,
                                  char *err_buf, size_t err_buf_size) {
    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo abrir %s para escritura", dest_path);
        return HTTP_ERR_FILE;
    }
    // stdio's default buffer (a few KB) means fwrite() below turns into that
    // many small writes to the sdmc filesystem driver, which has much higher
    // per-call latency than a desktop OS - for a multi-GB file that adds up
    // to the write side dominating total download time. A large explicit
    // buffer batches those into far fewer, bigger writes; 1 MB is a multiple
    // of the allocation-unit size typical SD cards are formatted with (exFAT
    // 128KB/256KB), so each flush lands as one big aligned sequential write.
    static char file_buf[1024 * 1024];
    setvbuf(fp, file_buf, _IOFBF, sizeof(file_buf));

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        if (err_buf) snprintf(err_buf, err_buf_size, "curl_easy_init falló");
        return HTTP_ERR_INIT;
    }

    ProgressCtx ctx = { .cb = cb, .userdata = userdata };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    // Bigger receive buffer (curl's default is 16KB) - fewer, larger calls
    // into the write callback, complementing the stdio buffer above. 512KB
    // is this curl version's max (CURL_MAX_READ_SIZE); two of these fill one
    // 1MB stdio buffer before it flushes to SD.
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512L * 1024L);
    // Some hosts (e.g. MediaFire's free-tier download servers) throttle or
    // otherwise treat requests differently when the User-Agent doesn't look
    // like a real browser - this doesn't claim to be any specific browser
    // (no version-specific behavior implied), just avoids the obvious
    // "this is a script" signal a custom UA string sends.
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                      "Chrome/124.0.0.0 Safari/537.36");
    // See http_get() above for why peer/host verification is off, IPv4 is
    // forced, and a minimum TLS version is pinned.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    // Confirmed (PC browser downloads the same link fast) that the
    // bottleneck is specific to this client, not the host - most likely
    // mbedtls doing AES in plain software here (the Switch's CPU has AES
    // instructions in hardware, but this devkitPro mbedtls build may not
    // have that path enabled). ChaCha20-Poly1305 is designed to be fast in
    // pure software with no special instructions, unlike AES - list it
    // first so the server picks it if it can, falling back to AES-GCM
    // otherwise so this doesn't break hosts that only offer AES. mbedtls's
    // own cipher-suite name strings (not OpenSSL-style short names).
    curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST,
                      "TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256:"
                      "TLS-ECDHE-ECDSA-WITH-CHACHA20-POLY1305-SHA256:"
                      "TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256:"
                      "TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384:"
                      "TLS-ECDHE-ECDSA-WITH-AES-128-GCM-SHA256:"
                      "TLS-ECDHE-ECDSA-WITH-AES-256-GCM-SHA384");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res == CURLE_ABORTED_BY_CALLBACK) {
        remove(dest_path);
        if (err_buf) snprintf(err_buf, err_buf_size, "descarga cancelada");
        return HTTP_ERR_CANCELED;
    }

    if (res != CURLE_OK) {
        if (res == CURLE_WRITE_ERROR) {
            // The single most common real-world cause on Switch: SD cards
            // (especially ≤32GB ones) often ship, or get reformatted, as
            // FAT32 - which caps any single file at 4GB, a completely
            // ordinary size for an NSP/XCI game dump. Writing past that
            // boundary fails right here with exactly this curl error.
            // exFAT (what Nintendo's own format tool already uses for
            // cards over 32GB, and can be forced on smaller ones from a
            // PC) has no such limit.
            if (err_buf) {
                snprintf(err_buf, err_buf_size,
                         "%s - si el archivo pesa más de 4GB, es probable que la tarjeta SD esté "
                         "formateada en FAT32 (límite de 4GB por archivo); formatéala en exFAT desde una "
                         "PC para poder instalar archivos grandes",
                         curl_easy_strerror(res));
            }
        } else {
            set_curl_error(err_buf, err_buf_size, res);
        }
        remove(dest_path);
        return HTTP_ERR_REQUEST;
    }

    return HTTP_OK;
}

// ---- Non-blocking GET (curl's multi interface) ----

struct HttpAsyncRequest {
    CURL *easy;
    MemBuffer buf;
    bool done;
    CURLcode result;
};

// One shared multi handle for the whole process - every async request gets
// added to (and, once finished, removed from) this. Created lazily on first
// use; this app is single-threaded, so no locking is needed around it.
static CURLM *s_multi = NULL;

HttpAsyncRequest *http_get_async_start(const char *url) {
    if (!s_multi) {
        s_multi = curl_multi_init();
        if (!s_multi) return NULL;
    }

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    HttpAsyncRequest *req = (HttpAsyncRequest *)calloc(1, sizeof(HttpAsyncRequest));
    if (!req) {
        curl_easy_cleanup(curl);
        return NULL;
    }
    req->easy = curl;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &req->buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    // See http_get() above for why this UA string, and why peer/host
    // verification is off, IPv4 is forced, and a minimum TLS version is pinned.
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                      "Chrome/124.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    if (curl_multi_add_handle(s_multi, curl) != CURLM_OK) {
        curl_easy_cleanup(curl);
        free(req);
        return NULL;
    }

    return req;
}

HttpAsyncState http_async_poll(HttpAsyncRequest *req) {
    if (!req || !s_multi) return HTTP_ASYNC_DONE_ERROR;
    if (req->done) return req->result == CURLE_OK ? HTTP_ASYNC_DONE_OK : HTTP_ASYNC_DONE_ERROR;

    int still_running = 0;
    curl_multi_perform(s_multi, &still_running);

    // Multiple async requests could in principle be in flight at once (this
    // is a general-purpose API, even though today's only caller - ui_icons.c
    // - only ever runs one at a time), so match the completion message
    // against *this* request's handle rather than assuming it's the first
    // one in the queue.
    int msgs_left = 0;
    CURLMsg *msg;
    while ((msg = curl_multi_info_read(s_multi, &msgs_left)) != NULL) {
        if (msg->msg == CURLMSG_DONE && msg->easy_handle == req->easy) {
            req->done = true;
            req->result = msg->data.result;
        }
    }

    if (!req->done) return HTTP_ASYNC_RUNNING;
    return req->result == CURLE_OK ? HTTP_ASYNC_DONE_OK : HTTP_ASYNC_DONE_ERROR;
}

void http_async_finish(HttpAsyncRequest *req, char **out_buf, size_t *out_len,
                        char *err_buf, size_t err_buf_size) {
    if (!req) return;

    if (req->done && req->result == CURLE_OK) {
        *out_buf = req->buf.data ? req->buf.data : strdup("");
        *out_len = req->buf.len;
    } else {
        free(req->buf.data);
        *out_buf = NULL;
        *out_len = 0;
        set_curl_error(err_buf, err_buf_size, req->result);
    }

    curl_multi_remove_handle(s_multi, req->easy);
    curl_easy_cleanup(req->easy);
    free(req);
}

void http_async_cancel(HttpAsyncRequest *req) {
    if (!req) return;
    curl_multi_remove_handle(s_multi, req->easy);
    curl_easy_cleanup(req->easy);
    free(req->buf.data);
    free(req);
}
