#include "install_common.h"

#include <mbedtls/sha256.h>
#include <errno.h>
#include <stdio.h>
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

bool install_common_progress_thunk(long dltotal, long dlnow, void *userdata) {
    InstallProgressThunkCtx *ctx = (InstallProgressThunkCtx *)userdata;
    if (!ctx->cb) return true;
    return ctx->cb(dltotal, dlnow, ctx->userdata);
}

void resolved_url_init(ResolvedUrl *r, const char *proxy_url) {
    snprintf(r->proxy_url, sizeof(r->proxy_url), "%s", proxy_url);
    r->direct_url[0] = '\0';
}

HttpResult resolved_url_get_range(ResolvedUrl *r, uint64_t offset, uint64_t length,
                                   char **out_buf, size_t *out_len,
                                   char *err_buf, size_t err_buf_size) {
    if (r->direct_url[0]) {
        HttpResult hres = http_get_range(r->direct_url, offset, length, out_buf, out_len,
                                          NULL, 0, err_buf, err_buf_size);
        if (hres == HTTP_OK) return HTTP_OK;
        // The cached direct link stopped working (expired, host hiccup,
        // etc.) - fall through to a fresh resolve instead of failing
        // outright.
        r->direct_url[0] = '\0';
    }
    return http_get_range(r->proxy_url, offset, length, out_buf, out_len,
                           r->direct_url, sizeof(r->direct_url), err_buf, err_buf_size);
}
