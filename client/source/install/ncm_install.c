#include "ncm_install.h"
#include "install_common.h"
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
// progress update below runs (once per chunk). Backed by the shared
// install scratch buffer - see install_common.h.
#define NCM_INSTALL_CHUNK_SIZE INSTALL_SCRATCH_SIZE

// Drops any content already registered under `content_id` so the caller can
// write it fresh.
//
// This used to be an optimization instead: ncmContentStorageHas() said the
// id was present, so the download was skipped entirely on the theory that a
// content id is the content's own hash and therefore can only ever name one
// exact set of bytes. That reasoning is sound for content that finished
// installing, and wrong for everything else - and "everything else" is
// reachable, because ncmContentStorageCreatePlaceHolder reserves the piece's
// full declared size up front and fills it in as bytes arrive. An install
// that dies partway (crash, sleep, a bug in an earlier build) can therefore
// leave content whose id is registered and whose *size* is exactly right,
// holding almost none of the real data.
//
// That was observed on hardware: a 16.7GB Program NCA sat registered at
// full size with ~8MB actually written, so every subsequent install skipped
// re-downloading it and reported success over a title that could never
// launch. A size check doesn't catch it (the size is right by construction),
// and the only check that would - hashing the piece - means reading
// multi-GB off the SD card on every single install, purely to guard against
// this. Re-downloading is the cheaper correct answer, and matches what the
// user asked for by starting an install at all.
static void drop_existing_content(NcmContentStorage *cs, const NcmContentId *content_id,
                                   uint64_t expected_size, const char *caller) {
    bool already_has = false;
    if (R_FAILED(ncmContentStorageHas(cs, &already_has, content_id)) || !already_has) return;

    s64 existing_size = 0;
    ncmContentStorageGetSizeFromContentId(cs, &existing_size, content_id);

    char hex[33];
    ncm_format_content_id(content_id, hex);
    download_debug_log("%s: %s already registered (size %lld, expected %llu) - deleting and "
                        "re-downloading, registered content is not proof it is complete",
                        caller, hex, (long long)existing_size, (unsigned long long)expected_size);

    ncmContentStorageDelete(cs, content_id);
}

bool ncm_install_content(NcmContentStorage *cs, const NcmContentId *content_id,
                          FILE *src, uint64_t file_offset, uint64_t size,
                          InstallProgressCallback cb, void *userdata,
                          bool *out_registered,
                          char *err_buf, size_t err_buf_size) {
    if (out_registered) *out_registered = false;

    drop_existing_content(cs, content_id, size, "ncm_install_content");

    NcmPlaceHolderId placeholder_id;
    Result rc;
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

    uint8_t *chunk = install_common_scratch();
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
    // Total nanoseconds spent inside nca_flush. Writing to NCM happens on
    // this same thread as the transfer, so every one of these nanoseconds is
    // time curl spends not reading the socket - which is both why a
    // download can average well below what the link actually delivers, and
    // why curl's own speed accounting can see a healthy connection as
    // stalled. Measuring it is the only way to tell "the host is slow" apart
    // from "we are the bottleneck".
    u64 flush_ns_total;
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
    u64 flush_start = armGetSystemTick();
    Result rc = ncmContentStorageWritePlaceHolder(ctx->cs, ctx->placeholder_id,
                                                   (s64)ctx->flushed, ctx->buf, ctx->buf_len);
    ctx->flush_ns_total += armTicksToNs(armGetSystemTick() - flush_start);
    if (R_FAILED(rc)) {
        // Logged with the raw Result: curl only ever surfaces this as a
        // generic "failed writing received data", which is indistinguishable
        // from the two other ways nca_network_write_cb can refuse a chunk,
        // and the actual NCM error code is what says whether it's out of
        // space, a bad placeholder, or something else entirely.
        download_debug_log("  nca_flush FAILED: ncmContentStorageWritePlaceHolder(offset=%llu, len=%zu) "
                            "rc=0x%x", (unsigned long long)ctx->flushed, ctx->buf_len, rc);
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
        download_debug_log("  write_cb REFUSED: server overran the requested range "
                            "(flushed=%llu buffered=%zu incoming=%zu total=%llu)",
                            (unsigned long long)ctx->flushed, ctx->buf_len, add,
                            (unsigned long long)ctx->total_size);
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
        download_debug_log("  write_cb: canceled by user at %llu bytes",
                            (unsigned long long)(ctx->flushed + ctx->buf_len));
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

    drop_existing_content(cs, content_id, size, "ncm_install_content_from_url");

    // Free space up front: a write that starts fine and fails partway is
    // exactly what running out of room looks like, and NCM reserves a
    // piece's full size at CreatePlaceHolder time without necessarily
    // failing there if the space isn't really available.
    {
        s64 free_space = 0;
        if (R_SUCCEEDED(ncmContentStorageGetFreeSpaceSize(cs, &free_space))) {
            download_debug_log("  ncm free space: %lld bytes (piece needs %llu)",
                                (long long)free_space, (unsigned long long)size);
        }
    }

    NcmPlaceHolderId placeholder_id;
    Result rc = ncmContentStorageGeneratePlaceHolderId(cs, &placeholder_id);
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
    // Shared (not on the stack) because it's far too big for a thread stack,
    // and safe to share because installs run one NCA at a time, serially.
    uint8_t *nca_flush_buf = install_common_scratch();

    NcaNetworkWriteCtx ctx = {
        .cs = cs,
        .placeholder_id = &placeholder_id,
        .flushed = 0,
        .total_size = size,
        .buf = nca_flush_buf,
        .buf_cap = INSTALL_SCRATCH_SIZE,
        .buf_len = 0,
        .cb = cb,
        .userdata = userdata,
        .canceled = false,
        .ncm_failed = false,
        .err_buf = err_buf,
        .err_buf_size = err_buf_size,
        .flush_ns_total = 0,
    };

    // How many attempts in a row may fail *without transferring anything*
    // before giving up. Deliberately counted per stall rather than in total:
    // MediaFire routinely cuts a long transfer partway (observed dropping a
    // 4.28GB piece at ~226MB, then ~87MB), so a total cap would doom any
    // large content to never finishing no matter how well each attempt
    // went. Attempts that make progress don't count against this at all -
    // only a link that delivers nothing at all, repeatedly, ends the loop.
    #define NCM_RANGE_STALL_RETRIES 6

    HttpResult hres = HTTP_OK;
    char net_err[160] = {0};
    if (size > 0) {
        // Resolve from this console where that's possible, so the link isn't
        // one the server resolved against its own IP (which MediaFire then
        // refuses to serve here) - see resolved_url_ensure_direct.
        resolved_url_ensure_direct(ru);

        int stalled = 0;
        bool first_attempt = true;

        // Resumes rather than restarts. ctx.flushed is how much has actually
        // been committed to the placeholder, and NCM writes are plain
        // offset writes, so a dropped transfer only costs the bytes that
        // hadn't been flushed yet - the next attempt asks for the remainder
        // and carries on. Restarting from zero each time (as this used to)
        // meant a piece bigger than whatever the host was willing to serve
        // in one go could never complete, however many retries it got.
        while (ctx.flushed < size && !ctx.canceled && !ctx.ncm_failed && stalled < NCM_RANGE_STALL_RETRIES) {
            // Only resolve a new link when the current one has stopped
            // delivering anything. Re-resolving costs a full page fetch
            // from MediaFire, so doing it after every hiccup turned each
            // recovery into a multi-second stall of its own - and it's
            // wasted work whenever the link is still fine, which is
            // precisely what an attempt that transferred data proves. A
            // host that merely throttles or drops long transfers (MediaFire
            // does both) is best answered by reconnecting to the same link;
            // only a link that yields nothing at all is worth replacing.
            // `stalled == 0` means the previous attempt moved data, so the
            // link is demonstrably still good; keep using it.
            bool link_still_good = ru->direct_url[0] != '\0' && (first_attempt || stalled == 0);
            const char *url = link_still_good ? ru->direct_url : resolved_url_refresh(ru);
            first_attempt = false;

            uint64_t before = ctx.flushed;
            u64 flush_ns_before = ctx.flush_ns_total;
            ctx.buf_len = 0; // drop the unflushed tail of a failed attempt; refetched below
            net_err[0] = '\0';

            hres = http_get_range_streamed(url, file_offset + ctx.flushed, size - ctx.flushed,
                                            nca_network_write_cb, &ctx, NULL, 0, net_err, sizeof(net_err));

            // Commit whatever did arrive, successful attempt or not - it's
            // contiguous data starting exactly at ctx.flushed either way,
            // and keeping it is what lets the next attempt pick up further
            // along instead of covering the same ground again.
            if (!ctx.canceled && !ctx.ncm_failed) {
                nca_flush(&ctx);
            }

            if (ctx.flushed > before) {
                stalled = 0;
                download_debug_log("  resume: %llu/%llu bytes committed (+%llu this attempt, "
                                    "%.1fs of it blocked writing to NCM)",
                                    (unsigned long long)ctx.flushed, (unsigned long long)size,
                                    (unsigned long long)(ctx.flushed - before),
                                    (ctx.flush_ns_total - flush_ns_before) / 1e9);
            } else {
                stalled++;
            }
        }

        // A transfer that ended short but whose flush completed the piece is
        // a success - the last attempt's CURLcode says nothing useful then.
        if (ctx.flushed == size) hres = HTTP_OK;
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

    // The actual gate deciding whether this content piece is considered
    // complete and gets registered - logged unconditionally (not just on
    // failure) so a "some app installs before fully downloaded" report can
    // be checked against what this function itself believed happened, not
    // just what curl reported lower down in http.c.
    download_debug_log("ncm_install_content_from_url: expected_size=%llu flushed=%llu hres=%d -> %s",
                        (unsigned long long)size, (unsigned long long)ctx.flushed, (int)hres,
                        (hres == HTTP_OK && ctx.flushed == size) ? "OK" : "INCOMPLETE");

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

    // Struct sizes logged alongside the header fields they're used with: the
    // whole content table is walked by striding sizeof(PackagedContentInfoRaw)
    // from a base derived from sizeof(PackagedContentMetaHeader), so if
    // either is off by even a byte (a stray alignment hole surviving
    // __attribute__((packed)), say) every entry past the first reads from
    // the wrong place - which is indistinguishable, from the outside, from
    // a title that genuinely only has small pieces.
    download_debug_log("  cnmt layout: header=%zu ext_header=%u info_entry=%zu content_count=%u",
                        sizeof(PackagedContentMetaHeader), (unsigned)hdr->extended_header_size,
                        sizeof(PackagedContentInfoRaw), (unsigned)hdr->content_count);

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

        // Every parsed entry, before any filtering: two entries coming out
        // with the same content id (which is what a title installing only
        // its small pieces looks like) means this parse is walking the
        // table wrong, and the id/type/size triplet is what shows where.
        // Copied out of the packed struct before use - taking the address of
        // a packed member yields a possibly-unaligned pointer, which these
        // helpers aren't written to accept.
        NcmContentInfo raw_info = packaged->info;
        u64 raw_size = 0;
        ncmContentInfoSizeToU64(&raw_info, &raw_size);
        char raw_hex[33];
        ncm_format_content_id(&raw_info.content_id, raw_hex);
        download_debug_log("  cnmt entry[%d] @0x%zx: id=%s type=%u size=%llu",
                            i, (size_t)((const uint8_t *)packaged - buf), raw_hex,
                            (unsigned)raw_info.content_type, (unsigned long long)raw_size);

        // Skip delta fragments (content_type 6) - no known installer installs these.
        if (packaged->info.content_type <= NcmContentType_LegalInformation) {
            out->content_infos[out->content_info_count++] = packaged->info;
        }
    }

    // The single most useful number for a report of "installed with most of
    // the title missing": how many content pieces the cnmt itself claims to
    // have, versus how many actually ended up in out->content_infos after
    // the delta-fragment filter. If content_count is already small here,
    // the cnmt read out of the just-installed Meta NCA genuinely says the
    // title only has this many pieces - the bug (or bad source file) is
    // upstream of this function, not in the install loop that follows it.
    download_debug_log("ncm_read_content_meta: title_id=%016llx cnmt_size=%lld content_count=%u "
                        "content_info_count=%d",
                        (unsigned long long)hdr->title_id, (long long)cnmt_size,
                        (unsigned)hdr->content_count, out->content_info_count);

    // A content id is the content's own hash, so two entries can never
    // legitimately share one - different content is different bytes is a
    // different id, by construction. A file whose cnmt does list the same id
    // twice is malformed, and the shape it takes in practice is nasty:
    // observed on hardware, a 16.7GB "game" whose cnmt named an 834KB piece
    // as *both* its Program and its Control, leaving the actual multi-GB NCA
    // in the container unreferenced. Every installer faithfully following
    // that cnmt (this one, DBI, any other) installs only the small piece and
    // has no reason to think anything went wrong - the title then appears on
    // the home menu and cannot launch. Rejecting it here turns that silent
    // wrong "success" into a clear failure that names the real cause.
    for (int i = 0; i < out->content_info_count; i++) {
        for (int j = i + 1; j < out->content_info_count; j++) {
            if (memcmp(out->content_infos[i].content_id.c,
                        out->content_infos[j].content_id.c,
                        sizeof(out->content_infos[i].content_id.c)) != 0) {
                continue;
            }
            char hex[33];
            ncm_format_content_id(&out->content_infos[i].content_id, hex);
            download_debug_log("ncm_read_content_meta: REJECTED - entries %d and %d share content id %s",
                                i, j, hex);
            if (err_buf) {
                snprintf(err_buf, err_buf_size,
                         "este archivo está mal armado: su índice interno declara dos veces el mismo "
                         "contenido (%s) y no referencia el resto del juego, así que instalarlo dejaría "
                         "un título que no abre. Consigue otra copia del archivo.",
                         hex);
            }
            free(buf);
            return false;
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
