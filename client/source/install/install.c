#include "install.h"
#include "../config.h"
#include "../net/http.h"

#include <mbedtls/sha256.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

static void mkdir_ignore_exists(const char *path) {
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        // Real failures (e.g. read-only SD, invalid path) surface later when
        // the caller tries to open a file inside this directory and fails.
    }
}

// Hex-encodes a 32-byte SHA-256 digest into a lowercase 65-byte buffer
// (64 hex chars + NUL).
static void sha256_hex(const unsigned char digest[32], char out_hex[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hex[(digest[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
}

static int sha256_file(const char *path, char out_hex[65]) {
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

typedef struct {
    InstallProgressCallback cb;
    void *userdata;
} ProgressThunkCtx;

static void progress_thunk(long dltotal, long dlnow, void *userdata) {
    ProgressThunkCtx *ctx = (ProgressThunkCtx *)userdata;
    if (ctx->cb) ctx->cb(dltotal, dlnow, ctx->userdata);
}

InstallResult install_app(const AppEntry *entry, const char *base_url,
                           InstallProgressCallback cb, void *userdata,
                           char *err_buf, size_t err_buf_size) {
    char dest_dir[300];
    snprintf(dest_dir, sizeof(dest_dir), "%s/%s", SWITCH_APPS_ROOT, entry->id);

    mkdir_ignore_exists(SWITCH_APPS_ROOT);
    mkdir_ignore_exists(dest_dir);

    struct statvfs st;
    if (statvfs("sdmc:/", &st) == 0) {
        unsigned long long free_bytes = (unsigned long long)st.f_bsize * st.f_bavail;
        if (entry->file_size > 0 && free_bytes < (unsigned long long)entry->file_size) {
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "not enough free space on SD card (need %ld bytes)",
                                   entry->file_size);
            return INSTALL_ERR_NO_SPACE;
        }
    }

    char part_path[512];
    snprintf(part_path, sizeof(part_path), "%s/%s.part", dest_dir, entry->nro_filename);

    char url[900];
    snprintf(url, sizeof(url), "%s%s", base_url, entry->download_url);

    ProgressThunkCtx thunk_ctx = { .cb = cb, .userdata = userdata };
    HttpResult hres = http_download_to_file(url, part_path, progress_thunk, &thunk_ctx,
                                             err_buf, err_buf_size);
    if (hres != HTTP_OK) {
        return INSTALL_ERR_DOWNLOAD;
    }

    char actual_hex[65];
    if (sha256_file(part_path, actual_hex) != 0) {
        remove(part_path);
        if (err_buf) snprintf(err_buf, err_buf_size, "could not read downloaded file to verify checksum");
        return INSTALL_ERR_DOWNLOAD;
    }

    if (strcasecmp(actual_hex, entry->sha256) != 0) {
        remove(part_path);
        if (err_buf) snprintf(err_buf, err_buf_size,
                               "checksum mismatch (expected %s, got %s) - download corrupted",
                               entry->sha256, actual_hex);
        return INSTALL_ERR_HASH_MISMATCH;
    }

    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/%s", dest_dir, entry->nro_filename);

    if (rename(part_path, final_path) != 0) {
        remove(part_path);
        if (err_buf) snprintf(err_buf, err_buf_size, "could not move downloaded file into place");
        return INSTALL_ERR_RENAME;
    }

    return INSTALL_OK;
}
