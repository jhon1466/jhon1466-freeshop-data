#include "http.h"

#include <switch.h>
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

typedef struct {
    int64_t content_length;
    int64_t range_total;
} SizeProbeHeaders;

static size_t size_probe_header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    SizeProbeHeaders *h = (SizeProbeHeaders *)userdata;
    size_t len = size * nitems;

    // A status line starts a NEW response - a redirect hop, or a 1xx
    // interim - so anything captured from the previous one is stale.
    // Deliberately keyed on this and NOT on the blank line that terminates
    // a header block: that blank line also arrives for the final response,
    // where resetting wipes the values immediately after parsing them.
    // (That was exactly the bug that made every size probe come back empty.)
    if (len >= 5 && strncasecmp(buffer, "HTTP/", 5) == 0) {
        h->content_length = -1;
        h->range_total = -1;
        return len;
    }

    if (len > 15 && strncasecmp(buffer, "content-length:", 15) == 0) {
        h->content_length = strtoll(buffer + 15, NULL, 10);
    } else if (len > 14 && strncasecmp(buffer, "content-range:", 14) == 0) {
        // "Content-Range: bytes 0-0/123456" - the total is after the slash.
        const char *slash = memchr(buffer, '/', len);
        if (slash) h->range_total = strtoll(slash + 1, NULL, 10);
    }
    return len;
}

// Deliberately refuses every byte of body - the transfer is only ever kept
// alive long enough for curl to finish parsing headers (which happens
// before this is ever called), then aborted. Used so a size probe never
// actually downloads a server's response body, however big it is.
static size_t size_probe_abort_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr; (void)size; (void)nmemb; (void)userdata;
    return 0;
}

// Shared driver for both probe styles. `use_head` picks a real HTTP HEAD
// (cheapest, when the server implements it properly); otherwise it's a GET
// carrying a throwaway Range hint, aborted the instant a body byte would be
// written. Either way the answer is read out of the response headers rather
// than curl_easy_getinfo, so a 206's Content-Range total is usable too.
//
// Timeouts are deliberately much tighter than the rest of this file's: this
// runs once per file while building a raw-directory catalog (see catalog.c),
// so a slow or unresponsive host has to fail fast rather than stall the
// whole catalog load for minutes.
static void size_probe(const char *url, bool use_head, int64_t *out_size) {
    *out_size = -1;

    CURL *curl = curl_easy_init();
    if (!curl) return;

    SizeProbeHeaders headers = { .content_length = -1, .range_total = -1 };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (use_head) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, size_probe_abort_write_cb);
    }
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, size_probe_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                      "Chrome/124.0.0.0 Safari/537.36");
    // See http_get() above for why peer/host verification is off, IPv4 is
    // forced, and a minimum TLS version is pinned.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    // The return value is intentionally ignored: CURLE_WRITE_ERROR is
    // size_probe_abort_write_cb doing its job, and any genuine error just
    // leaves both header fields at -1 below.
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    // Content-Range's total wins over Content-Length: on a 206 the latter
    // is only the single byte that was asked for, not the file's size.
    if (headers.range_total > 0) *out_size = headers.range_total;
    else if (headers.content_length > 0) *out_size = headers.content_length;
}

HttpResult http_head_content_length(const char *url, int64_t *out_size,
                                     char *err_buf, size_t err_buf_size) {
    (void)err_buf;
    (void)err_buf_size;

    size_probe(url, true, out_size);
    if (*out_size > 0) return HTTP_OK;

    // HEAD gave nothing usable - plenty of minimal static-file servers
    // (exactly the kind a raw folder source points at) either reject it
    // outright or answer it without a Content-Length. A GET always works.
    size_probe(url, false, out_size);
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

// ---- Non-blocking GET (a real background thread running a blocking
// curl_easy_perform(), not curl's multi/non-blocking-socket interface) ----
//
// This used to be built on curl_multi - curl_multi_perform() driven a
// little at a time from the render loop, relying on curl's non-blocking
// socket support to make incremental progress each call. In practice icons
// loaded inconsistently ("some load, some don't", reported after this had
// already shipped) even after adding retries, which pointed at the
// mechanism itself rather than at any individual failure: a single stuck
// connection holds curl_multi's non-blocking read pending indefinitely with
// no way to tell "still working" from "stalled" apart from the 60s
// CURLOPT_TIMEOUT - and since ui_icons.c only ever runs one fetch at a
// time, one stuck icon blocked every icon behind it for up to a minute.
// libnx's non-blocking socket support isn't guaranteed to behave like a
// desktop OS's, which curl_multi's model assumes.
//
// A real OS thread doing the exact same blocking curl_easy_perform() that
// http_get()/http_download_to_file() already use successfully sidesteps
// that assumption entirely - the network I/O genuinely blocks, just on a
// thread that isn't the render loop's, so nothing about the request's
// timing is any different from a normal blocking fetch, and the main
// thread only ever does a cheap atomic flag check to poll it.
struct HttpAsyncRequest {
    Thread thread;
    char url[900];
    MemBuffer buf;
    CURLcode result;
    bool done;    // written with release semantics by the worker, read with acquire by the poller
    bool started; // threadCreate+threadStart both succeeded - only then is `thread` valid to wait on/close
};

// Icons are small - if a connection hasn't even finished by this point,
// something is wrong with that host/link specifically, and it's better to
// fail fast (ui_icons.c retries a failed fetch a few times on its own) than
// to sit on the request for the full minute http_get()'s general-purpose
// timeout allows.
#define HTTP_ASYNC_CONNECT_TIMEOUT_S 10L
#define HTTP_ASYNC_TOTAL_TIMEOUT_S 20L
// mbedtls's TLS handshake plus libpng/libjpeg-turbo decoding elsewhere on
// this same stack easily fit in a fraction of this; generous headroom costs
// nothing (SD-card-backed heap, not a tightly limited real stack budget).
#define HTTP_ASYNC_STACK_SIZE (64 * 1024)

static void http_async_thread_func(void *arg) {
    HttpAsyncRequest *req = (HttpAsyncRequest *)arg;

    CURL *curl = curl_easy_init();
    if (!curl) {
        req->result = CURLE_FAILED_INIT;
        __atomic_store_n(&req->done, true, __ATOMIC_RELEASE);
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &req->buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, HTTP_ASYNC_CONNECT_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, HTTP_ASYNC_TOTAL_TIMEOUT_S);
    // See http_get() above for why this UA string, and why peer/host
    // verification is off, IPv4 is forced, and a minimum TLS version is pinned.
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                      "Chrome/124.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    req->result = res;
    // Release-store: everything written above (req->buf, req->result) must
    // be visible to whichever core later observes `done == true` via the
    // matching acquire-load in http_async_poll().
    __atomic_store_n(&req->done, true, __ATOMIC_RELEASE);
}

HttpAsyncRequest *http_get_async_start(const char *url) {
    HttpAsyncRequest *req = (HttpAsyncRequest *)calloc(1, sizeof(HttpAsyncRequest));
    if (!req) return NULL;
    snprintf(req->url, sizeof(req->url), "%s", url);

    // Default priority/core (0x2C, -2) - same as every other thread this
    // app creates implicitly via libnx's own defaults; nothing about a
    // small icon fetch needs different scheduling treatment.
    Result rc = threadCreate(&req->thread, http_async_thread_func, req, NULL, HTTP_ASYNC_STACK_SIZE, 0x2C, -2);
    if (R_FAILED(rc)) {
        free(req);
        return NULL;
    }
    rc = threadStart(&req->thread);
    if (R_FAILED(rc)) {
        threadClose(&req->thread);
        free(req);
        return NULL;
    }
    req->started = true;
    return req;
}

HttpAsyncState http_async_poll(HttpAsyncRequest *req) {
    if (!req) return HTTP_ASYNC_DONE_ERROR;
    bool done = __atomic_load_n(&req->done, __ATOMIC_ACQUIRE);
    if (!done) return HTTP_ASYNC_RUNNING;
    return req->result == CURLE_OK ? HTTP_ASYNC_DONE_OK : HTTP_ASYNC_DONE_ERROR;
}

void http_async_finish(HttpAsyncRequest *req, char **out_buf, size_t *out_len,
                        char *err_buf, size_t err_buf_size) {
    if (!req) return;

    // Only ever called after http_async_poll() reported a DONE_* state, so
    // the thread has already returned (or is about to) - this join is not
    // a meaningful wait, just releasing the kernel resources.
    if (req->started) {
        threadWaitForExit(&req->thread);
        threadClose(&req->thread);
    }

    if (req->result == CURLE_OK) {
        *out_buf = req->buf.data ? req->buf.data : strdup("");
        *out_len = req->buf.len;
    } else {
        free(req->buf.data);
        *out_buf = NULL;
        *out_len = 0;
        set_curl_error(err_buf, err_buf_size, req->result);
    }

    free(req);
}

void http_async_cancel(HttpAsyncRequest *req) {
    if (!req) return;
    // Unlike http_async_finish(), this can be called before the thread has
    // finished (e.g. switching catalog sources mid-fetch) - there's no safe
    // way to forcibly kill it mid curl_easy_perform (mbedtls/curl state
    // would be left in an unknown state), so this waits for it to finish on
    // its own. Bounded by HTTP_ASYNC_CONNECT_TIMEOUT_S/HTTP_ASYNC_TOTAL_TIMEOUT_S
    // (20s worst case) - not instant, but a source switch is rare enough
    // that this is an acceptable trade-off against the complexity of a
    // proper fire-and-forget abandon path.
    if (req->started) {
        threadWaitForExit(&req->thread);
        threadClose(&req->thread);
    }
    free(req->buf.data);
    free(req);
}
