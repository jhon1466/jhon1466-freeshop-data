#include "zip_create.h"

#include <switch.h> // FS_MAX_PATH
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <zlib.h>

#define EOCD_SIGNATURE 0x06054b50u
#define CENTRAL_DIR_SIGNATURE 0x02014b50u
#define LOCAL_FILE_SIGNATURE 0x04034b50u
#define METHOD_DEFLATE 8

// Same on-disk layout as zip_extract.c's structs (see PKWARE's APPNOTE.TXT)
// - duplicated rather than shared between the two files, matching how this
// project generally prefers a small local copy over a new shared header for
// something this size.
typedef struct {
    uint32_t signature;
    uint16_t disk_number;
    uint16_t cd_start_disk;
    uint16_t cd_entries_this_disk;
    uint16_t cd_entries_total;
    uint32_t cd_size;
    uint32_t cd_offset;
    uint16_t comment_len;
} __attribute__((packed)) EocdRecord;

typedef struct {
    uint32_t signature;
    uint16_t version_made_by;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t method;
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_len;
    uint16_t extra_len;
    uint16_t comment_len;
    uint16_t disk_number_start;
    uint16_t internal_attrs;
    uint32_t external_attrs;
    uint32_t local_header_offset;
} __attribute__((packed)) CentralDirHeader;

typedef struct {
    uint32_t signature;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t method;
    uint16_t mod_time;
    uint16_t mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_len;
    uint16_t extra_len;
} __attribute__((packed)) LocalFileHeader;

_Static_assert(sizeof(EocdRecord) == 22, "EocdRecord must be 22 bytes");
_Static_assert(sizeof(CentralDirHeader) == 46, "CentralDirHeader must be 46 bytes");
_Static_assert(sizeof(LocalFileHeader) == 30, "LocalFileHeader must be 30 bytes");
// The three fields patched after compressing (see deflate_file_into_zip)
// must be contiguous with no padding between them for the single patch
// write below to land correctly - true as long as this stays packed.
_Static_assert(offsetof(LocalFileHeader, crc32) + 4 == offsetof(LocalFileHeader, compressed_size) &&
               offsetof(LocalFileHeader, compressed_size) + 4 == offsetof(LocalFileHeader, uncompressed_size),
               "LocalFileHeader's crc32/compressed_size/uncompressed_size must be contiguous");

// Generous - a save is normally a few dozen to a few hundred files; past
// this the archive is aborted (see the check in add_file) rather than
// silently written incomplete.
#define ZIP_CREATE_ENTRIES_MAX 4096
#define ZIP_ENTRY_NAME_MAX 256

typedef struct {
    char name[ZIP_ENTRY_NAME_MAX];
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_header_offset;
} ZipEntryRecord;

typedef struct {
    FILE *out_fp;
    ZipEntryRecord *entries; // malloc'd - ZIP_CREATE_ENTRIES_MAX * sizeof(ZipEntryRecord) is a few hundred KB
    int entry_count;
    InstallProgressCallback cb;
    void *userdata;
    long total;
    long done;
    bool canceled; // distinct from a real failure - see zip_create_from_dir's error-message handling
    char *err_buf;
    size_t err_buf_size;
} ZipWriteCtx;

// Same recursion shape as save_backup.c's dir_size_recursive - duplicated
// rather than shared, same reasoning as the struct duplication above.
static long dir_size_recursive(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return 0;

    long total = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char child[FS_MAX_PATH];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);

        struct stat st;
        if (stat(child, &st) != 0) continue;
        total += S_ISDIR(st.st_mode) ? dir_size_recursive(child) : (long)st.st_size;
    }
    closedir(dir);
    return total;
}

// Compresses `src_path`'s contents into a new entry named `entry_name`,
// appended to ctx->out_fp at its current position. Writes the local header
// with crc32/sizes as 0 first (they aren't known until the data is fully
// read), then seeks back to patch them in once they are - simpler than
// buffering a whole file in memory first, and still produces a local header
// real unzip tools can read directly rather than requiring the streaming
// "data descriptor" trailer form of the format.
static bool add_file(ZipWriteCtx *ctx, const char *src_path, const char *entry_name) {
    if (ctx->entry_count >= ZIP_CREATE_ENTRIES_MAX) {
        if (ctx->err_buf && ctx->err_buf[0] == '\0') {
            snprintf(ctx->err_buf, ctx->err_buf_size, "demasiados archivos para comprimir (más de %d)",
                     ZIP_CREATE_ENTRIES_MAX);
        }
        return false;
    }
    if (strlen(entry_name) >= ZIP_ENTRY_NAME_MAX) {
        if (ctx->err_buf && ctx->err_buf[0] == '\0') {
            snprintf(ctx->err_buf, ctx->err_buf_size, "nombre de archivo demasiado largo: %s", entry_name);
        }
        return false;
    }

    FILE *in_fp = fopen(src_path, "rb");
    if (!in_fp) {
        if (ctx->err_buf && ctx->err_buf[0] == '\0') {
            snprintf(ctx->err_buf, ctx->err_buf_size, "no se pudo abrir %s", entry_name);
        }
        return false;
    }

    long header_pos = ftell(ctx->out_fp);

    LocalFileHeader local;
    memset(&local, 0, sizeof(local));
    local.signature = LOCAL_FILE_SIGNATURE;
    local.version_needed = 20;
    local.method = METHOD_DEFLATE;
    local.filename_len = (uint16_t)strlen(entry_name);

    if (fwrite(&local, sizeof(local), 1, ctx->out_fp) != 1 ||
        fwrite(entry_name, 1, local.filename_len, ctx->out_fp) != local.filename_len) {
        fclose(in_fp);
        if (ctx->err_buf && ctx->err_buf[0] == '\0') {
            snprintf(ctx->err_buf, ctx->err_buf_size, "no se pudo escribir el encabezado ZIP para %s", entry_name);
        }
        return false;
    }

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    // Negative windowBits = raw deflate, no zlib/gzip header - the format a
    // ZIP entry's compressed data is stored in (mirrors inflateInit2(-15)
    // in zip_extract.c's reader).
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        fclose(in_fp);
        if (ctx->err_buf && ctx->err_buf[0] == '\0') snprintf(ctx->err_buf, ctx->err_buf_size, "deflateInit2 falló");
        return false;
    }

    static uint8_t in_buf[64 * 1024];
    static uint8_t out_buf[64 * 1024];
    uint32_t crc = crc32(0L, Z_NULL, 0);
    uint32_t uncompressed = 0, compressed = 0;
    bool ok = true;
    int flush = Z_NO_FLUSH;

    while (flush != Z_FINISH) {
        size_t got = fread(in_buf, 1, sizeof(in_buf), in_fp);
        if (got < sizeof(in_buf)) {
            if (ferror(in_fp)) {
                ok = false;
                if (ctx->err_buf && ctx->err_buf[0] == '\0') {
                    snprintf(ctx->err_buf, ctx->err_buf_size, "no se pudo leer %s", entry_name);
                }
                break;
            }
            flush = Z_FINISH;
        }
        crc = crc32(crc, in_buf, (uInt)got);
        uncompressed += (uint32_t)got;
        strm.next_in = in_buf;
        strm.avail_in = (uInt)got;

        do {
            strm.next_out = out_buf;
            strm.avail_out = sizeof(out_buf);
            if (deflate(&strm, flush) == Z_STREAM_ERROR) {
                ok = false;
                if (ctx->err_buf && ctx->err_buf[0] == '\0') {
                    snprintf(ctx->err_buf, ctx->err_buf_size, "zlib deflate falló en %s", entry_name);
                }
                break;
            }
            size_t produced = sizeof(out_buf) - strm.avail_out;
            if (produced > 0) {
                if (fwrite(out_buf, 1, produced, ctx->out_fp) != produced) {
                    ok = false;
                    if (ctx->err_buf && ctx->err_buf[0] == '\0') {
                        snprintf(ctx->err_buf, ctx->err_buf_size, "no se pudo escribir %s", entry_name);
                    }
                    break;
                }
                compressed += (uint32_t)produced;
            }
        } while (strm.avail_out == 0);
        if (!ok) break;

        ctx->done += (long)got;
        if (ctx->cb && !ctx->cb(ctx->total, ctx->done, ctx->userdata)) {
            ctx->canceled = true;
            ok = false;
            break;
        }
    }

    deflateEnd(&strm);
    fclose(in_fp);
    if (!ok) return false;

    long end_pos = ftell(ctx->out_fp);
    uint32_t patch[3] = { crc, compressed, uncompressed };
    if (fseek(ctx->out_fp, header_pos + (long)offsetof(LocalFileHeader, crc32), SEEK_SET) != 0 ||
        fwrite(patch, sizeof(patch), 1, ctx->out_fp) != 1 ||
        fseek(ctx->out_fp, end_pos, SEEK_SET) != 0) {
        if (ctx->err_buf && ctx->err_buf[0] == '\0') {
            snprintf(ctx->err_buf, ctx->err_buf_size, "no se pudo actualizar el encabezado ZIP para %s", entry_name);
        }
        return false;
    }

    ZipEntryRecord *rec = &ctx->entries[ctx->entry_count++];
    snprintf(rec->name, sizeof(rec->name), "%s", entry_name);
    rec->crc32 = crc;
    rec->compressed_size = compressed;
    rec->uncompressed_size = uncompressed;
    rec->local_header_offset = (uint32_t)header_pos;
    return true;
}

// `root_len` is how many leading characters of every path under `dir_path`
// belong to the archive's root (src_dir plus its separating '/') - stripped
// off so entry names are relative ("data/save.bin"), not absolute
// ("save:/data/save.bin").
static bool add_dir_recursive(ZipWriteCtx *ctx, const char *dir_path, size_t root_len) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        if (ctx->err_buf && ctx->err_buf[0] == '\0') snprintf(ctx->err_buf, ctx->err_buf_size, "no se pudo abrir %s", dir_path);
        return false;
    }

    bool ok = true;
    struct dirent *ent;
    while (!ctx->canceled && (ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char child[FS_MAX_PATH];
        snprintf(child, sizeof(child), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        bool is_dir = (stat(child, &st) == 0) && S_ISDIR(st.st_mode);

        if (is_dir) {
            if (!add_dir_recursive(ctx, child, root_len)) ok = false;
        } else if (!add_file(ctx, child, child + root_len)) {
            ok = false;
        }
    }
    closedir(dir);
    return ok;
}

bool zip_create_from_dir(const char *src_dir, const char *zip_path,
                          InstallProgressCallback cb, void *userdata,
                          char *err_buf, size_t err_buf_size) {
    if (err_buf && err_buf_size > 0) err_buf[0] = '\0';

    FILE *out_fp = fopen(zip_path, "wb");
    if (!out_fp) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo crear %s", zip_path);
        return false;
    }
    // Same reasoning as zip_extract.c's own setvbuf calls - stdio's default
    // buffer turns every fwrite() below into that many small writes against
    // the sdmc filesystem driver.
    static char write_buf[256 * 1024];
    setvbuf(out_fp, write_buf, _IOFBF, sizeof(write_buf));

    ZipEntryRecord *entries = (ZipEntryRecord *)malloc(sizeof(ZipEntryRecord) * ZIP_CREATE_ENTRIES_MAX);
    if (!entries) {
        fclose(out_fp);
        remove(zip_path);
        if (err_buf) snprintf(err_buf, err_buf_size, "sin memoria suficiente");
        return false;
    }

    size_t root_len = strlen(src_dir);
    if (root_len == 0 || src_dir[root_len - 1] != '/') root_len += 1; // skip the '/' a joined child path would have

    ZipWriteCtx ctx = {
        .out_fp = out_fp, .entries = entries, .entry_count = 0,
        .cb = cb, .userdata = userdata, .total = dir_size_recursive(src_dir), .done = 0,
        .canceled = false, .err_buf = err_buf, .err_buf_size = err_buf_size,
    };

    bool ok = add_dir_recursive(&ctx, src_dir, root_len);

    if (ok) {
        long cd_offset = ftell(out_fp);
        for (int i = 0; i < ctx.entry_count && ok; i++) {
            const ZipEntryRecord *e = &ctx.entries[i];
            CentralDirHeader hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.signature = CENTRAL_DIR_SIGNATURE;
            hdr.version_made_by = 20;
            hdr.version_needed = 20;
            hdr.method = METHOD_DEFLATE;
            hdr.crc32 = e->crc32;
            hdr.compressed_size = e->compressed_size;
            hdr.uncompressed_size = e->uncompressed_size;
            hdr.filename_len = (uint16_t)strlen(e->name);
            hdr.local_header_offset = e->local_header_offset;

            if (fwrite(&hdr, sizeof(hdr), 1, out_fp) != 1 ||
                fwrite(e->name, 1, hdr.filename_len, out_fp) != hdr.filename_len) {
                ok = false;
                if (err_buf && err_buf[0] == '\0') snprintf(err_buf, err_buf_size, "no se pudo escribir el índice ZIP");
            }
        }

        if (ok) {
            long cd_size = ftell(out_fp) - cd_offset;
            EocdRecord eocd;
            memset(&eocd, 0, sizeof(eocd));
            eocd.signature = EOCD_SIGNATURE;
            eocd.cd_entries_this_disk = (uint16_t)ctx.entry_count;
            eocd.cd_entries_total = (uint16_t)ctx.entry_count;
            eocd.cd_size = (uint32_t)cd_size;
            eocd.cd_offset = (uint32_t)cd_offset;
            if (fwrite(&eocd, sizeof(eocd), 1, out_fp) != 1) {
                ok = false;
                if (err_buf && err_buf[0] == '\0') snprintf(err_buf, err_buf_size, "no se pudo escribir el final del ZIP");
            }
        }
    } else if (ctx.canceled && err_buf && err_buf[0] == '\0') {
        snprintf(err_buf, err_buf_size, "compresión cancelada");
    }

    free(entries);
    fclose(out_fp);
    if (!ok) remove(zip_path); // don't leave a half-written/corrupt archive behind
    return ok;
}
