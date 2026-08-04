#include "install_stream.h"
#include "pfs0.h"
#include "hfs0.h"
#include "xci_container.h"
#include "ncm_install.h"
#include "ncz.h"
#include "es_ticket.h"
#include "ns_record.h"

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Up to 4 ticket/cert pairs - matches install_local.c/install_nsp_native.c.
#define MAX_TICKET_PAIRS 4

// PFS0_MAX_ENTRIES and HFS0_MAX_ENTRIES (pfs0.h/hfs0.h) are both 128 - one
// cap covers whichever container type is actually in play.
#define STREAM_MAX_ENTRIES 128
#define STREAM_NAME_MAX 64

// A PFS0/HFS0 header is 16 bytes + entries + a string table capped at 64KB
// by both parsers - comfortably under 72KB even at STREAM_MAX_ENTRIES
// entries. XCI's root-partition search needs a bigger window (see
// xci_container.h) - one buffer sized for the larger of the two covers
// both, since only one is ever in use per stream.
#define STREAM_HEADER_MAX (72 * 1024)
#define STREAM_HEADER_BUF_SIZE (XCI_SEARCH_WINDOW > STREAM_HEADER_MAX ? XCI_SEARCH_WINDOW : STREAM_HEADER_MAX)

// Tickets are ~1 KB and certs ~2 KB; this is generous enough that hitting
// it means the entry isn't really a ticket.
#define SMALL_FILE_MAX (256 * 1024)
#define MAX_SMALL_FILES (MAX_TICKET_PAIRS * 2)

// How many (content_id -> real decompressed size) pairs to remember from
// already-processed .cnmt.nca entries - see the InstallStream.known_sizes
// doc comment. Headroom for a handful of bundled titles (base+patch+DLCs),
// each contributing up to NCM_MAX_CONTENT_INFOS entries.
#define STREAM_MAX_KNOWN_SIZES (NCM_MAX_CONTENT_INFOS * 4)

// One container entry FreeShop might care about (an .nca, a .tik/.cert, or
// something ignored) - normalized to a flat {offset, size, name} regardless
// of whether it came from an NSP's single PFS0 or an XCI's nested "secure"
// HFS0 partition, so everything downstream of header-parsing (routing,
// finish) doesn't need to know which.
typedef struct {
    uint64_t offset; // absolute within the byte stream this InstallStream is fed
    uint64_t size;
    char name[STREAM_NAME_MAX];
} StreamEntry;

// A .tik/.cert entry held in memory as it streams past, because es_ticket
// wants the whole thing at once and both are tiny. Everything else either
// streams straight into NCM (.nca) or is ignored.
typedef struct {
    int entry_index; // into InstallStream.entries
    uint8_t *data;
    uint64_t size;
    uint64_t written;
} SmallFile;

// Stages an XCI passes through before its "secure" partition's entries are
// known and the common per-entry routing (shared with NSP) can start. An
// NSP skips straight to that shared routing once its one PFS0 header is
// parsed - it has no equivalent of these.
typedef enum {
    XCI_STAGE_ROOT,   // accumulating up to XCI_SEARCH_WINDOW bytes to locate + parse the root HFS0
    XCI_STAGE_SEEK,   // discarding bytes until reaching the "secure" partition's start
    XCI_STAGE_SECURE, // accumulating "secure"'s own HFS0 header
} XciStage;

struct InstallStream {
    bool is_xci;
    uint64_t total_size;
    uint64_t consumed; // absolute offset of the next byte expected

    uint8_t *header_buf;
    size_t header_len;
    bool header_done; // entries[] is populated and routing can begin
    XciStage xci_stage;
    uint64_t secure_abs_offset; // XCI only, valid once xci_stage is past XCI_STAGE_ROOT

    StreamEntry entries[STREAM_MAX_ENTRIES];
    int entry_count;

    bool services_up;
    NcmContentStorage cs;
    NcmContentMetaDatabase db;

    int cur_entry; // entries[] index the incoming bytes currently fall inside, -1 if none

    // The .nca entry currently being written, if cur_entry is one.
    bool nca_active;
    NcmContentId nca_id;
    NcmPlaceHolderId nca_ph;
    uint64_t nca_written;

    // The .ncz entry currently being decompressed, if cur_entry is one -
    // an NSZ's (or XCZ's) compressed counterpart to a plain .nca. Mutually
    // exclusive with nca_active.
    bool ncz_active;
    NcmContentId ncz_content_id;
    NczPushCtx *ncz_ctx;

    // A .ncz entry's own size (in entries[]) is its on-wire *compressed*
    // size - routing bytes to the right entry only ever needs that. But
    // decompressing one needs the real, final NCA size upfront (to size its
    // NCM placeholder), which only exists in the CNMT that describes it -
    // and unlike a plain .nca, that size can't be recovered from the .ncz
    // entry's own bytes. So every .cnmt.nca gets read back the moment its
    // own bytes finish arriving (see end_entry) and its content sizes
    // recorded here, in case a .ncz entry needing one of them is still to
    // come. Relies on real NSZ/XCZ packers placing each title's .cnmt.nca
    // before the .ncz entries it describes - true of every NSZ/XCZ this was
    // tested against; a title packed the other way around fails cleanly
    // (see begin_entry) rather than silently mis-sizing anything.
    struct {
        NcmContentId id;
        uint64_t size;
    } known_sizes[STREAM_MAX_KNOWN_SIZES];
    int known_size_count;

    SmallFile smalls[MAX_SMALL_FILES];
    int small_count;
    int cur_small; // index into smalls of the entry being filled, -1 if none

    NcmContentId registered_ids[STREAM_MAX_ENTRIES];
    int registered_count;
};

static bool ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (xl > sl) return false;
    return strcmp(s + (sl - xl), suffix) == 0;
}

static int stream_find_by_suffix(const InstallStream *s, const char *suffix, int start_from) {
    for (int i = start_from; i < s->entry_count; i++) {
        if (ends_with(s->entries[i].name, suffix)) return i;
    }
    return -1;
}

// Same convention as install_local.c's - see its doc comment.
static uint64_t application_id_for_meta(const NcmContentMetaKey *key) {
    if (key->type == NcmContentMetaType_Patch) {
        return key->id ^ 0x800ULL;
    }
    if (key->type == NcmContentMetaType_AddOnContent) {
        return (key->id ^ 0x1000ULL) & ~0xFFFULL;
    }
    return key->id;
}

static void rollback_registered(InstallStream *s) {
    for (int i = 0; i < s->registered_count; i++) {
        ncmContentStorageDelete(&s->cs, &s->registered_ids[i]);
    }
    s->registered_count = 0;
}

static void stream_free(InstallStream *s) {
    if (!s) return;

    if (s->nca_active) {
        ncmContentStorageDeletePlaceHolder(&s->cs, &s->nca_ph);
        s->nca_active = false;
    }
    if (s->ncz_active) {
        ncz_push_abort(s->ncz_ctx);
        s->ncz_ctx = NULL;
        s->ncz_active = false;
    }
    for (int i = 0; i < s->small_count; i++) free(s->smalls[i].data);
    free(s->header_buf);

    if (s->services_up) {
        ncmContentMetaDatabaseClose(&s->db);
        ncmContentStorageClose(&s->cs);
        ncmExit();
        ns_record_exit();
        es_exit();
    }
    free(s);
}

static InstallStream *stream_begin_common(uint64_t total_size, char *err_buf, size_t err_buf_size) {
    InstallStream *s = (InstallStream *)calloc(1, sizeof(InstallStream));
    if (!s) {
        if (err_buf) snprintf(err_buf, err_buf_size, "memoria insuficiente");
        return NULL;
    }
    s->total_size = total_size;
    s->cur_entry = -1;
    s->cur_small = -1;

    s->header_buf = (uint8_t *)malloc(STREAM_HEADER_BUF_SIZE);
    if (!s->header_buf) {
        if (err_buf) snprintf(err_buf, err_buf_size, "memoria insuficiente para la cabecera del contenedor");
        free(s);
        return NULL;
    }

    // Same service bring-up order (and teardown on partial failure) as
    // install_local.c's - es first, then ns, then ncm.
    Result rc = es_initialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio es (0x%x)", rc);
        free(s->header_buf); free(s);
        return NULL;
    }
    rc = ns_record_initialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio ns (0x%x)", rc);
        es_exit(); free(s->header_buf); free(s);
        return NULL;
    }
    rc = ncmInitialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio ncm (0x%x)", rc);
        ns_record_exit(); es_exit(); free(s->header_buf); free(s);
        return NULL;
    }
    rc = ncmOpenContentStorage(&s->cs, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmOpenContentStorage falló (0x%x)", rc);
        ncmExit(); ns_record_exit(); es_exit(); free(s->header_buf); free(s);
        return NULL;
    }
    rc = ncmOpenContentMetaDatabase(&s->db, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmOpenContentMetaDatabase falló (0x%x)", rc);
        ncmContentStorageClose(&s->cs);
        ncmExit(); ns_record_exit(); es_exit(); free(s->header_buf); free(s);
        return NULL;
    }

    s->services_up = true;
    return s;
}

InstallStream *install_stream_begin(uint64_t total_size, char *err_buf, size_t err_buf_size) {
    return stream_begin_common(total_size, err_buf, err_buf_size);
}

InstallStream *install_stream_begin_xci(uint64_t total_size, char *err_buf, size_t err_buf_size) {
    InstallStream *s = stream_begin_common(total_size, err_buf, err_buf_size);
    if (s) s->is_xci = true; // xci_stage defaults to XCI_STAGE_ROOT (0) via calloc
    return s;
}

// ---- Per-entry routing - shared between NSP and XCI once entries[] is known ----

static int entry_at(const InstallStream *s, uint64_t offset) {
    for (int i = 0; i < s->entry_count; i++) {
        if (offset >= s->entries[i].offset && offset < s->entries[i].offset + s->entries[i].size) return i;
    }
    return -1;
}

// Start of the earliest entry beginning after `offset` - how far ahead the
// next real data is when `offset` lands in padding between entries.
static uint64_t next_entry_start(const InstallStream *s, uint64_t offset) {
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < s->entry_count; i++) {
        if (s->entries[i].offset > offset && s->entries[i].offset < best) best = s->entries[i].offset;
    }
    return best;
}

static bool begin_entry(InstallStream *s, int idx, char *err_buf, size_t err_buf_size) {
    const char *name = s->entries[idx].name;
    uint64_t size = s->entries[idx].size;

    if (ends_with(name, ".nca")) { // also covers ".cnmt.nca"
        if (!ncm_parse_content_id(name, &s->nca_id)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "nombre de NCA inválido: %s", name);
            return false;
        }

        Result rc = ncmContentStorageGeneratePlaceHolderId(&s->cs, &s->nca_ph);
        if (R_FAILED(rc)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageGeneratePlaceHolderId falló (0x%x)", rc);
            return false;
        }
        // Clear out any stale placeholder left behind by a previous failed attempt.
        ncmContentStorageDeletePlaceHolder(&s->cs, &s->nca_ph);

        rc = ncmContentStorageCreatePlaceHolder(&s->cs, &s->nca_id, &s->nca_ph, (s64)size);
        if (R_FAILED(rc)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageCreatePlaceHolder falló (0x%x)", rc);
            return false;
        }
        rc = ncmContentStorageSetPlaceHolderSize(&s->cs, &s->nca_ph, (s64)size);
        if (R_FAILED(rc)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageSetPlaceHolderSize falló (0x%x)", rc);
            ncmContentStorageDeletePlaceHolder(&s->cs, &s->nca_ph);
            return false;
        }

        s->nca_active = true;
        s->nca_written = 0;
        return true;
    }

    if (ends_with(name, ".ncz")) {
        NcmContentId content_id;
        if (!ncm_parse_content_id(name, &content_id)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "nombre de NCZ inválido: %s", name);
            return false;
        }

        uint64_t final_size = 0;
        bool found = false;
        for (int i = 0; i < s->known_size_count; i++) {
            if (memcmp(&s->known_sizes[i].id, &content_id, sizeof(content_id)) == 0) {
                final_size = s->known_sizes[i].size;
                found = true;
                break;
            }
        }
        if (!found) {
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "este NSZ/XCZ no está ordenado de forma compatible con la instalación por "
                                   "streaming (%s aparece antes que el .cnmt.nca que lo describe)", name);
            return false;
        }

        s->ncz_content_id = content_id;
        s->ncz_ctx = ncz_push_begin(&s->cs, &content_id, size, final_size, err_buf, err_buf_size);
        if (!s->ncz_ctx) return false;
        s->ncz_active = true;
        return true;
    }

    if (ends_with(name, ".tik") || ends_with(name, ".cert")) {
        if (s->small_count >= MAX_SMALL_FILES || size > SMALL_FILE_MAX) {
            // Not fatal - a container with more ticket pairs than this
            // responder handles still installs its content fine; the extra
            // tickets just don't get imported, same as the staged
            // installer's own MAX_TICKET_PAIRS cap.
            return true;
        }
        SmallFile *sf = &s->smalls[s->small_count];
        sf->data = (uint8_t *)malloc((size_t)size);
        if (!sf->data) {
            if (err_buf) snprintf(err_buf, err_buf_size, "memoria insuficiente para el ticket");
            return false;
        }
        sf->entry_index = idx;
        sf->size = size;
        sf->written = 0;
        s->cur_small = s->small_count;
        s->small_count++;
        return true;
    }

    return true; // anything else in the container is not ours to install
}

static bool write_entry(InstallStream *s, const uint8_t *p, size_t len, char *err_buf, size_t err_buf_size) {
    if (s->nca_active) {
        Result rc = ncmContentStorageWritePlaceHolder(&s->cs, &s->nca_ph, s->nca_written, p, len);
        if (R_FAILED(rc)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageWritePlaceHolder falló (0x%x)", rc);
            return false;
        }
        s->nca_written += len;
        return true;
    }
    if (s->ncz_active) {
        // ncz_push_feed's own err_buf was already bound at ncz_push_begin -
        // its failure leaves a reason there directly, same convention
        // end_entry uses when it reads that back on failure.
        return ncz_push_feed(s->ncz_ctx, p, len);
    }
    if (s->cur_small >= 0) {
        SmallFile *sf = &s->smalls[s->cur_small];
        if (sf->written + len <= sf->size) {
            memcpy(sf->data + sf->written, p, len);
            sf->written += len;
        }
        return true;
    }
    return true; // ignored entry - bytes just pass through
}

static bool end_entry(InstallStream *s, char *err_buf, size_t err_buf_size) {
    if (s->nca_active) {
        Result rc = ncmContentStorageRegister(&s->cs, &s->nca_id, &s->nca_ph);
        if (R_FAILED(rc)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageRegister falló (0x%x)", rc);
            ncmContentStorageDeletePlaceHolder(&s->cs, &s->nca_ph);
            s->nca_active = false;
            return false;
        }
        // Register consumes the placeholder in the common case - this is
        // best-effort cleanup in case anything was left behind.
        ncmContentStorageDeletePlaceHolder(&s->cs, &s->nca_ph);

        if (s->registered_count < STREAM_MAX_ENTRIES) {
            s->registered_ids[s->registered_count++] = s->nca_id;
        }

        // If this was a .cnmt.nca, read its content list back immediately -
        // a .ncz entry describing part of the same title may need its real
        // (decompressed) size before its own bytes arrive, and there's no
        // later opportunity to learn it once streaming has moved past this
        // entry. Best-effort: a failure here isn't reported (err_buf=NULL)
        // since install_stream_finish re-reads and properly validates every
        // CNMT at the end anyway - if this genuinely can't be read, any
        // .ncz entry that needed it will fail its own clear "not found"
        // check in begin_entry instead.
        if (ends_with(s->entries[s->cur_entry].name, ".cnmt.nca")) {
            ContentMetaInfo meta;
            if (ncm_read_content_meta(&s->cs, &s->nca_id, &meta, NULL, 0)) {
                for (int i = 0; i < meta.content_info_count && s->known_size_count < STREAM_MAX_KNOWN_SIZES; i++) {
                    u64 piece = 0;
                    ncmContentInfoSizeToU64(&meta.content_infos[i], &piece);
                    s->known_sizes[s->known_size_count].id = meta.content_infos[i].content_id;
                    s->known_sizes[s->known_size_count].size = piece;
                    s->known_size_count++;
                }
            }
        }

        s->nca_active = false;
    }
    if (s->ncz_active) {
        bool ok = ncz_push_finish(s->ncz_ctx, err_buf, err_buf_size);
        s->ncz_ctx = NULL;
        s->ncz_active = false;
        if (!ok) return false;
        if (s->registered_count < STREAM_MAX_ENTRIES) {
            s->registered_ids[s->registered_count++] = s->ncz_content_id;
        }
    }
    s->cur_small = -1;
    return true;
}

static bool route(InstallStream *s, const uint8_t *p, size_t len, char *err_buf, size_t err_buf_size) {
    while (len > 0) {
        int idx = entry_at(s, s->consumed);

        if (idx < 0) {
            // Padding between entries (PFS0/HFS0 both align their data
            // regions) - skip ahead without touching anything.
            uint64_t next = next_entry_start(s, s->consumed);
            uint64_t gap = (next == UINT64_MAX) ? len : next - s->consumed;
            if (gap > len) gap = len;
            s->consumed += gap;
            p += gap;
            len -= (size_t)gap;
            continue;
        }

        uint64_t entry_end = s->entries[idx].offset + s->entries[idx].size;
        uint64_t remaining_in_entry = entry_end - s->consumed;
        size_t take = remaining_in_entry < len ? (size_t)remaining_in_entry : len;

        if (idx != s->cur_entry) {
            if (!begin_entry(s, idx, err_buf, err_buf_size)) return false;
            s->cur_entry = idx;
        }
        if (!write_entry(s, p, take, err_buf, err_buf_size)) return false;

        s->consumed += take;
        p += take;
        len -= take;

        if (s->consumed >= entry_end) {
            if (!end_entry(s, err_buf, err_buf_size)) return false;
            s->cur_entry = -1;
        }
    }
    return true;
}

// ---- Header parsing - NSP is a single PFS0; XCI has to find its root HFS0
// and then the "secure" partition nested inside it before entries[] is
// known. Both converge on the same route() once header_done is set. ----

static bool build_entries_from_pfs0(InstallStream *s, const Pfs0 *pfs0, char *err_buf, size_t err_buf_size) {
    if (pfs0->count > STREAM_MAX_ENTRIES) {
        if (err_buf) snprintf(err_buf, err_buf_size, "el NSP tiene demasiadas entradas");
        return false;
    }
    s->entry_count = pfs0->count;
    for (int i = 0; i < pfs0->count; i++) {
        s->entries[i].offset = pfs0_entry_file_offset(pfs0, i);
        s->entries[i].size = pfs0->entries[i].file_size;
        snprintf(s->entries[i].name, sizeof(s->entries[i].name), "%s", pfs0->names[i]);
    }
    return true;
}

static bool nsp_feed_header(InstallStream *s, const uint8_t **pp, size_t *len, char *err_buf, size_t err_buf_size) {
    size_t space = STREAM_HEADER_MAX - s->header_len;
    if (space == 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "cabecera PFS0 inválida o demasiado grande");
        return false;
    }
    size_t take = *len < space ? *len : space;
    memcpy(s->header_buf + s->header_len, *pp, take);
    s->header_len += take;
    *pp += take;
    *len -= take;

    Pfs0 pfs0;
    int rc = pfs0_parse_buffer(s->header_buf, s->header_len, &pfs0);
    if (rc == -2) {
        if (err_buf) snprintf(err_buf, err_buf_size, "el archivo no es un NSP (PFS0) válido");
        return false;
    }
    if (rc == -3) return true; // need more bytes

    if (!build_entries_from_pfs0(s, &pfs0, err_buf, err_buf_size)) return false;
    s->consumed = pfs0.data_region_offset;
    s->header_done = true;

    // Whatever was accumulated past the end of the header is already file
    // data and has to be routed, not dropped.
    size_t leftover = s->header_len - (size_t)pfs0.data_region_offset;
    bool ok = true;
    if (leftover > 0) {
        ok = route(s, s->header_buf + pfs0.data_region_offset, leftover, err_buf, err_buf_size);
    }
    free(s->header_buf);
    s->header_buf = NULL;
    return ok;
}

// XCI_STAGE_ROOT: accumulate up to XCI_SEARCH_WINDOW bytes (or the whole
// file, if it's smaller) from the very start, then locate + parse the root
// HFS0 partition table within them exactly like the staged installer's
// xci_open_secure_partition_local does from a single fread - streaming just
// means those bytes arrive incrementally instead of in one shot.
static bool xci_feed_root(InstallStream *s, const uint8_t **pp, size_t *len, char *err_buf, size_t err_buf_size) {
    size_t window_cap = XCI_SEARCH_WINDOW;
    if (s->total_size > 0 && s->total_size < window_cap) window_cap = (size_t)s->total_size;

    size_t space = window_cap - s->header_len;
    if (space == 0) {
        if (err_buf) snprintf(err_buf, err_buf_size,
                               "el archivo no es un XCI válido (no se encontró la tabla de particiones)");
        return false;
    }
    size_t take = *len < space ? *len : space;
    memcpy(s->header_buf + s->header_len, *pp, take);
    s->header_len += take;
    *pp += take;
    *len -= take;

    // xci_find_root_hfs0 has no partial/"need more" signal of its own
    // (unlike pfs0_parse_buffer) - it's built around getting the whole
    // window at once, so wait until it's as full as it's going to get.
    if (s->header_len < window_cap) return true;

    uint64_t root_offset = 0;
    if (!xci_find_root_hfs0(s->header_buf, s->header_len, &root_offset, err_buf, err_buf_size)) {
        return false;
    }
    Hfs0 root;
    if (hfs0_parse_buffer(s->header_buf, s->header_len, root_offset, &root) != 0) {
        if (err_buf) snprintf(err_buf, err_buf_size,
                               "el archivo no es un XCI válido (tabla de particiones raíz inválida)");
        return false;
    }
    int secure_index = hfs0_find_by_name(&root, "secure");
    if (secure_index < 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "el XCI no tiene partición 'secure'");
        return false;
    }
    // header_offset passed above was absolute (root_offset, found within a
    // window starting at byte 0 of the XCI), so this is already absolute.
    s->secure_abs_offset = hfs0_entry_file_offset(&root, secure_index);
    s->consumed = s->header_len; // every byte fed so far, consecutively from offset 0

    if (s->consumed >= s->secure_abs_offset) {
        // secure starts inside what's already been buffered (a small XCI,
        // or an unusually early "secure") - keep its bytes, discard the
        // window bytes before it, and go straight to parsing its header.
        size_t already = (size_t)(s->consumed - s->secure_abs_offset);
        memmove(s->header_buf, s->header_buf + (size_t)s->secure_abs_offset, already);
        s->header_len = already;
        s->xci_stage = XCI_STAGE_SECURE;
    } else {
        s->header_len = 0;
        s->xci_stage = XCI_STAGE_SEEK;
    }
    return true;
}

// XCI_STAGE_SEEK: discard bytes until reaching "secure"'s start. This is
// usually most of the transfer - "secure" is ordinarily the last and by far
// the largest partition in a real XCI, holding the actual game content, so
// there's little to install-while-copying before it starts; the win here is
// still real (no staging copy, no second install pass after), just later.
static bool xci_feed_seek(InstallStream *s, const uint8_t **pp, size_t *len) {
    uint64_t remaining_gap = s->secure_abs_offset - s->consumed;
    size_t skip = (*len < remaining_gap) ? *len : (size_t)remaining_gap;
    s->consumed += skip;
    *pp += skip;
    *len -= skip;
    if (s->consumed >= s->secure_abs_offset) s->xci_stage = XCI_STAGE_SECURE;
    return true;
}

// XCI_STAGE_SECURE: accumulate "secure"'s own HFS0 header (structurally the
// same size class as an NSP's PFS0 one), then build entries[] from it with
// offsets translated to be absolute in the whole XCI.
static bool xci_feed_secure(InstallStream *s, const uint8_t **pp, size_t *len, char *err_buf, size_t err_buf_size) {
    size_t space = STREAM_HEADER_MAX - s->header_len;
    if (space == 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "la partición 'secure' tiene una cabecera inválida o demasiado grande");
        return false;
    }
    size_t take = *len < space ? *len : space;
    memcpy(s->header_buf + s->header_len, *pp, take);
    s->header_len += take;
    *pp += take;
    *len -= take;
    s->consumed += take;

    // header_buf's byte 0 is secure_abs_offset here (not XCI byte 0), so
    // header_offset=0 - hfs0_parse_buffer's own -1 ("buffer too small for a
    // header at this offset") doubles as "need more bytes" for this
    // in-memory variant, since there's no I/O involved to fail for real.
    Hfs0 secure;
    int rc = hfs0_parse_buffer(s->header_buf, s->header_len, 0, &secure);
    if (rc == -2) {
        if (err_buf) snprintf(err_buf, err_buf_size, "la partición 'secure' no es válida");
        return false;
    }
    if (rc == -1) return true; // need more bytes

    if (secure.count > STREAM_MAX_ENTRIES) {
        if (err_buf) snprintf(err_buf, err_buf_size, "la partición 'secure' tiene demasiadas entradas");
        return false;
    }
    s->entry_count = secure.count;
    for (int i = 0; i < secure.count; i++) {
        s->entries[i].offset = s->secure_abs_offset + hfs0_entry_file_offset(&secure, i);
        s->entries[i].size = secure.entries[i].file_size;
        snprintf(s->entries[i].name, sizeof(s->entries[i].name), "%s", secure.names[i]);
    }
    s->header_done = true;

    // route() places whatever it's handed at s->consumed, so consumed has to
    // point at where the leftover bytes actually begin - not where the
    // accumulation loop above left it (past everything buffered, header
    // included). Getting this wrong doesn't fail loudly: every byte of
    // content lands shifted by the size of the leftover, the NCAs are
    // written subtly corrupt, and the first sign of trouble is the OS
    // refusing to open the meta NCA at the very end of the install.
    size_t leftover = s->header_len - (size_t)secure.data_region_offset;
    s->consumed = s->secure_abs_offset + secure.data_region_offset;

    bool ok = true;
    if (leftover > 0) {
        ok = route(s, s->header_buf + secure.data_region_offset, leftover, err_buf, err_buf_size);
    }
    free(s->header_buf);
    s->header_buf = NULL;
    return ok;
}

static bool xci_feed_header(InstallStream *s, const uint8_t **pp, size_t *len, char *err_buf, size_t err_buf_size) {
    switch (s->xci_stage) {
        case XCI_STAGE_ROOT:   return xci_feed_root(s, pp, len, err_buf, err_buf_size);
        case XCI_STAGE_SEEK:   return xci_feed_seek(s, pp, len);
        case XCI_STAGE_SECURE: return xci_feed_secure(s, pp, len, err_buf, err_buf_size);
    }
    return true;
}

bool install_stream_feed(InstallStream *s, const void *data, size_t len, char *err_buf, size_t err_buf_size) {
    const uint8_t *p = (const uint8_t *)data;

    while (len > 0) {
        if (s->header_done) return route(s, p, len, err_buf, err_buf_size);

        bool ok = s->is_xci ? xci_feed_header(s, &p, &len, err_buf, err_buf_size)
                             : nsp_feed_header(s, &p, &len, err_buf, err_buf_size);
        if (!ok) return false;
    }
    return true;
}

// ---- Completion ----

static bool import_tickets(InstallStream *s, char *err_buf, size_t err_buf_size) {
    const SmallFile *tiks[MAX_TICKET_PAIRS];
    const SmallFile *certs[MAX_TICKET_PAIRS];
    int tik_count = 0, cert_count = 0;

    for (int i = 0; i < s->small_count; i++) {
        const SmallFile *sf = &s->smalls[i];
        if (sf->written != sf->size) continue; // never fully arrived - skip it
        const char *name = s->entries[sf->entry_index].name;
        if (ends_with(name, ".tik") && tik_count < MAX_TICKET_PAIRS) tiks[tik_count++] = sf;
        else if (ends_with(name, ".cert") && cert_count < MAX_TICKET_PAIRS) certs[cert_count++] = sf;
    }

    if (tik_count != cert_count) {
        if (err_buf) snprintf(err_buf, err_buf_size, "el contenedor tiene un número distinto de archivos .tik y .cert");
        return false;
    }

    for (int i = 0; i < tik_count; i++) {
        Result rc = es_import_ticket(tiks[i]->data, (size_t)tiks[i]->size,
                                      certs[i]->data, (size_t)certs[i]->size);
        if (R_FAILED(rc)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "esImportTicket falló (0x%x)", rc);
            return false;
        }
    }
    return true;
}

InstallLocalResult install_stream_finish(InstallStream *s, char *err_buf, size_t err_buf_size) {
    if (!s) return INSTALL_LOCAL_ERR_PARSE;

    InstallLocalResult result = INSTALL_LOCAL_OK;

    if (!s->header_done) {
        if (err_buf) {
            snprintf(err_buf, err_buf_size, s->is_xci ? "el archivo no es un XCI válido"
                                                        : "el archivo no es un NSP (PFS0) válido");
        }
        result = INSTALL_LOCAL_ERR_PARSE;
    } else if (s->nca_active) {
        // The stream ended partway through an NCA.
        if (err_buf) snprintf(err_buf, err_buf_size, "la transferencia terminó antes de completar el archivo");
        result = INSTALL_LOCAL_ERR_PARSE;
    }

    // Every .cnmt.nca in the container describes one title - commit each.
    int cnmt_index = (result == INSTALL_LOCAL_OK) ? stream_find_by_suffix(s, ".cnmt.nca", 0) : -1;
    if (result == INSTALL_LOCAL_OK && cnmt_index < 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "el archivo no contiene ningún .cnmt.nca");
        result = INSTALL_LOCAL_ERR_PARSE;
    }

    while (cnmt_index >= 0 && result == INSTALL_LOCAL_OK) {
        NcmContentId cnmt_id;
        if (!ncm_parse_content_id(s->entries[cnmt_index].name, &cnmt_id)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "nombre de archivo cnmt.nca inválido: %s",
                                   s->entries[cnmt_index].name);
            result = INSTALL_LOCAL_ERR_PARSE;
            break;
        }

        ContentMetaInfo meta;
        if (!ncm_read_content_meta(&s->cs, &cnmt_id, &meta, err_buf, err_buf_size)) {
            result = INSTALL_LOCAL_ERR_NCM;
            break;
        }

        // Everything the CNMT references should already be registered - it
        // was written as it streamed past. Verify rather than assume, so a
        // container missing content fails cleanly instead of committing a
        // meta record pointing at content that isn't there.
        for (int i = 0; i < meta.content_info_count && result == INSTALL_LOCAL_OK; i++) {
            char hex[33];
            ncm_format_content_id(&meta.content_infos[i].content_id, hex);
            char nca_filename[40];
            snprintf(nca_filename, sizeof(nca_filename), "%s.nca", hex);

            bool present = false;
            for (int j = 0; j < s->entry_count; j++) {
                if (strcmp(s->entries[j].name, nca_filename) == 0) { present = true; break; }
            }
            if (!present) {
                // Not found as a plain .nca - an NSZ/XCZ replaces some
                // content pieces with a compressed ".ncz" of the same
                // content id instead (see ncz.h). Same content either way,
                // just already-registered under this stream's own bytes-
                // as-they-arrived accounting (end_entry), not this loop's
                // concern.
                char ncz_filename[40];
                snprintf(ncz_filename, sizeof(ncz_filename), "%s.ncz", hex);
                for (int j = 0; j < s->entry_count; j++) {
                    if (strcmp(s->entries[j].name, ncz_filename) == 0) { present = true; break; }
                }
            }
            if (!present) {
                if (err_buf) snprintf(err_buf, err_buf_size, "el archivo no incluye %s (referenciado por su .cnmt)",
                                       nca_filename);
                result = INSTALL_LOCAL_ERR_PARSE;
            }
        }
        if (result != INSTALL_LOCAL_OK) break;

        NcmContentInfo cnmt_content_info;
        memset(&cnmt_content_info, 0, sizeof(cnmt_content_info));
        cnmt_content_info.content_id = cnmt_id;
        ncmU64ToContentInfoSize(s->entries[cnmt_index].size, &cnmt_content_info);
        cnmt_content_info.content_type = NcmContentType_Meta;

        if (!ncm_commit_content_meta(&s->db, &meta, &cnmt_content_info, err_buf, err_buf_size)) {
            result = INSTALL_LOCAL_ERR_NCM;
            break;
        }

        NsContentStorageRecord storage_record;
        storage_record.meta_record = meta.key;
        storage_record.storage_id = NcmStorageId_SdCard;

        Result rc = ns_push_application_record(application_id_for_meta(&meta.key), NsRecordType_Installed,
                                                &storage_record, 1);
        if (R_FAILED(rc)) {
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "el contenido se instaló, pero no se pudo registrar en el menú (0x%x)", rc);
            result = INSTALL_LOCAL_ERR_RECORD;
            break;
        }

        cnmt_index = stream_find_by_suffix(s, ".cnmt.nca", cnmt_index + 1);
    }

    if (result == INSTALL_LOCAL_OK && !import_tickets(s, err_buf, err_buf_size)) {
        result = INSTALL_LOCAL_ERR_TICKET;
    }

    // INSTALL_LOCAL_ERR_RECORD means the content is installed and only the
    // hbmenu record failed - rolling that back would throw away a good
    // install, matching install_local.c's own handling.
    if (result != INSTALL_LOCAL_OK && result != INSTALL_LOCAL_ERR_RECORD) {
        rollback_registered(s);
    }

    stream_free(s);
    return result;
}

void install_stream_abort(InstallStream *s) {
    if (!s) return;
    rollback_registered(s);
    stream_free(s);
}
