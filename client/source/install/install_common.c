#include "install_common.h"

#include <mbedtls/sha256.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

uint8_t *install_common_scratch(void) {
    // Deliberately BSS rather than a lazy malloc: this is needed at the
    // exact moments memory is tightest (mid-install), where a failed
    // allocation would be far worse than reserving it up front.
    static uint8_t buf[INSTALL_SCRATCH_SIZE];
    return buf;
}

static bool is_absolute_url(const char *url) {
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

void install_common_resolve_url(const char *base_url, const char *download_url,
                                 char *out, size_t out_size) {
    if (is_absolute_url(download_url)) {
        snprintf(out, out_size, "%s", download_url);
    } else {
        snprintf(out, out_size, "%s%s", base_url, download_url);
    }
}

void install_common_mkdir_ignore_exists(const char *path) {
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        // Real failures (e.g. read-only SD, invalid path) surface later when
        // the caller tries to open a file inside this directory and fails.
    }
}

static void sha256_hex(const unsigned char digest[32], char out_hex[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[(digest[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
}

int install_common_sha256_file(const char *path, char out_hex[65]) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256, not the SHA-224 variant

    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        mbedtls_sha256_update(&ctx, buf, n);
    }
    fclose(fp);

    unsigned char digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    sha256_hex(digest, out_hex);
    return 0;
}

int install_common_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;

    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }

    char buf[8192];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }

    fclose(in);
    fclose(out);

    if (!ok) {
        remove(dst);
        return -1;
    }
    return 0;
}

bool install_agg_progress_cb(long total, long now, void *userdata) {
    InstallAggProgressCtx *agg = (InstallAggProgressCtx *)userdata;
    if (!agg->cb) return true;

    if (agg->grand_total == 0) {
        // Sizes not known yet - nothing to aggregate against, so pass the
        // piece's own numbers through (see InstallAggProgressCtx).
        return agg->cb(total, now, agg->userdata);
    }

    uint64_t overall = agg->done_before + (now > 0 ? (uint64_t)now : 0);
    // Clamped because the sizes come from two different places (the CNMT's
    // declared sizes vs. what each transfer actually reports) and a piece
    // overshooting its declared size by a few bytes shouldn't ever render
    // as more than 100%.
    if (overall > agg->grand_total) overall = agg->grand_total;
    return agg->cb((long)agg->grand_total, (long)overall, agg->userdata);
}

bool install_common_progress_thunk(long dltotal, long dlnow, void *userdata) {
    InstallProgressThunkCtx *ctx = (InstallProgressThunkCtx *)userdata;
    if (!ctx->cb) return true;
    return ctx->cb(dltotal, dlnow, ctx->userdata);
}

// ---- On-console MediaFire resolution ----
//
// MediaFire binds a resolved direct link to the IP that resolved it. The
// catalog's /api/dl/mediafire proxy resolves from the server's IP, so the
// link it hands back is bound to *that* address - which the server's own
// fileSize probe can then use (same IP, works), but the console cannot: it
// downloads from a different IP and MediaFire bounces it to
// download_repair.php instead of serving bytes. Confirmed on the wire:
// the bounce URL literally names the requesting IP. Nothing header-side
// fixes it - a matching Referer/User-Agent changes nothing.
//
// The fix is to cut the server out of the resolve entirely: scrape
// MediaFire's file page from the console, so the address that resolves the
// link is the same one that downloads it - exactly what a browser does.
// Purely an optimization: if any step here fails (page unreachable,
// ad-gated page with no ready link, layout change), direct_url stays empty
// and everything falls back to the proxy exactly as before.

#define MEDIAFIRE_PROXY_MARKER "/api/dl/mediafire?url="

// Percent-decodes `src` into `dest`. Deliberately leaves '+' alone: it's a
// form-encoding convention, not a URL-path one, and MediaFire filenames
// genuinely contain '+' where spaces were.
static void percent_decode(const char *src, char *dest, size_t dest_size) {
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dest_size; si++) {
        if (src[si] == '%' && isxdigit((unsigned char)src[si + 1]) && isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            dest[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dest[di++] = src[si];
        }
    }
    dest[di] = '\0';
}

// Pulls the wrapped MediaFire page URL back out of a proxy URL. False when
// `url` isn't one of ours (a plain direct link, GitHub raw, a self-hosted
// source, etc.) - those need no resolving and are used as-is.
static bool mediafire_page_from_proxy(const char *url, char *out, size_t out_size) {
    const char *marker = strstr(url, MEDIAFIRE_PROXY_MARKER);
    if (!marker) return false;
    percent_decode(marker + strlen(MEDIAFIRE_PROXY_MARKER), out, out_size);
    return strncmp(out, "http", 4) == 0;
}

// Scrapes the ready-to-use CDN link out of a MediaFire file page: the
// anchor carrying id="downloadButton". `href` sits *before* `id` in the
// real markup, so this finds the id first and then walks back to the
// opening "<a" to search the whole tag - matching on the id is what makes
// it the right anchor (the page has several others).
static bool mediafire_scrape_link(const char *html, char *out, size_t out_size) {
    const char *id = strstr(html, "id=\"downloadButton\"");
    if (!id) return false;

    const char *tag = id;
    while (tag > html && !(tag[0] == '<' && (tag[1] == 'a' || tag[1] == 'A'))) tag--;
    if (tag == html) return false;

    const char *href = strstr(tag, "href=\"");
    if (!href || href > id) return false;
    href += 6;

    const char *end = strchr(href, '"');
    if (!end) return false;

    size_t len = (size_t)(end - href);
    if (len == 0 || len >= out_size) return false;
    memcpy(out, href, len);
    out[len] = '\0';

    // The href is HTML-escaped in the page source; "&amp;" is the only
    // entity that shows up in these links.
    char *amp;
    while ((amp = strstr(out, "&amp;")) != NULL) {
        memmove(amp + 1, amp + 5, strlen(amp + 5) + 1);
    }
    return strncmp(out, "http", 4) == 0;
}

bool resolved_url_ensure_direct(ResolvedUrl *r) {
    if (r->direct_url[0]) return true;

    char page_url[900];
    if (!mediafire_page_from_proxy(r->proxy_url, page_url, sizeof(page_url))) return false;

    char *html = NULL;
    size_t html_len = 0;
    if (http_get(page_url, &html, &html_len, NULL, 0) != HTTP_OK) return false;

    bool ok = mediafire_scrape_link(html, r->direct_url, sizeof(r->direct_url));
    free(html);
    if (!ok) r->direct_url[0] = '\0';
    return ok;
}

void install_common_direct_download_url(const char *url, char *out, size_t out_size) {
    ResolvedUrl r;
    resolved_url_init(&r, url);
    if (resolved_url_ensure_direct(&r)) {
        snprintf(out, out_size, "%s", r.direct_url);
    } else {
        snprintf(out, out_size, "%s", url);
    }
}

void resolved_url_init(ResolvedUrl *r, const char *proxy_url) {
    snprintf(r->proxy_url, sizeof(r->proxy_url), "%s", proxy_url);
    r->direct_url[0] = '\0';
}

// A self-resolving proxy like this catalog's /api/dl/mediafire hands back a
// different storage node on every resolve (confirmed directly: three
// requests against the same proxy URL landed on three different
// download*.mediafire.com hosts) - MediaFire's own CDN, not something the
// proxy controls. Node-to-node behavior isn't uniform: some don't honor
// Range at all (answer 200 with the whole body instead of 206 with the
// requested slice - see http_get_range's check), which single-handedly
// blocks the native streaming installer for whichever titles happen to land
// on one. A few retries, each a genuinely fresh resolve, gives a real shot
// at landing on a node that does support it instead of failing outright on
// the first unlucky one.
#define RESOLVED_URL_RANGE_RETRIES 4

HttpResult resolved_url_get_range(ResolvedUrl *r, uint64_t offset, uint64_t length,
                                   char **out_buf, size_t *out_len,
                                   char *err_buf, size_t err_buf_size) {
    resolved_url_ensure_direct(r);

    if (r->direct_url[0]) {
        HttpResult hres = http_get_range(r->direct_url, offset, length, out_buf, out_len,
                                          NULL, 0, err_buf, err_buf_size);
        if (hres == HTTP_OK) return HTTP_OK;
        // The cached direct link stopped working (expired, host hiccup,
        // etc.) - fall through to a fresh resolve instead of failing
        // outright.
        r->direct_url[0] = '\0';
    }

    HttpResult hres = HTTP_ERR_REQUEST;
    for (int attempt = 0; attempt < RESOLVED_URL_RANGE_RETRIES; attempt++) {
        hres = http_get_range(r->proxy_url, offset, length, out_buf, out_len,
                               r->direct_url, sizeof(r->direct_url), err_buf, err_buf_size);
        if (hres == HTTP_OK) return HTTP_OK;
        r->direct_url[0] = '\0';
    }
    return hres;
}
