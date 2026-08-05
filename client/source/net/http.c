#include "http.h"

#include <switch.h>
#include <curl/curl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Diagnostic-only, same pattern as main.c's update_debug_log/ui_icons.c's
// icon_debug_log: a real user report of some apps "installing before fully
// downloaded" pointed at a truncated transfer somehow not being caught -
// logging every download's requested vs. actually-received byte count here
// instead of guessing blind. Best-effort: a failure to open the log is
// silently ignored. Always on (no settings toggle) - matches this
// project's other debug logs, and the file only grows when something is
// actually downloaded, which is rare enough not to matter.
void download_debug_log(const char *fmt, ...) {
    FILE *fp = fopen("sdmc:/switch/freeshop/download_debug.log", "a");
    if (!fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

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

typedef struct {
    MemBuffer buf;
    size_t max_len; // abort once buf.len would exceed this
} BoundedMemBuffer;

// Same growable-buffer behavior as mem_write_cb, but refuses to grow past
// max_len. http_get_range asks for a small slice (a header, a ticket, a few
// KB); a server that ignores Range and answers 200 with the entire file
// instead would otherwise have mem_write_cb buffer that whole file - for an
// NSP/XCI, gigabytes - into RAM before curl_easy_perform even returns,
// which is what a "the app just hangs for a long time" report turned out to
// be. Returning a short write here aborts the transfer immediately (curl
// surfaces it as CURLE_WRITE_ERROR) instead of buffering data nothing will
// ever use.
static size_t bounded_mem_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    BoundedMemBuffer *bounded = (BoundedMemBuffer *)userdata;
    size_t add = size * nmemb;
    if (bounded->buf.len + add > bounded->max_len) return 0;
    return mem_write_cb(ptr, size, nmemb, &bounded->buf);
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

    // A little slack past the requested length: a compliant 206 response is
    // never larger than asked for, so this only ever matters for the
    // non-compliant-server case this guards against, not for legitimate
    // traffic.
    BoundedMemBuffer bounded = { .buf = {0}, .max_len = (size_t)length + 4096 };
    char range[64];
    snprintf(range, sizeof(range), "%llu-%llu",
             (unsigned long long)offset, (unsigned long long)(offset + length - 1));

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_RANGE, range);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bounded_mem_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bounded);
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

    static const char *const RANGE_UNSUPPORTED_MSG =
        "el servidor no soporta descargas por rango - los archivos NSP/XCI grandes necesitan esto "
        "para instalarse sin descargar el archivo completo primero; usa \"Instalar vía DBI\" en su lugar";

    if (res == CURLE_WRITE_ERROR) {
        // bounded_mem_write_cb refusing more than requested - a server
        // ignoring Range and sending the whole file back. Reported the same
        // way as the 200-instead-of-206 case below (it's the same
        // underlying problem, just caught before wasting time/memory
        // buffering gigabytes that would've been thrown away anyway).
        curl_easy_cleanup(curl);
        free(bounded.buf.data);
        if (err_buf) snprintf(err_buf, err_buf_size, "%s", RANGE_UNSUPPORTED_MSG);
        return HTTP_ERR_REQUEST;
    }
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        free(bounded.buf.data);
        set_curl_error(err_buf, err_buf_size, res);
        return HTTP_ERR_REQUEST;
    }

    // A server that doesn't support (or outright ignores) Range but answers
    // with something small enough to fit under bounded_mem_write_cb's cap
    // still needs catching here: 200 with the WHOLE body instead of 206
    // with just the requested slice. Every caller of this function
    // (hfs0_parse_at_url/pfs0 equivalents, ticket/cert fetches) treats
    // whatever comes back as if it were exactly the requested
    // [offset, offset+length) slice, so a 200 here would get silently
    // parsed as if it were that slice - garbage read from the wrong file
    // position, surfacing as a confusing "not a valid NSP/XCI" or "'secure'
    // partition not found" instead of the real cause.
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != 206) {
        curl_easy_cleanup(curl);
        free(bounded.buf.data);
        if (err_buf) snprintf(err_buf, err_buf_size, "%s", RANGE_UNSUPPORTED_MSG);
        return HTTP_ERR_REQUEST;
    }

    if (effective_url_out) {
        char *effective_url = NULL;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
        snprintf(effective_url_out, effective_url_out_size, "%s", effective_url ? effective_url : url);
    }
    curl_easy_cleanup(curl);

    *out_buf = bounded.buf.data ? bounded.buf.data : strdup("");
    *out_len = bounded.buf.len;
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
    //
    // A stall-detecting bound instead: without one, a connection that goes
    // quiet without closing hangs this call forever, and because the
    // install's progress/cancel handling only runs when data arrives (the
    // write callback drives it), the whole app appears frozen with no way
    // to back out. Aborting on "essentially no data for this long" turns
    // that into a normal transfer error the caller resumes from
    // (ncm_install.c reconnects and continues where it left off), so a
    // dropped connection recovers by itself.
    //
    // The threshold is deliberately near-zero rather than a real speed
    // floor, so genuinely slow connections are never cut off - only ones
    // delivering literally nothing. The window is short because every
    // second spent waiting on a connection that has already died is a
    // second of visibly stalled download, and reconnecting is cheap
    // (measured: ~0.1s to connect, ~0.5s to first byte).
    //
    // It still has to clear how long a 4MB flush to NCM can block this
    // transfer (see nca_flush), since that time counts against curl's speed
    // average even though nothing is wrong - measured at ~0.16s per flush,
    // so 8s leaves a very wide margin.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 8L);
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

    u64 start_tick = armGetSystemTick();
    CURLcode res = curl_easy_perform(curl);
    double elapsed_sec = armTicksToNs(armGetSystemTick() - start_tick) / 1e9;

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (res == CURLE_OK && status == 206 && effective_url_out) {
        char *effective_url = NULL;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
        snprintf(effective_url_out, effective_url_out_size, "%s", effective_url ? effective_url : url);
    }

    // See http_download_to_file's identical block for why - this is the
    // per-NCA-piece equivalent, and the one most directly relevant to a
    // report of a *native* NSP/XCI install finishing with less than the
    // whole title actually written: `downloaded` is exactly how many bytes
    // curl handed to write_cb (which is what actually reaches NCM here),
    // whether or not that matches what was asked for in `length`.
    //
    // Timing alongside it: how long the connection took to set up
    // (mbedtls's handshake is slow on this hardware) versus how long it
    // then spent transferring is the difference between "the host is
    // throttling" and "reconnecting is costing more than it saves", which
    // the byte counts alone can't distinguish.
    curl_off_t dbg_downloaded = -1;
    double dbg_connect = 0.0, dbg_starttransfer = 0.0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &dbg_downloaded);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &dbg_connect);
    curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &dbg_starttransfer);
    download_debug_log("http_get_range_streamed: offset=%llu requested=%llu result=%s "
                        "http_status=%ld downloaded=%lld elapsed=%.1fs connect=%.1fs first_byte=%.1fs "
                        "avg=%.2fMB/s",
                        (unsigned long long)offset, (unsigned long long)length,
                        curl_easy_strerror(res), status, (long long)dbg_downloaded,
                        elapsed_sec, dbg_connect, dbg_starttransfer,
                        elapsed_sec > 0 ? (dbg_downloaded / elapsed_sec) / (1024.0 * 1024.0) : 0.0);

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
    // Same stall bound as http_get_range_streamed - see the comment there.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
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

    // Captured before cleanup/fclose, purely for download_debug_log below -
    // none of this affects the actual success/failure decision, which stays
    // exactly what it already was (curl's own CURLcode). See this
    // function's header comment: the point is to see, after the fact,
    // whether a report of "installed before fully downloaded" lines up with
    // curl already knowing the transfer was short (expected vs. actual
    // disagree here) or with it believing everything arrived fine (in which
    // case the bug is downstream of this function, not in the download
    // itself).
    curl_off_t dbg_content_length = -1, dbg_downloaded = -1;
    long dbg_status = 0;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &dbg_content_length);
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &dbg_downloaded);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &dbg_status);
    long dbg_file_size = ftell(fp);
    download_debug_log("http_download_to_file: url=%s result=%s http_status=%ld "
                        "content_length=%lld downloaded=%lld file_size=%ld",
                        url, curl_easy_strerror(res), dbg_status,
                        (long long)dbg_content_length, (long long)dbg_downloaded, dbg_file_size);

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

// Defined with the abandoned-request list below (http_async_cancel).
static void reap_abandoned(void);

HttpAsyncRequest *http_get_async_start(const char *url) {
    // Cheap, never blocks - keeps canceled-but-finished requests from
    // lingering when the caller cancels rarely but starts often.
    reap_abandoned();

    HttpAsyncRequest *req = (HttpAsyncRequest *)calloc(1, sizeof(HttpAsyncRequest));
    if (!req) return NULL;
    snprintf(req->url, sizeof(req->url), "%s", url);

    // A couple of points lower priority (0x2E) than the render/main
    // thread's default 0x2C - NOT the default this used to run at. Up to
    // ICON_CONCURRENT_FETCHES of these run at once, each doing a real
    // curl_easy_perform() with mbedtls's TLS handshake and AES done in
    // software (see this file's own comments on that) - genuinely
    // CPU-heavy work, not just I/O wait. At equal priority with the render
    // thread and no core pinned (-2 = any core), several of these
    // competing for the same core as the render thread was measured
    // (icon_debug.log evidence: frame times tracking fetches-in-flight
    // almost exactly, even while the app was otherwise idle/stationary)
    // starving it of CPU time - not blocked on anything, just not getting
    // scheduled.
    //
    // This first shipped as 0x3F (63, the lowest value HOS's priority range
    // allows) on the reasoning that these are cosmetic background work and
    // should always lose to the render thread - measured on hardware as
    // starving them completely instead (0 of 885 fetches in one session
    // ever completed, not even with an error - see icon_debug.log's
    // "starting fetch" vs "fetch OK"/"fetch failed" counts). HOS's
    // scheduler is strictly priority-preemptive - any lower priority than
    // the render thread already gets it preempted the instant the render
    // thread is ready to run, so the full drop to 63 bought nothing over a
    // small one and apparently hit some other threshold instead (this
    // devkitPro build's own priority clamping, most likely). A small gap is
    // both sufficient and, empirically, actually works.
    Result rc = threadCreate(&req->thread, http_async_thread_func, req, NULL, HTTP_ASYNC_STACK_SIZE, 0x2E, -2);
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

// ---- abandoned (canceled-but-still-running) requests ----
//
// There's no safe way to forcibly kill a thread mid curl_easy_perform -
// mbedtls/curl state would be left in an unknown state - so a canceled
// request has to be allowed to finish on its own.
//
// This used to be done by simply blocking in http_async_cancel() until the
// thread exited, on the theory that cancellation only happened on a rare
// event like switching catalog sources. That stopped being true: the icon
// cache cancels a fetch every time one scrolls off-screen (see ui_icons.c's
// preempt_fetch_slot), which happens constantly while scrolling - and
// http_async_cancel runs on the render thread. A user-captured log caught
// the result: a single 16.5-SECOND frozen frame during a fast scroll, which
// is exactly this blocking wait sitting out a stuck connection's full
// HTTP_ASYNC_TOTAL_TIMEOUT_S.
//
// Instead: hand the request over here and return immediately. Its thread is
// already detached from anything the UI cares about (nothing will read its
// buffer), so it just needs collecting once it finishes on its own - which
// reap_abandoned() does, without ever waiting.
#define HTTP_ASYNC_MAX_ABANDONED 32
static HttpAsyncRequest *s_abandoned[HTTP_ASYNC_MAX_ABANDONED];
static int s_abandoned_count = 0;

// Frees every abandoned request whose thread has already exited. Never
// blocks: a still-running one is simply left for a later sweep.
static void reap_abandoned(void) {
    for (int i = 0; i < s_abandoned_count; ) {
        HttpAsyncRequest *req = s_abandoned[i];
        if (!__atomic_load_n(&req->done, __ATOMIC_ACQUIRE)) {
            i++;
            continue;
        }
        threadWaitForExit(&req->thread); // already exited - returns at once
        threadClose(&req->thread);
        free(req->buf.data);
        free(req);
        s_abandoned[i] = s_abandoned[--s_abandoned_count];
    }
}

void http_async_cancel(HttpAsyncRequest *req) {
    if (!req) return;

    // Collect anything abandoned earlier that has since finished, so the
    // list stays short without needing its own timer or public API.
    reap_abandoned();

    if (!req->started) {
        free(req->buf.data);
        free(req);
        return;
    }

    if (__atomic_load_n(&req->done, __ATOMIC_ACQUIRE)) {
        // Already finished - collecting it costs nothing.
        threadWaitForExit(&req->thread);
        threadClose(&req->thread);
        free(req->buf.data);
        free(req);
        return;
    }

    if (s_abandoned_count < HTTP_ASYNC_MAX_ABANDONED) {
        s_abandoned[s_abandoned_count++] = req;
        return;
    }

    // The abandoned list is full - fall back to the old blocking wait
    // rather than leaking. Reaching this needs 32 separate fetches to be
    // simultaneously canceled AND still running, which the caller's own
    // concurrency cap makes unreachable in practice.
    threadWaitForExit(&req->thread);
    threadClose(&req->thread);
    free(req->buf.data);
    free(req);
}
