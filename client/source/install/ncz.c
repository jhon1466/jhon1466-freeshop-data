// NCZ (compressed NCA, the payload format inside an .nsz/.xcz) support.
//
// An .ncz entry is a normal NCA with most of its body replaced by a zstd
// stream. Layout, verified against nsz's own reference decompressor
// (decompress.py, https://github.com/nicoboss/nsz and forks):
//
//   [0x0000, 0x4000)  the original NCA header, byte-for-byte unchanged -
//                      still just as encrypted as a real NCA's header
//                      always is, nsz never touches it.
//   [0x4000, ...)      "NCZSECTN" (8 bytes) + section count (u64 LE), then
//                      that many 64-byte section entries:
//                        offset      u64 LE  (absolute in the final NCA)
//                        size        u64 LE
//                        cryptoType  u64 LE  (1 = plaintext, 3/4 = AES-CTR)
//                        padding     u64 LE  (unused)
//                        cryptoKey     16 bytes
//                        cryptoCounter 16 bytes
//   [end of table, EOF) a single zstd stream, decompressing to exactly
//                      final_size - 0x4000 bytes: the NCA body in
//                      PLAINTEXT (nsz decrypts before compressing, since
//                      encrypted bytes don't compress).
//
// Reinstalling is: decompress that stream back to plaintext, then for each
// section (skipping cryptoType 1), AES-128-CTR re-encrypt it in place using
// the key/counter given right there in the section entry. That's the whole
// point of storing them in the file - reversing an NCZ needs no title or
// console keys at all, unlike installing a fresh, never-before-seen title
// would. NCA section offsets are always a multiple of the 0x200-byte media
// unit (a stock NCA convention), so every section starts on a 16-byte AES
// block boundary - the CTR counter never needs a fractional in-block offset
// at a section's start.
//
// This streams throughout, exactly like ncm_install_content_from_url() in
// ncm_install.c: network bytes arrive -> fed into zstd's streaming decoder
// -> each decompressed chunk is split at section boundaries and
// AES-CTR'd (or passed through, for gaps and cryptoType 1) -> batched into
// NCM placeholder writes. Nothing about the (potentially multi-GB)
// decompressed NCA is ever buffered whole - only ever a bounded, constant
// amount of it at a time.

#include "ncz.h"
#include "ncm_install.h"
#include "../net/http.h"

#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#define ZSTD_STATIC_LINKING_ONLY // ZSTD_getErrorCode / ZSTD_ErrorCode
#include <zstd.h>
#include <zstd_errors.h>
#include <stdlib.h>
#include <string.h>

#define NCA_HEADER_SIZE 0x4000
#define NCZ_SECTION_ENTRY_SIZE 64
// Upper bound on the NCZSECTN table. These are *crypto* regions, not the
// NCA's four filesystem sections - nsz emits one per contiguous run sharing
// a key/counter, so a real game routinely has well over a dozen (19 seen on
// hardware). 256 is generous headroom while still bounding the table's size
// and the prefetch below; at 56 bytes each that's ~14KB.
#define NCZ_MAX_SECTIONS 256
// NCA header + magic/count (16) + the whole section table, fetched in one
// bounded request - same idea as install_nsp_native.c's
// PFS0_HEADER_PREFETCH_BYTES.
#define NCZ_HEADER_PREFETCH (NCA_HEADER_SIZE + 16 + NCZ_MAX_SECTIONS * NCZ_SECTION_ENTRY_SIZE)

// Decompressed-output staging chunk. Kept modest on purpose: this is BSS
// that stays resident, and every megabyte here is a megabyte zstd can't
// have for its window buffer (see NCZ_MAX_WINDOW_SIZE below). The NCM
// write batching that actually needs to be large is done downstream, into
// the shared 4MB install scratch buffer.
#define NCZ_ZSTD_OUT_CHUNK (128 * 1024)

// Headroom on top of zstd's window buffer: the decoder also allocates a
// block buffer and literals area of its own, and the transfer still needs
// curl/mbedtls buffers. The large staging buffers this installer uses are
// BSS (see install_common_scratch), not heap, so they don't figure in here.
#define NCZ_MEMORY_HEADROOM (8 * 1024 * 1024)

// Whether an allocation this large can actually succeed right now.
//
// Deliberately a real malloc rather than kernel memory counters: libnx
// claims the whole available heap from the kernel at startup
// (svcSetHeapSize), so InfoType_UsedMemorySize counts that entire claimed
// heap as "used" no matter how much of it malloc still has free. Checking
// TotalMemorySize - UsedMemorySize therefore reports only *unmapped*
// memory, which on real hardware came back as ~136MB and refused a 128MB
// window that the heap itself could have served without trouble. Asking
// the allocator directly is the only measurement that answers the actual
// question.
static bool ncz_can_allocate(size_t bytes) {
    void *probe = malloc(bytes);
    if (!probe) return false;
    free(probe);
    return true;
}

typedef struct {
    uint64_t offset;
    uint64_t size;
    uint64_t crypto_type;
    uint8_t key[16];
    uint8_t counter[16];
} NczSection;

static uint64_t read_u64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

// Parses the NCA header + NCZSECTN table out of `buf` (the first
// NCZ_HEADER_PREFETCH-ish bytes of a .ncz entry). *out_stream_start is the
// byte offset (within the .ncz entry, not the whole NSZ) where the zstd
// stream begins.
static bool parse_ncz_sections(const uint8_t *buf, size_t buf_len, NczSection *out_sections,
                                int *out_count, size_t *out_stream_start,
                                char *err_buf, size_t err_buf_size) {
    if (buf_len < NCA_HEADER_SIZE + 16) {
        if (err_buf) snprintf(err_buf, err_buf_size, "archivo NCZ demasiado pequeño (encabezado incompleto)");
        return false;
    }

    const uint8_t *p = buf + NCA_HEADER_SIZE;
    if (memcmp(p, "NCZSECTN", 8) != 0) {
        if (err_buf) snprintf(err_buf, err_buf_size,
                               "no se encontró la firma NCZSECTN - este NCZ usa un formato no soportado "
                               "(¿comprimido por bloques?)");
        return false;
    }

    uint64_t count = read_u64_le(p + 8);
    if (count == 0 || count > NCZ_MAX_SECTIONS) {
        if (err_buf) snprintf(err_buf, err_buf_size,
                               "número de secciones NCZ inesperado (%llu, máximo %d)",
                               (unsigned long long)count, NCZ_MAX_SECTIONS);
        return false;
    }

    size_t table_end = NCA_HEADER_SIZE + 16 + (size_t)count * NCZ_SECTION_ENTRY_SIZE;
    if (buf_len < table_end) {
        if (err_buf) snprintf(err_buf, err_buf_size, "archivo NCZ demasiado pequeño (tabla de secciones incompleta)");
        return false;
    }

    const uint8_t *s = p + 16;
    for (uint64_t i = 0; i < count; i++) {
        NczSection *sec = &out_sections[i];
        sec->offset = read_u64_le(s + 0);
        sec->size = read_u64_le(s + 8);
        sec->crypto_type = read_u64_le(s + 16);
        // s+24..32 is padding, ignored.
        memcpy(sec->key, s + 32, 16);
        memcpy(sec->counter, s + 48, 16);
        s += NCZ_SECTION_ENTRY_SIZE;
    }

    // Insertion sort by offset so the streaming pass below,
    // which walks sections in lockstep with a monotonically increasing NCA
    // offset, can just advance an index forward and never search backward -
    // the table's on-disk order isn't guaranteed to already be sorted.
    for (int i = 1; i < (int)count; i++) {
        NczSection key = out_sections[i];
        int j = i - 1;
        while (j >= 0 && out_sections[j].offset > key.offset) {
            out_sections[j + 1] = out_sections[j];
            j--;
        }
        out_sections[j + 1] = key;
    }

    *out_count = (int)count;
    *out_stream_start = table_end;
    return true;
}

typedef struct {
    NcmContentStorage *cs;
    NcmPlaceHolderId *placeholder_id;

    ZSTD_DCtx *zds;

    NczSection sections[NCZ_MAX_SECTIONS];
    int section_count;
    int cur_section;

    // AES-CTR state for whichever section is currently active - (re)keyed
    // every time cur_section advances into a new one. mbedtls_aes_crypt_ctr
    // mutates nonce_counter/stream_block/nc_off in place as bytes are
    // processed, so these must persist across calls to correctly resume mid
    // section (the usual case - zstd's output chunks are far smaller than a
    // typical section).
    mbedtls_aes_context aes;
    int keyed_section; // index `aes` is currently keyed for, -1 when none
    size_t nc_off;
    uint8_t nonce_counter[16];
    uint8_t stream_block[16];

    uint64_t abs_offset; // next NCA-absolute byte position to produce
    uint64_t final_size;
    uint64_t window_size; // what this frame's header says zstd will allocate, for error messages

    // Running SHA-256 over exactly the bytes handed to NCM. An NCA's content
    // id is the first 16 bytes of its own SHA-256, so hashing as we go and
    // comparing at the end proves the decompress + re-encrypt round trip
    // reproduced the original NCA byte for byte - the only way to catch a
    // subtly wrong reconstruction before registering it. Getting this wrong
    // silently is what leaves a title on the home menu that the console
    // then refuses to launch ("debes tener el programa instalado").
    mbedtls_sha256_context sha;

    uint8_t *flush_buf;
    size_t flush_cap;
    size_t flush_len;
    uint64_t flushed;

    InstallProgressCallback cb;
    void *userdata;
    bool canceled;
    bool failed;
    char *err_buf;
    size_t err_buf_size;

    uint8_t zstd_out[NCZ_ZSTD_OUT_CHUNK];
} NczStreamCtx;

static bool ncz_flush(NczStreamCtx *ctx) {
    if (ctx->flush_len == 0) return true;
    Result rc = ncmContentStorageWritePlaceHolder(ctx->cs, ctx->placeholder_id,
                                                   (s64)ctx->flushed, ctx->flush_buf, ctx->flush_len);
    if (R_FAILED(rc)) {
        if (ctx->err_buf) snprintf(ctx->err_buf, ctx->err_buf_size, "ncmContentStorageWritePlaceHolder falló (0x%x)", rc);
        ctx->failed = true;
        return false;
    }
    ctx->flushed += ctx->flush_len;
    ctx->flush_len = 0;
    return true;
}

static bool ncz_output_bytes(NczStreamCtx *ctx, const uint8_t *data, size_t len) {
    mbedtls_sha256_update(&ctx->sha, data, len);

    size_t remaining = len;
    while (remaining > 0) {
        size_t space = ctx->flush_cap - ctx->flush_len;
        size_t take = remaining < space ? remaining : space;
        memcpy(ctx->flush_buf + ctx->flush_len, data, take);
        ctx->flush_len += take;
        data += take;
        remaining -= take;
        if (ctx->flush_len == ctx->flush_cap && !ncz_flush(ctx)) return false;
    }
    return true;
}

// Sets up nonce_counter for resuming `sec`'s keystream at absolute NCA
// offset `abs`.
//
// Keyed from `abs` rather than from sec->offset: the two are normally the
// same (runs are always cut at section boundaries, so a section is entered
// at its first byte), but not when the very first decompressed byte -
// always 0x4000, where the zstd stream starts - lands inside a section that
// began earlier. Keying from sec->offset there would offset the entire
// keystream and silently corrupt the NCA. `abs` is always 16-byte aligned
// (0x4000, and NCA section offsets are multiples of the 0x200 media unit),
// so the counter never needs a fractional in-block start.
static void ncz_key_section(NczStreamCtx *ctx, const NczSection *sec, uint64_t abs) {
    mbedtls_aes_free(&ctx->aes);
    mbedtls_aes_init(&ctx->aes);
    mbedtls_aes_setkey_enc(&ctx->aes, sec->key, 128);

    memcpy(ctx->nonce_counter, sec->counter, 8);
    uint64_t block = abs >> 4;
    for (int i = 0; i < 8; i++) {
        ctx->nonce_counter[15 - i] = (uint8_t)(block & 0xFF);
        block >>= 8;
    }
    ctx->nc_off = 0;
    memset(ctx->stream_block, 0, sizeof(ctx->stream_block));
    ctx->keyed_section = ctx->cur_section;
}

// `data`/`len` is freshly zstd-decompressed NCA-plaintext content starting
// at ctx->abs_offset - splits it at section boundaries, AES-CTR re-encrypts
// (or passes through, for gaps/cryptoType 1) each run, and flushes the
// result into the NCM placeholder.
static bool ncz_process_plaintext(NczStreamCtx *ctx, uint8_t *data, size_t len) {
    size_t pos_in_chunk = 0;
    while (pos_in_chunk < len) {
        uint64_t abs = ctx->abs_offset;

        if (abs >= ctx->final_size) {
            if (ctx->err_buf) snprintf(ctx->err_buf, ctx->err_buf_size,
                                        "el NCZ descomprimió más bytes de los esperados (%llu)",
                                        (unsigned long long)ctx->final_size);
            ctx->failed = true;
            return false;
        }

        while (ctx->cur_section < ctx->section_count &&
               abs >= ctx->sections[ctx->cur_section].offset + ctx->sections[ctx->cur_section].size) {
            ctx->cur_section++;
        }

        bool in_section = ctx->cur_section < ctx->section_count &&
                           abs >= ctx->sections[ctx->cur_section].offset &&
                           abs < ctx->sections[ctx->cur_section].offset + ctx->sections[ctx->cur_section].size;

        uint64_t run_end_abs;
        if (in_section) {
            run_end_abs = ctx->sections[ctx->cur_section].offset + ctx->sections[ctx->cur_section].size;
        } else if (ctx->cur_section < ctx->section_count) {
            run_end_abs = ctx->sections[ctx->cur_section].offset;
        } else {
            run_end_abs = ctx->final_size;
        }

        size_t chunk_remaining = len - pos_in_chunk;
        uint64_t run_len64 = run_end_abs - abs;
        size_t run_len = (run_len64 < (uint64_t)chunk_remaining) ? (size_t)run_len64 : chunk_remaining;
        if (run_len == 0) break;

        // Not const: the AES branch below re-encrypts in place. `data` is
        // always ctx->zstd_out (scratch this function owns for the duration
        // of the call), never the caller's own memory.
        uint8_t *run_data = (uint8_t *)data + pos_in_chunk;

        if (in_section && ctx->sections[ctx->cur_section].crypto_type != 1) {
            const NczSection *sec = &ctx->sections[ctx->cur_section];
            if (sec->crypto_type != 3 && sec->crypto_type != 4) {
                if (ctx->err_buf) snprintf(ctx->err_buf, ctx->err_buf_size,
                                            "tipo de cifrado NCZ no soportado (%llu)",
                                            (unsigned long long)sec->crypto_type);
                ctx->failed = true;
                return false;
            }
            // Re-key whenever the active section changes; while staying
            // inside one, the context carries the keystream forward across
            // calls (see nc_off/stream_block).
            if (ctx->keyed_section != ctx->cur_section) {
                ncz_key_section(ctx, sec, abs);
            }

            // CTR mode: encrypt and decrypt are the same XOR-keystream
            // operation. The input here is the plaintext zstd just produced;
            // "encrypting" it is exactly what reconstructs the ciphertext
            // NCM/ES expect on disk. Done in place (input == output) - CTR
            // is a pure XOR against a keystream computed independently of
            // the data, so overwriting as it goes is safe and saves a
            // second full-size scratch buffer.
            int rc = mbedtls_aes_crypt_ctr(&ctx->aes, run_len, &ctx->nc_off, ctx->nonce_counter,
                                            ctx->stream_block, run_data, run_data);
            if (rc != 0) {
                if (ctx->err_buf) snprintf(ctx->err_buf, ctx->err_buf_size, "mbedtls_aes_crypt_ctr falló (%d)", rc);
                ctx->failed = true;
                return false;
            }
            if (!ncz_output_bytes(ctx, run_data, run_len)) return false;
        } else {
            if (!ncz_output_bytes(ctx, run_data, run_len)) return false;
        }

        pos_in_chunk += run_len;
        ctx->abs_offset += run_len;
        if (in_section && ctx->abs_offset == run_end_abs) ctx->cur_section++;
    }

    if (ctx->cb && !ctx->cb((long)ctx->final_size, (long)(ctx->flushed + ctx->flush_len), ctx->userdata)) {
        ctx->canceled = true;
        return false;
    }
    return true;
}

static bool ncz_feed_compressed(NczStreamCtx *ctx, const uint8_t *data, size_t len) {
    ZSTD_inBuffer in = { data, len, 0 };
    while (in.pos < in.size) {
        ZSTD_outBuffer out = { ctx->zstd_out, sizeof(ctx->zstd_out), 0 };
        size_t ret = ZSTD_decompressStream(ctx->zds, &out, &in);
        if (ZSTD_isError(ret)) {
            if (ctx->err_buf) {
                ZSTD_ErrorCode code = ZSTD_getErrorCode(ret);
                if (code == ZSTD_error_frameParameter_windowTooLarge ||
                    code == ZSTD_error_memory_allocation) {
                    // The up-front probe in ncm_install_ncz_content_from_url
                    // said this would fit, so name the window size that
                    // nonetheless didn't rather than repeating a generic
                    // "not enough memory".
                    snprintf(ctx->err_buf, ctx->err_buf_size,
                             "sin memoria al descomprimir (ventana de %llu MB).\n\n"
                             "Abre FreeShop en modo applet (desde el álbum) e inténtalo de nuevo, "
                             "o instálalo con \"Instalar vía DBI\" (botón X).",
                             (unsigned long long)(ctx->window_size / (1024 * 1024)));
                } else {
                    snprintf(ctx->err_buf, ctx->err_buf_size, "zstd: %s", ZSTD_getErrorName(ret));
                }
            }
            ctx->failed = true;
            return false;
        }
        if (out.pos > 0 && !ncz_process_plaintext(ctx, ctx->zstd_out, out.pos)) {
            return false;
        }
    }
    return true;
}

static size_t ncz_network_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    NczStreamCtx *ctx = (NczStreamCtx *)userdata;
    size_t add = size * nmemb;
    if (!ncz_feed_compressed(ctx, (const uint8_t *)ptr, add)) {
        return 0;
    }
    return add;
}

bool ncm_install_ncz_content_from_url(NcmContentStorage *cs, const NcmContentId *content_id,
                                       ResolvedUrl *ru, uint64_t entry_offset,
                                       uint64_t compressed_size, uint64_t final_size,
                                       InstallProgressCallback cb, void *userdata,
                                       bool *out_registered,
                                       char *err_buf, size_t err_buf_size) {
    if (out_registered) *out_registered = false;

    // Content already present is never taken on trust - see
    // drop_existing_content() in ncm_install.c, which does the same for the
    // uncompressed paths and carries the full reasoning. (This path reached
    // that conclusion first, over byte-wrong reconstructions from earlier
    // NCZ builds; the plain-NCA paths since hit the same class of problem
    // from a different direction, so it's now uniform across all of them.)
    bool already_has = false;
    Result rc = ncmContentStorageHas(cs, &already_has, content_id);
    if (R_SUCCEEDED(rc) && already_has) {
        ncmContentStorageDelete(cs, content_id);
    }

    if (compressed_size < NCA_HEADER_SIZE + 16 || final_size < NCA_HEADER_SIZE) {
        if (err_buf) snprintf(err_buf, err_buf_size, "archivo NCZ demasiado pequeño o tamaño final inválido");
        return false;
    }

    size_t prefetch_len = NCZ_HEADER_PREFETCH;
    if ((uint64_t)prefetch_len > compressed_size) prefetch_len = (size_t)compressed_size;

    char *hdr_buf = NULL;
    size_t hdr_len = 0;
    HttpResult hres = resolved_url_get_range(ru, entry_offset, prefetch_len, &hdr_buf, &hdr_len, err_buf, err_buf_size);
    if (hres != HTTP_OK) return false;

    // static, like `ctx` below: at NCZ_MAX_SECTIONS this is ~14KB, and this
    // function is already non-reentrant by design (installs are serial).
    static NczSection sections[NCZ_MAX_SECTIONS];
    int section_count = 0;
    size_t stream_start = 0;
    if (!parse_ncz_sections((const uint8_t *)hdr_buf, hdr_len, sections, &section_count,
                             &stream_start, err_buf, err_buf_size)) {
        free(hdr_buf);
        return false;
    }

    NcmPlaceHolderId placeholder_id;
    rc = ncmContentStorageGeneratePlaceHolderId(cs, &placeholder_id);
    if (R_FAILED(rc)) {
        free(hdr_buf);
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageGeneratePlaceHolderId falló (0x%x)", rc);
        return false;
    }
    ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);

    rc = ncmContentStorageCreatePlaceHolder(cs, content_id, &placeholder_id, (s64)final_size);
    if (R_FAILED(rc)) {
        free(hdr_buf);
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageCreatePlaceHolder falló (0x%x)", rc);
        return false;
    }
    rc = ncmContentStorageSetPlaceHolderSize(cs, &placeholder_id, (s64)final_size);
    if (R_FAILED(rc)) {
        free(hdr_buf);
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageSetPlaceHolderSize falló (0x%x)", rc);
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        return false;
    }

    // The NCA header (bytes [0, 0x4000)) is never touched by nsz - write it
    // verbatim before starting the decompression pass.
    rc = ncmContentStorageWritePlaceHolder(cs, &placeholder_id, 0, hdr_buf, NCA_HEADER_SIZE);
    if (R_FAILED(rc)) {
        free(hdr_buf);
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageWritePlaceHolder falló (0x%x)", rc);
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        return false;
    }

    // How much window buffer this particular file will make zstd allocate
    // is stated in its frame header - read it now, while there's still a
    // chance to refuse with a message that names real numbers instead of
    // letting the allocation blow up opaquely somewhere inside zstd.
    uint64_t window_size = 0;
    if (hdr_len > stream_start) {
        ZSTD_FrameHeader zfh;
        if (ZSTD_getFrameHeader(&zfh, (const uint8_t *)hdr_buf + stream_start,
                                 hdr_len - stream_start) == 0) {
            window_size = zfh.windowSize;
        }
    }

    if (window_size > 0 && !ncz_can_allocate((size_t)window_size + NCZ_MEMORY_HEADROOM)) {
        free(hdr_buf);
        if (err_buf) {
            // Confirmed on real hardware: the same file that fails here
            // installs fine once FreeShop is launched in applet mode, which
            // is where this decompression gets the memory it needs.
            snprintf(err_buf, err_buf_size,
                     "este NSZ necesita %llu MB de memoria para descomprimirse y no hay suficiente.\n\n"
                     "Abre FreeShop en modo applet (desde el álbum) e inténtalo de nuevo, "
                     "o instálalo con \"Instalar vía DBI\" (botón X).",
                     (unsigned long long)(window_size / (1024 * 1024)));
        }
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        return false;
    }

    ZSTD_DCtx *zds = ZSTD_createDCtx();
    if (!zds) {
        free(hdr_buf);
        if (err_buf) snprintf(err_buf, err_buf_size, "ZSTD_createDCtx falló (memoria insuficiente)");
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        return false;
    }
    // No artificial window cap - the check above already decided this fits,
    // using what's actually free right now rather than a guessed constant.
    // zstd's own default windowLogMax would otherwise reject large-window
    // frames that this console can, in fact, handle.
    ZSTD_DCtx_setParameter(zds, ZSTD_d_windowLogMax, ZSTD_WINDOWLOG_MAX);

    // Static, not stack-allocated: this struct embeds a 128KB scratch
    // buffer - too big for a thread stack. Installs run one content piece
    // at a time, serially, so reusing one static instance is safe (mirrors
    // ncm_install.c's own NcaNetworkWriteCtx pattern).
    static NczStreamCtx ctx;
    uint8_t *s_flush_buf = install_common_scratch();
    memset(&ctx, 0, sizeof(ctx));
    ctx.cs = cs;
    ctx.placeholder_id = &placeholder_id;
    ctx.zds = zds;
    memcpy(ctx.sections, sections, sizeof(sections));
    ctx.section_count = section_count;
    ctx.cur_section = 0;
    ctx.keyed_section = -1; // nothing keyed yet (memset above would read as section 0)
    ctx.abs_offset = NCA_HEADER_SIZE;
    ctx.final_size = final_size;
    ctx.window_size = window_size;
    ctx.flush_buf = s_flush_buf;
    ctx.flush_cap = INSTALL_SCRATCH_SIZE;
    ctx.flushed = NCA_HEADER_SIZE; // header already written directly, above
    ctx.cb = cb;
    ctx.userdata = userdata;
    ctx.err_buf = err_buf;
    ctx.err_buf_size = err_buf_size;

    // Hash covers the whole reconstructed NCA, so it starts with the verbatim
    // header that was written above (ncz_output_bytes feeds it everything
    // after that).
    mbedtls_sha256_init(&ctx.sha);
    mbedtls_sha256_starts(&ctx.sha, 0); // 0 = SHA-256, not SHA-224
    mbedtls_sha256_update(&ctx.sha, (const unsigned char *)hdr_buf, NCA_HEADER_SIZE);

    // Whatever's left in the prefetch buffer past the section table is
    // already the start of the zstd stream - feed it in directly instead of
    // re-requesting those same bytes from the network.
    bool ok = true;
    if (hdr_len > stream_start) {
        ok = ncz_feed_compressed(&ctx, (const uint8_t *)hdr_buf + stream_start, hdr_len - stream_start);
    }
    free(hdr_buf);

    uint64_t remaining_compressed = compressed_size - prefetch_len;
    if (ok && remaining_compressed > 0) {
        // Cached link first, then a freshly resolved one on failure - same
        // as ncm_install_content_from_url, and for the same reason: a
        // MediaFire link can expire partway through a long transfer, and
        // re-resolving has to happen on-console (resolved_url_refresh)
        // rather than by requesting the proxy URL, which yields a link
        // bound to the server's address that MediaFire refuses here.
        resolved_url_ensure_direct(ru);
        bool had_cached = ru->direct_url[0] != '\0';

        char net_err[160] = {0};
        HttpResult r2 = HTTP_ERR_REQUEST;
        if (had_cached) {
            r2 = http_get_range_streamed(ru->direct_url, entry_offset + prefetch_len, remaining_compressed,
                                          ncz_network_write_cb, &ctx, NULL, 0, net_err, sizeof(net_err));
        }
        if (r2 != HTTP_OK && !ctx.canceled && !ctx.failed) {
            const char *url = resolved_url_refresh(ru);
            net_err[0] = '\0';
            r2 = http_get_range_streamed(url, entry_offset + prefetch_len, remaining_compressed,
                                          ncz_network_write_cb, &ctx, NULL, 0, net_err, sizeof(net_err));
        }
        if (r2 != HTTP_OK && !ctx.canceled && !ctx.failed) {
            ok = false;
            if (err_buf) {
                if (net_err[0]) snprintf(err_buf, err_buf_size, "descarga falló: %s", net_err);
                else snprintf(err_buf, err_buf_size, "descarga de red incompleta");
            }
        }
    }

    ZSTD_freeDCtx(zds);
    mbedtls_aes_free(&ctx.aes);

    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx.sha, digest);
    mbedtls_sha256_free(&ctx.sha);

    if (ctx.canceled) {
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        if (err_buf) snprintf(err_buf, err_buf_size, "instalación cancelada");
        return false;
    }
    if (ctx.failed || !ok) {
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        return false; // err_buf already filled
    }

    if (!ncz_flush(&ctx)) {
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        return false;
    }

    // Same "unconditional, not just on failure" reasoning as
    // ncm_install_content_from_url's own checkpoint in ncm_install.c.
    download_debug_log("ncm_install_ncz_content_from_url: compressed_size=%llu final_size=%llu "
                        "decompressed=%llu -> %s",
                        (unsigned long long)compressed_size, (unsigned long long)final_size,
                        (unsigned long long)ctx.abs_offset,
                        (ctx.abs_offset == final_size) ? "OK" : "INCOMPLETE");

    if (ctx.abs_offset != final_size) {
        if (err_buf) snprintf(err_buf, err_buf_size,
                               "el contenido NCZ descomprimido no coincide con el tamaño esperado (%llu vs %llu bytes)",
                               (unsigned long long)ctx.abs_offset, (unsigned long long)final_size);
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
        return false;
    }

    // An NCA's content id is the first 16 bytes of its own SHA-256, so this
    // is a complete integrity check on the reconstruction - not just its
    // length. Registering content that fails it would put a title on the
    // home menu that the console then refuses to launch, which is far worse
    // than a clean failure here.
    if (memcmp(digest, content_id->c, 16) != 0) {
        // Distinct from the size checkpoint above on purpose: same length
        // but wrong bytes points at a decompression/re-encryption bug
        // rather than a truncated transfer.
        download_debug_log("ncm_install_ncz_content_from_url: SHA-256 mismatch (length matched, "
                            "content did not) - not registered");
        if (err_buf) snprintf(err_buf, err_buf_size,
                               "el contenido reconstruido del NCZ no coincide con su hash esperado - "
                               "no se instaló nada. Usa \"Instalar vía DBI\" (botón X) para este archivo.");
        ncmContentStorageDeletePlaceHolder(cs, &placeholder_id);
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
