#include "zip_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <zlib.h>

#define EOCD_SIGNATURE 0x06054b50u
#define CENTRAL_DIR_SIGNATURE 0x02014b50u
#define LOCAL_FILE_SIGNATURE 0x04034b50u
#define METHOD_STORED 0
#define METHOD_DEFLATE 8

// Standard ZIP structures (see PKWARE's APPNOTE.TXT) - read via fread into
// these directly rather than a manual byte-by-byte cursor since ARM64 is
// little-endian, matching ZIP's on-disk byte order, and every field here is
// already naturally aligned within its packed size.
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

// Creates every missing directory along `path` (like `mkdir -p`).
static void mkdir_recursive(const char *path) {
    char buf[600];
    snprintf(buf, sizeof(buf), "%s", path);

    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0777);
            *p = '/';
        }
    }
    mkdir(buf, 0777);
}

// Finds the End Of Central Directory record. Scans the last 64KB + the
// record's own fixed size from the end of the file (the maximum a trailing
// comment can push it back) rather than assuming it's the very last bytes -
// tools rarely add one, but this is cheap insurance against a zip that has.
#define EOCD_SEARCH_WINDOW (64 * 1024 + (long)sizeof(EocdRecord))

static bool find_eocd(FILE *fp, long file_size, EocdRecord *out) {
    long window = file_size < EOCD_SEARCH_WINDOW ? file_size : EOCD_SEARCH_WINDOW;
    long start = file_size - window;

    uint8_t *buf = (uint8_t *)malloc((size_t)window);
    if (!buf) return false;

    if (fseek(fp, start, SEEK_SET) != 0 || fread(buf, 1, (size_t)window, fp) != (size_t)window) {
        free(buf);
        return false;
    }

    bool found = false;
    for (long i = window - (long)sizeof(EocdRecord); i >= 0; i--) {
        uint32_t sig = (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8) |
                       ((uint32_t)buf[i + 2] << 16) | ((uint32_t)buf[i + 3] << 24);
        if (sig == EOCD_SIGNATURE) {
            memcpy(out, buf + i, sizeof(EocdRecord));
            found = true;
            break;
        }
    }

    free(buf);
    return found;
}

// Reports progress against compressed bytes consumed from `zip_path` (not
// decompressed bytes produced) - simpler to total up front (a single pass
// over the compact central directory, no file data touched) and still a
// reasonable proxy for how much of the archive is left to process.
typedef struct {
    long total;
    long done;
} ZipProgress;

static bool inflate_entry(FILE *zip_fp, uint32_t compressed_size, FILE *out_fp,
                           InstallProgressCallback cb, void *userdata, ZipProgress *progress,
                           char *err_buf, size_t err_buf_size) {
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    // Negative windowBits = raw deflate, no zlib/gzip header - exactly the
    // format a ZIP entry's compressed data is stored in.
    if (inflateInit2(&strm, -15) != Z_OK) {
        if (err_buf) snprintf(err_buf, err_buf_size, "inflateInit2 falló");
        return false;
    }

    static uint8_t in_buf[16 * 1024];
    static uint8_t out_buf[64 * 1024];
    uint32_t remaining_in = compressed_size;
    bool ok = true;
    int zrc = Z_OK;

    while (zrc != Z_STREAM_END) {
        if (strm.avail_in == 0) {
            if (remaining_in == 0) {
                if (err_buf) snprintf(err_buf, err_buf_size, "entrada ZIP truncada");
                ok = false;
                break;
            }
            size_t want = remaining_in < sizeof(in_buf) ? remaining_in : sizeof(in_buf);
            size_t got = fread(in_buf, 1, want, zip_fp);
            if (got != want) {
                if (err_buf) snprintf(err_buf, err_buf_size, "lectura incompleta de datos comprimidos");
                ok = false;
                break;
            }
            remaining_in -= (uint32_t)got;
            strm.next_in = in_buf;
            strm.avail_in = (uInt)got;
            progress->done += (long)got;
        }

        strm.next_out = out_buf;
        strm.avail_out = sizeof(out_buf);

        zrc = inflate(&strm, Z_NO_FLUSH);
        if (zrc != Z_OK && zrc != Z_STREAM_END) {
            if (err_buf) snprintf(err_buf, err_buf_size, "zlib inflate falló (%d)", zrc);
            ok = false;
            break;
        }

        size_t produced = sizeof(out_buf) - strm.avail_out;
        if (produced > 0 && fwrite(out_buf, 1, produced, out_fp) != produced) {
            if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo escribir el archivo extraído");
            ok = false;
            break;
        }

        if (cb && !cb(progress->total, progress->done, userdata)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "extracción cancelada");
            ok = false;
            break;
        }
    }

    inflateEnd(&strm);
    return ok;
}

static bool extract_stored(FILE *zip_fp, uint32_t size, FILE *out_fp,
                            InstallProgressCallback cb, void *userdata, ZipProgress *progress,
                            char *err_buf, size_t err_buf_size) {
    static uint8_t buf[64 * 1024];
    uint32_t remaining = size;

    while (remaining > 0) {
        size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        size_t got = fread(buf, 1, want, zip_fp);
        if (got != want) {
            if (err_buf) snprintf(err_buf, err_buf_size, "lectura incompleta del archivo");
            return false;
        }
        if (fwrite(buf, 1, got, out_fp) != got) {
            if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo escribir el archivo extraído");
            return false;
        }
        remaining -= (uint32_t)got;
        progress->done += (long)got;
        if (cb && !cb(progress->total, progress->done, userdata)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "extracción cancelada");
            return false;
        }
    }
    return true;
}

// Reads past one central directory entry's variable-length fields
// (filename/extra/comment) without buffering the filename - used by the
// size-totaling first pass, which only needs compressed_size.
static bool skip_cd_entry_tail(FILE *fp, const CentralDirHeader *hdr) {
    return fseek(fp, (long)hdr->filename_len + hdr->extra_len + hdr->comment_len, SEEK_CUR) == 0;
}

bool zip_extract_to_dir(const char *zip_path, const char *dest_dir,
                         InstallProgressCallback cb, void *userdata,
                         char *err_buf, size_t err_buf_size) {
    FILE *zip_fp = fopen(zip_path, "rb");
    if (!zip_fp) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo abrir el archivo ZIP descargado");
        return false;
    }
    // stdio's default buffer (a few KB) turns every fread() below into that
    // many small reads against the sdmc filesystem driver - same fix as
    // http_download_to_file's own setvbuf, for the same reason.
    static char zip_read_buf[256 * 1024];
    setvbuf(zip_fp, zip_read_buf, _IOFBF, sizeof(zip_read_buf));

    if (fseek(zip_fp, 0, SEEK_END) != 0) {
        fclose(zip_fp);
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo leer el tamaño del ZIP");
        return false;
    }
    long file_size = ftell(zip_fp);

    EocdRecord eocd;
    if (!find_eocd(zip_fp, file_size, &eocd)) {
        fclose(zip_fp);
        if (err_buf) snprintf(err_buf, err_buf_size,
                               "no se encontró el índice del ZIP (archivo corrupto o formato no soportado)");
        return false;
    }

    // Pass 1: total up compressed_size across every entry for the progress
    // bar - cheap, only the compact central directory is touched.
    ZipProgress progress = { .total = 0, .done = 0 };
    if (fseek(zip_fp, (long)eocd.cd_offset, SEEK_SET) != 0) {
        fclose(zip_fp);
        if (err_buf) snprintf(err_buf, err_buf_size, "índice del ZIP inválido");
        return false;
    }
    for (int i = 0; i < eocd.cd_entries_total; i++) {
        CentralDirHeader hdr;
        if (fread(&hdr, sizeof(hdr), 1, zip_fp) != 1 || hdr.signature != CENTRAL_DIR_SIGNATURE) {
            fclose(zip_fp);
            if (err_buf) snprintf(err_buf, err_buf_size, "índice del ZIP corrupto");
            return false;
        }
        progress.total += (long)hdr.compressed_size;
        if (!skip_cd_entry_tail(zip_fp, &hdr)) {
            fclose(zip_fp);
            if (err_buf) snprintf(err_buf, err_buf_size, "índice del ZIP corrupto");
            return false;
        }
    }

    // Pass 2: actually extract, reading the central directory again (this
    // time keeping each entry's filename).
    if (fseek(zip_fp, (long)eocd.cd_offset, SEEK_SET) != 0) {
        fclose(zip_fp);
        if (err_buf) snprintf(err_buf, err_buf_size, "índice del ZIP inválido");
        return false;
    }

    bool ok = true;
    for (int i = 0; i < eocd.cd_entries_total && ok; i++) {
        CentralDirHeader hdr;
        if (fread(&hdr, sizeof(hdr), 1, zip_fp) != 1 || hdr.signature != CENTRAL_DIR_SIGNATURE) {
            if (err_buf) snprintf(err_buf, err_buf_size, "índice del ZIP corrupto");
            ok = false;
            break;
        }

        char name[512];
        if (hdr.filename_len >= sizeof(name)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "nombre de archivo demasiado largo dentro del ZIP");
            ok = false;
            break;
        }
        if (fread(name, 1, hdr.filename_len, zip_fp) != hdr.filename_len) {
            if (err_buf) snprintf(err_buf, err_buf_size, "índice del ZIP corrupto");
            ok = false;
            break;
        }
        name[hdr.filename_len] = '\0';
        if (fseek(zip_fp, hdr.extra_len + hdr.comment_len, SEEK_CUR) != 0) {
            ok = false;
            break;
        }
        long next_cd_pos = ftell(zip_fp);

        char dest_path[700];
        snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_dir, name);

        size_t name_len = strlen(name);
        bool is_dir = name_len > 0 && name[name_len - 1] == '/';

        if (is_dir) {
            mkdir_recursive(dest_path);
        } else if (hdr.method != METHOD_STORED && hdr.method != METHOD_DEFLATE) {
            if (err_buf) snprintf(err_buf, err_buf_size, "método de compresión no soportado en %s", name);
            ok = false;
            break;
        } else {
            // Create the parent directory even if the archive has no
            // explicit directory entry for it (some zip tools omit them).
            char *last_slash = strrchr(dest_path, '/');
            if (last_slash) {
                *last_slash = '\0';
                mkdir_recursive(dest_path);
                *last_slash = '/';
            }

            if (fseek(zip_fp, (long)hdr.local_header_offset, SEEK_SET) != 0) {
                if (err_buf) snprintf(err_buf, err_buf_size, "offset inválido para %s", name);
                ok = false;
                break;
            }

            LocalFileHeader local;
            if (fread(&local, sizeof(local), 1, zip_fp) != 1 || local.signature != LOCAL_FILE_SIGNATURE) {
                if (err_buf) snprintf(err_buf, err_buf_size, "cabecera local inválida para %s", name);
                ok = false;
                break;
            }
            if (fseek(zip_fp, (long)local.filename_len + local.extra_len, SEEK_CUR) != 0) {
                ok = false;
                break;
            }

            FILE *out_fp = fopen(dest_path, "wb");
            if (!out_fp) {
                if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo crear %s", dest_path);
                ok = false;
                break;
            }
            // Same reasoning as zip_read_buf above, for the extracted
            // file's writes - reused across entries, one file open at a time.
            static char out_write_buf[256 * 1024];
            setvbuf(out_fp, out_write_buf, _IOFBF, sizeof(out_write_buf));

            if (hdr.method == METHOD_STORED) {
                ok = extract_stored(zip_fp, hdr.compressed_size, out_fp, cb, userdata, &progress,
                                     err_buf, err_buf_size);
            } else {
                ok = inflate_entry(zip_fp, hdr.compressed_size, out_fp, cb, userdata, &progress,
                                    err_buf, err_buf_size);
            }

            fclose(out_fp);
        }

        if (ok && fseek(zip_fp, next_cd_pos, SEEK_SET) != 0) {
            ok = false;
        }
    }

    fclose(zip_fp);
    return ok;
}
