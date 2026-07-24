#include "ncm_install.h"
#include "../net/http.h"

#include <stdlib.h>
#include <string.h>

// On-disk CNMT layout (the file inside a Meta-type NCA, extracted via
// fsOpenFileSystemWithId below) - public format, documented on
// switchbrew.org's "NCA/Content Archive" and "CNMT" wiki pages.
typedef struct {
    uint64_t title_id;
    uint32_t version;
    uint8_t type; // NcmContentMetaType
    uint8_t reserved_0d;
    uint16_t extended_header_size;
    uint16_t content_count;
    uint16_t content_meta_count;
    uint8_t attributes;
    uint8_t storage_id;
    uint8_t install_type;
    uint8_t committed;
    uint32_t required_download_system_version;
    uint32_t reserved_1c;
} __attribute__((packed)) PackagedContentMetaHeader;

typedef struct {
    uint8_t hash[0x20];
    NcmContentInfo info;
} __attribute__((packed)) PackagedContentInfoRaw;

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ncm_parse_content_id(const char *name, NcmContentId *out) {
    for (int i = 0; i < 16; i++) {
        int hi = hex_val(name[i * 2]);
        int lo = hex_val(name[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out->c[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void ncm_format_content_id(const NcmContentId *id, char out_hex[33]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out_hex[i * 2] = hex[(id->c[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hex[id->c[i] & 0xF];
    }
    out_hex[32] = '\0';
}

// How much is read from the source file and handed to the NCM service per
// iteration during the install phase. Bigger = fewer read()+IPC round trips
// for the same file, which the install phase (re-reading the whole
// downloaded file off SD and writing it back into content storage) is
// dominated by. 4 MB balances that against how often the cancel check /
// progress update below runs (once per chunk).
#define NCM_INSTALL_CHUNK_SIZE (4 * 1024 * 1024)

bool ncm_install_content(NcmContentStorage *cs, const NcmContentId *content_id,
                          FILE *src, uint64_t file_offset, uint64_t size,
                          InstallProgressCallback cb, void *userdata,
                          bool *out_registered,
                          char *err_buf, size_t err_buf_size) {
    if (out_registered) *out_registered = false;

    bool already_has = false;
    Result rc = ncmContentStorageHas(cs, &already_has, content_id);
    if (R_SUCCEEDED(rc) && already_has) {
        return true; // already present (e.g. shared with another installed title) - not ours to roll back
    }

    NcmPlaceHolderId placeholder_id;
    rc = ncmContentStorageGeneratePlaceHolderId(cs, &placeholder_id);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageGeneratePlaceHolderId falló (0x%x)", rc);
        return false;
    }

    // Clear out any stale placeholder left behind by a previous failed attempt.
    ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);

    rc = ncmContentStorageCreatePlaceHolder(cs, content_id, &placeholder_id, (s64)size);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageCreatePlaceHolder falló (0x%x)", rc);
        return false;
    }

    rc = ncmContentStorageSetPlaceHolderSize(cs, &placeholder_id, (s64)size);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageSetPlaceHolderSize falló (0x%x)", rc);
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        return false;
    }

    if (fseek(src, (long)file_offset, SEEK_SET) != 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo posicionar en el archivo fuente");
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        return false;
    }

    static uint8_t chunk[NCM_INSTALL_CHUNK_SIZE];
    uint64_t written = 0;
    bool canceled = false;

    while (written < size) {
        uint64_t want = size - written;
        if (want > NCM_INSTALL_CHUNK_SIZE) want = NCM_INSTALL_CHUNK_SIZE;

        size_t got = fread(chunk, 1, (size_t)want, src);
        if (got != want) {
            if (err_buf) snprintf(err_buf, err_buf_size, "lectura incompleta del archivo fuente");
            ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
            return false;
        }

        rc = ncmContentStorageWritePlaceHolder(cs, &placeholder_id, written, chunk, got);
        if (R_FAILED(rc)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageWritePlaceHolder falló (0x%x)", rc);
            ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
            return false;
        }

        written += got;

        if (cb && !cb((long)size, (long)written, userdata)) {
            canceled = true;
            break;
        }
    }

    if (canceled) {
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        if (err_buf) snprintf(err_buf, err_buf_size, "instalación cancelada");
        return false;
    }

    rc = ncmContentStorageRegister(cs, content_id, &placeholder_id);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageRegister falló (0x%x)", rc);
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        return false;
    }

    // Register moves/consumes the placeholder in the common case - this is
    // just a harmless best-effort cleanup in case anything was left behind.
    ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);

    if (out_registered) *out_registered = true;
    return true;
}

typedef struct {
    NcmContentStorage *cs;
    NcmPlaceHolderId *placeholder_id;
    uint64_t flushed;      // bytes already handed to NCM (= offset for the next flush)
    uint64_t total_size;
    uint8_t *buf;          // accumulates incoming network chunks before flushing
    size_t buf_cap;
    size_t buf_len;        // bytes currently sitting in buf, not yet flushed
    InstallProgressCallback cb;
    void *userdata;
    bool canceled;
    bool ncm_failed;
    char *err_buf;
    size_t err_buf_size;
} NcaNetworkWriteCtx;

// Flushes whatever's buffered into the placeholder in one ncm write, then
// empties the buffer. Batching matters a lot here: TLS hands curl's write
// callback data in small (~16KB) records, and each ncmContentStorageWrite-
// PlaceHolder is an IPC round-trip to the NCM service backed by an SD write,
// which has far higher per-call latency than the network delivering the
// bytes - writing per-record instead of per-4MB was pinning installs at
// ~1MB/s even though the same link pulls 15-23MB/s on a PC. This mirrors the
// 4MB batching the local-file path (ncm_install_content) already does.
static bool nca_flush(NcaNetworkWriteCtx *ctx) {
    if (ctx->buf_len == 0) return true;
    Result rc = ncmContentStorageWritePlaceHolder(ctx->cs, ctx->placeholder_id,
                                                   (s64)ctx->flushed, ctx->buf, ctx->buf_len);
    if (R_FAILED(rc)) {
        if (ctx->err_buf) snprintf(ctx->err_buf, ctx->err_buf_size, "ncmContentStorageWritePlaceHolder falló (0x%x)", rc);
        ctx->ncm_failed = true;
        return false;
    }
    ctx->flushed += ctx->buf_len;
    ctx->buf_len = 0;
    return true;
}

static size_t nca_network_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    NcaNetworkWriteCtx *ctx = (NcaNetworkWriteCtx *)userdata;
    size_t add = size * nmemb;

    // A well-behaved Range response never sends more than what was asked
    // for - if it does, refuse it outright rather than silently truncating
    // (truncating would register a placeholder full of wrong/incomplete
    // data without ever surfacing an error).
    if (ctx->flushed + ctx->buf_len + add > ctx->total_size) {
        if (ctx->err_buf) snprintf(ctx->err_buf, ctx->err_buf_size,
                                    "el servidor envió más datos de los esperados para este contenido");
        ctx->ncm_failed = true;
        return 0;
    }

    const uint8_t *in = (const uint8_t *)ptr;
    size_t remaining = add;
    while (remaining > 0) {
        size_t space = ctx->buf_cap - ctx->buf_len;
        size_t take = remaining < space ? remaining : space;
        memcpy(ctx->buf + ctx->buf_len, in, take);
        ctx->buf_len += take;
        in += take;
        remaining -= take;
        if (ctx->buf_len == ctx->buf_cap && !nca_flush(ctx)) {
            return 0; // ncm_failed already set + err_buf filled by nca_flush
        }
    }

    if (ctx->cb && !ctx->cb((long)ctx->total_size, (long)(ctx->flushed + ctx->buf_len), ctx->userdata)) {
        ctx->canceled = true;
        return 0;
    }

    return add;
}

bool ncm_install_content_from_url(NcmContentStorage *cs, const NcmContentId *content_id,
                                   ResolvedUrl *ru, uint64_t file_offset, uint64_t size,
                                   InstallProgressCallback cb, void *userdata,
                                   bool *out_registered,
                                   char *err_buf, size_t err_buf_size) {
    if (out_registered) *out_registered = false;

    bool already_has = false;
    Result rc = ncmContentStorageHas(cs, &already_has, content_id);
    if (R_SUCCEEDED(rc) && already_has) {
        return true; // already present (e.g. shared with another installed title) - not ours to roll back
    }

    NcmPlaceHolderId placeholder_id;
    rc = ncmContentStorageGeneratePlaceHolderId(cs, &placeholder_id);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageGeneratePlaceHolderId falló (0x%x)", rc);
        return false;
    }

    // Clear out any stale placeholder left behind by a previous failed attempt.
    ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);

    rc = ncmContentStorageCreatePlaceHolder(cs, content_id, &placeholder_id, (s64)size);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageCreatePlaceHolder falló (0x%x)", rc);
        return false;
    }

    rc = ncmContentStorageSetPlaceHolderSize(cs, &placeholder_id, (s64)size);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageSetPlaceHolderSize falló (0x%x)", rc);
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        return false;
    }

    // 4MB flush buffer, matching the local-file path's NCM_INSTALL_CHUNK_SIZE.
    // static (not on the stack) because it's far too big for a thread stack,
    // and safe to share because installs run one NCA at a time, serially.
    static uint8_t nca_flush_buf[NCM_INSTALL_CHUNK_SIZE];

    NcaNetworkWriteCtx ctx = {
        .cs = cs,
        .placeholder_id = &placeholder_id,
        .flushed = 0,
        .total_size = size,
        .buf = nca_flush_buf,
        .buf_cap = sizeof(nca_flush_buf),
        .buf_len = 0,
        .cb = cb,
        .userdata = userdata,
        .canceled = false,
        .ncm_failed = false,
        .err_buf = err_buf,
        .err_buf_size = err_buf_size,
    };

    HttpResult hres = HTTP_OK;
    char net_err[160] = {0};
    if (size > 0) {
        // Try the cached direct link first (skips re-resolving through the
        // proxy on every single content piece - see ResolvedUrl's doc
        // comment in install_common.h). If that fails outright (not a
        // cancel, not an NCM-side failure), the cached link most likely
        // stopped working (expired, host hiccup) - reset what's been
        // written so far (ncmContentStorageWritePlaceHolder is a plain
        // offset write, safe to redo from scratch) and retry once through
        // the proxy, which re-resolves to a fresh direct link and re-seeds
        // the cache for the NCAs still to come.
        bool had_cached = ru->direct_url[0] != '\0';
        const char *first_url = had_cached ? ru->direct_url : ru->proxy_url;
        char *first_effective_out = had_cached ? NULL : ru->direct_url;
        size_t first_effective_out_size = had_cached ? 0 : sizeof(ru->direct_url);

        hres = http_get_range_streamed(first_url, file_offset, size, nca_network_write_cb, &ctx,
                                        first_effective_out, first_effective_out_size, net_err, sizeof(net_err));
        // Flush whatever's left in the buffer from a successful transfer
        // (the last chunk almost never lands exactly on a 4MB boundary).
        if (hres == HTTP_OK && !ctx.canceled && !ctx.ncm_failed) {
            nca_flush(&ctx);
        }

        if (hres != HTTP_OK && !ctx.canceled && !ctx.ncm_failed && had_cached) {
            ru->direct_url[0] = '\0';
            ctx.flushed = 0;
            ctx.buf_len = 0;
            net_err[0] = '\0';
            hres = http_get_range_streamed(ru->proxy_url, file_offset, size, nca_network_write_cb, &ctx,
                                            ru->direct_url, sizeof(ru->direct_url), net_err, sizeof(net_err));
            if (hres == HTTP_OK && !ctx.canceled && !ctx.ncm_failed) {
                nca_flush(&ctx);
            }
        }
    }

    if (ctx.canceled) {
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        if (err_buf) snprintf(err_buf, err_buf_size, "instalación cancelada");
        return false;
    }
    if (ctx.ncm_failed) {
        // err_buf was already filled in by nca_network_write_cb / nca_flush.
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        return false;
    }
    if (hres != HTTP_OK || ctx.flushed != size) {
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        if (err_buf) {
            if (net_err[0]) snprintf(err_buf, err_buf_size, "descarga falló: %s", net_err);
            else snprintf(err_buf, err_buf_size, "descarga de red incompleta");
        }
        return false;
    }

    rc = ncmContentStorageRegister(cs, content_id, &placeholder_id);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageRegister falló (0x%x)", rc);
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        return false;
    }

    ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);

    if (out_registered) *out_registered = true;
    return true;
}

bool ncm_read_content_meta(NcmContentStorage *cs, const NcmContentId *cnmt_content_id, ContentMetaInfo *out,
                            char *err_buf, size_t err_buf_size) {
    memset(out, 0, sizeof(*out));

    char nca_path[FS_MAX_PATH];
    Result rc = ncmContentStorageGetPath(cs, nca_path, sizeof(nca_path), cnmt_content_id);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageGetPath falló (0x%x)", rc);
        return false;
    }

    FsFileSystem meta_fs;
    rc = fsOpenFileSystemWithId(&meta_fs, 0, FsFileSystemType_ContentMeta, nca_path, FsContentAttributes_All);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo abrir el NCA de metadatos (0x%x)", rc);
        return false;
    }

    FsDir dir;
    rc = fsFsOpenDirectory(&meta_fs, "/", FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) {
        fsFsClose(&meta_fs);
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo abrir el directorio del NCA de metadatos (0x%x)", rc);
        return false;
    }

    char cnmt_name[FS_MAX_PATH] = {0};
    FsDirectoryEntry entry;
    s64 total_read = 0;
    while (R_SUCCEEDED(fsDirRead(&dir, &total_read, 1, &entry)) && total_read > 0) {
        size_t nlen = strlen(entry.name);
        if (nlen > 5 && strcmp(entry.name + nlen - 5, ".cnmt") == 0) {
            snprintf(cnmt_name, sizeof(cnmt_name), "%s", entry.name);
            break;
        }
    }
    fsDirClose(&dir);

    if (cnmt_name[0] == '\0') {
        fsFsClose(&meta_fs);
        if (err_buf) snprintf(err_buf, err_buf_size, "no se encontró el archivo .cnmt dentro del NCA de metadatos");
        return false;
    }

    char cnmt_path[FS_MAX_PATH];
    snprintf(cnmt_path, sizeof(cnmt_path), "/%s", cnmt_name);

    FsFile file;
    rc = fsFsOpenFile(&meta_fs, cnmt_path, FsOpenMode_Read, &file);
    if (R_FAILED(rc)) {
        fsFsClose(&meta_fs);
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo abrir %s (0x%x)", cnmt_path, rc);
        return false;
    }

    s64 cnmt_size = 0;
    fsFileGetSize(&file, &cnmt_size);

    if (cnmt_size < (s64)sizeof(PackagedContentMetaHeader) || cnmt_size > 64 * 1024) {
        fsFileClose(&file);
        fsFsClose(&meta_fs);
        if (err_buf) snprintf(err_buf, err_buf_size, "tamaño de .cnmt inesperado");
        return false;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)cnmt_size);
    if (!buf) {
        fsFileClose(&file);
        fsFsClose(&meta_fs);
        if (err_buf) snprintf(err_buf, err_buf_size, "memoria insuficiente para leer .cnmt");
        return false;
    }

    u64 bytes_read = 0;
    rc = fsFileRead(&file, 0, buf, (u64)cnmt_size, FsReadOption_None, &bytes_read);
    fsFileClose(&file);
    fsFsClose(&meta_fs);

    if (R_FAILED(rc) || (s64)bytes_read != cnmt_size) {
        free(buf);
        if (err_buf) snprintf(err_buf, err_buf_size, "lectura incompleta de .cnmt");
        return false;
    }

    const PackagedContentMetaHeader *hdr = (const PackagedContentMetaHeader *)buf;

    if ((uint64_t)cnmt_size < sizeof(PackagedContentMetaHeader) + hdr->extended_header_size) {
        free(buf);
        if (err_buf) snprintf(err_buf, err_buf_size, ".cnmt truncado (extended header)");
        return false;
    }
    if (hdr->extended_header_size > sizeof(out->raw_extended_header)) {
        free(buf);
        if (err_buf) snprintf(err_buf, err_buf_size, "extended header de .cnmt inesperadamente grande");
        return false;
    }

    memset(&out->key, 0, sizeof(out->key));
    out->key.id = hdr->title_id;
    out->key.version = hdr->version;
    out->key.type = hdr->type;
    out->key.install_type = NcmContentInstallType_Full;

    out->extended_header_size = hdr->extended_header_size;
    out->content_meta_count = hdr->content_meta_count;
    out->attributes = hdr->attributes;
    memcpy(out->raw_extended_header, buf + sizeof(PackagedContentMetaHeader), hdr->extended_header_size);

    const uint8_t *info_cursor = buf + sizeof(PackagedContentMetaHeader) + hdr->extended_header_size;
    uint64_t infos_bytes = (uint64_t)hdr->content_count * sizeof(PackagedContentInfoRaw);
    if ((uint64_t)cnmt_size < (uint64_t)(info_cursor - buf) + infos_bytes) {
        free(buf);
        if (err_buf) snprintf(err_buf, err_buf_size, ".cnmt truncado (content infos)");
        return false;
    }

    out->content_info_count = 0;
    for (int i = 0; i < hdr->content_count && out->content_info_count < NCM_MAX_CONTENT_INFOS; i++) {
        const PackagedContentInfoRaw *packaged = (const PackagedContentInfoRaw *)(info_cursor + (size_t)i * sizeof(PackagedContentInfoRaw));
        // Skip delta fragments (content_type 6) - no known installer installs these.
        if (packaged->info.content_type <= NcmContentType_LegalInformation) {
            out->content_infos[out->content_info_count++] = packaged->info;
        }
    }

    free(buf);
    return true;
}

bool ncm_commit_content_meta(NcmContentMetaDatabase *db, const ContentMetaInfo *meta,
                              const NcmContentInfo *cnmt_content_info,
                              char *err_buf, size_t err_buf_size) {
    NcmContentMetaHeader header;
    header.extended_header_size = meta->extended_header_size;
    header.content_count = (uint16_t)(meta->content_info_count + 1); // +1 for the cnmt's own record
    header.content_meta_count = meta->content_meta_count;
    header.attributes = meta->attributes;
    header.storage_id = 0;

    size_t total_size = sizeof(NcmContentMetaHeader) + meta->extended_header_size +
                         (size_t)header.content_count * sizeof(NcmContentInfo);

    uint8_t *buf = (uint8_t *)malloc(total_size);
    if (!buf) {
        if (err_buf) snprintf(err_buf, err_buf_size, "memoria insuficiente armando el registro de contenido");
        return false;
    }

    uint8_t *cursor = buf;
    memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    memcpy(cursor, meta->raw_extended_header, meta->extended_header_size);
    cursor += meta->extended_header_size;
    memcpy(cursor, cnmt_content_info, sizeof(*cnmt_content_info));
    cursor += sizeof(*cnmt_content_info);
    for (int i = 0; i < meta->content_info_count; i++) {
        memcpy(cursor, &meta->content_infos[i], sizeof(NcmContentInfo));
        cursor += sizeof(NcmContentInfo);
    }

    Result rc = ncmContentMetaDatabaseSet(db, &meta->key, buf, total_size);
    free(buf);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentMetaDatabaseSet falló (0x%x)", rc);
        return false;
    }

    rc = ncmContentMetaDatabaseCommit(db);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentMetaDatabaseCommit falló (0x%x)", rc);
        return false;
    }

    return true;
}
