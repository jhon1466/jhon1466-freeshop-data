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

// TEMPORARY diagnostic instrumentation for the "Timeout was reached" issue
// on real hardware (survives Firebase, GitHub raw, curl_global_init, and
// bigger socket buffers - all identical) - logs libcurl's own blow-by-blow
// account of the connection (DNS/connect/TLS handshake stages) to a file on
// the SD card, since there's no visible stderr on-console. Remove once
// diagnosed. See CURLOPT_DEBUGFUNCTION docs for the curl_infotype meanings.
#define HTTP_DEBUG_LOG_PATH "sdmc:/switch/freeshop/http_debug.log"

static int curl_debug_cb(CURL *handle, curl_infotype type, char *data, size_t size, void *userdata) {
    (void)handle;
    FILE *fp = (FILE *)userdata;
    const char *prefix;
    switch (type) {
        case CURLINFO_TEXT:       prefix = "* ";       break;
        case CURLINFO_HEADER_OUT: prefix = "> ";        break;
        case CURLINFO_HEADER_IN:  prefix = "< ";        break;
        case CURLINFO_SSL_DATA_OUT: prefix = ">> SSL "; break;
        case CURLINFO_SSL_DATA_IN:  prefix = "<< SSL "; break;
        case CURLINFO_DATA_OUT:  prefix = ">> DATA ";   break;
        case CURLINFO_DATA_IN:   prefix = "<< DATA ";   break;
        default: return 0;
    }

    if (type == CURLINFO_TEXT || type == CURLINFO_HEADER_OUT || type == CURLINFO_HEADER_IN) {
        fputs(prefix, fp);
        fwrite(data, 1, size, fp);
        if (size == 0 || data[size - 1] != '\n') fputc('\n', fp);
    } else {
        fprintf(fp, "%s%zu bytes\n", prefix, size);
    }
    fflush(fp);
    return 0;
}

HttpResult http_get(const char *url, char **out_buf, size_t *out_len,
                     char *err_buf, size_t err_buf_size) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err_buf) snprintf(err_buf, err_buf_size, "curl_easy_init falló");
        return HTTP_ERR_INIT;
    }

    FILE *debug_fp = fopen(HTTP_DEBUG_LOG_PATH, "w");

    MemBuffer buf = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (debug_fp) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_cb);
        curl_easy_setopt(curl, CURLOPT_DEBUGDATA, debug_fp);
    }
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
    if (debug_fp) fclose(debug_fp);

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
    // buffer batches those into far fewer, bigger writes.
    static char file_buf[256 * 1024];
    setvbuf(fp, file_buf, _IOFBF, sizeof(file_buf));

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        if (err_buf) snprintf(err_buf, err_buf_size, "curl_easy_init falló");
        return HTTP_ERR_INIT;
    }

    // TEMPORARY - same diagnostic pattern as http_get's HTTP_DEBUG_LOG_PATH,
    // to confirm which cipher actually got negotiated (was the
    // CURLOPT_SSL_CIPHER_LIST below honored, or ignored/fell back?) rather
    // than guessing. Look for a "SSL connection using ..." line in the log.
    FILE *debug_fp = fopen("sdmc:/switch/freeshop/download_debug.log", "w");

    ProgressCtx ctx = { .cb = cb, .userdata = userdata };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    if (debug_fp) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_cb);
        curl_easy_setopt(curl, CURLOPT_DEBUGDATA, debug_fp);
    }
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    // Bigger receive buffer (curl's default is 16KB) - fewer, larger calls
    // into the write callback, complementing the stdio buffer above.
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
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
    if (debug_fp) fclose(debug_fp);

    if (res == CURLE_ABORTED_BY_CALLBACK) {
        remove(dest_path);
        if (err_buf) snprintf(err_buf, err_buf_size, "descarga cancelada");
        return HTTP_ERR_CANCELED;
    }

    if (res != CURLE_OK) {
        set_curl_error(err_buf, err_buf_size, res);
        remove(dest_path);
        return HTTP_ERR_REQUEST;
    }

    return HTTP_OK;
}
