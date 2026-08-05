#include "install_local.h"
#include "install_common.h"
#include "pfs0.h"
#include "hfs0.h"
#include "xci_container.h"
#include "ncm_install.h"
#include "ncz.h"
#include "es_ticket.h"
#include "ns_record.h"
#include "../torrent/torrent_log.h"

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Up to 4 ticket/cert pairs - matches install_nsp_native.c/install_xci_native.c.
#define MAX_TICKET_PAIRS 4

// How much of the file's head must exist before its container header can be
// parsed at all. Covers both formats' worst case: an XCI's root-partition
// search window (XCI_SEARCH_WINDOW, 0x20000) and a PFS0 header at
// PFS0_MAX_ENTRIES (0x10 + 128*0x18 + names, ~12KB).
#define HEADER_PREFETCH_BYTES 0x20000

// A gated file is being appended to while we read it. stdio's read-ahead
// would happily cache a block that was still zero-filled when it was
// pulled in and keep serving those stale zeros after the real bytes
// landed, so reads go straight to the filesystem instead. Costs nothing
// here in practice: the bulk reads are already whole 4MB scratch-buffer
// chunks (see ncm_install_content), where buffering only adds a copy.
static void set_unbuffered_if_gated(FILE *src, const InstallLocalGate *gate) {
    if (gate && gate->ensure_range) setvbuf(src, NULL, _IONBF, 0);
}

// NULL gate = an ordinary complete file (the SD explorer's case); every
// range is trivially already there.
//
// `src` (may be NULL, or point to a NULL FILE*) is this side's read handle
// on `path`. It is CLOSED for the duration of the wait and reopened
// afterwards, because the Switch's filesystem refuses to open a file for
// reading while it is still open for writing elsewhere - and the thing
// filling this file is doing exactly that. So the downloader and the
// installer take strict turns on the handle: the downloader drops its own
// (torrent_storage.c's storage_commit) before handing control back here,
// and this drops ours before handing control there.
static bool gate_ensure(const InstallLocalGate *gate, FILE **src, const char *path,
                        uint64_t offset, uint64_t len);

// Adapts this file's gate to the lower-level InstallReadGate the content
// readers take, so they can wait chunk by chunk instead of this waiting
// for a whole content piece up front (see install_common.h).
typedef struct {
    const InstallLocalGate *gate;
    const char *path;
} ReadGateCtx;

static bool read_gate_ensure(void *user, FILE **src, uint64_t offset, uint64_t len) {
    ReadGateCtx *ctx = (ReadGateCtx *)user;
    return gate_ensure(ctx->gate, src, ctx->path, offset, len);
}

static bool gate_ensure(const InstallLocalGate *gate, FILE **src, const char *path,
                        uint64_t offset, uint64_t len) {
    if (!gate || !gate->ensure_range) return true;

    if (src && *src) {
        fclose(*src);
        *src = NULL;
    }
    bool ok = gate->ensure_range(gate->user, offset, len);
    if (src) {
        *src = fopen(path, "rb");
        if (!*src) {
            // Indistinguishable from a real cancel one level up (this
            // function just returns false either way) - logged here since
            // this is the only place that knows the reopen itself is what
            // failed, not the data actually being unavailable (ok can be
            // true here - see torrent_gate_ensure's fast-path doc comment
            // on the write-handle race this used to hit before it started
            // flushing there too).
            torrent_debug_log("[install] gate_ensure: reopen for read failed (ok=%d) '%s' off=%llu len=%llu",
                              ok, path, (unsigned long long)offset, (unsigned long long)len);
            return false;
        }
        set_unbuffered_if_gated(*src, gate);
    }
    return ok;
}

static uint64_t application_id_for_meta(const NcmContentMetaKey *key) {
    // Public Nintendo title-id convention (documented on switchbrew.org):
    // a patch/update's application id is its own id XOR 0x800; an
    // AddOnContent (DLC)'s is (id XOR 0x1000) masked to the base id.
    if (key->type == NcmContentMetaType_Patch) {
        return key->id ^ 0x800ULL;
    }
    if (key->type == NcmContentMetaType_AddOnContent) {
        return (key->id ^ 0x1000ULL) & ~0xFFFULL;
    }
    return key->id;
}

// Same role as install_nsp_native.c/install_xci_native.c's rollback_registered
// - see that comment for why this exists. Local installs can fail/get
// canceled partway through a multi-NCA title exactly the same way network
// ones can, so they need the same cleanup.
static void rollback_registered(NcmContentStorage *cs, const NcmContentId *ids, int count) {
    for (int i = 0; i < count; i++) {
        ncmContentStorageDelete(cs, &ids[i]);
    }
}

// Opens the XCI's "secure" partition (the one holding the actual title's
// NCAs/cnmt/tik/cert) from an already-open local file: reads the gamecard
// header to find where the root partition table actually is (see
// xci_container.h - a trimmed dump shifts this earlier than an untrimmed
// one's fixed 0x10000, so it can't be a hardcoded constant), parses that
// root table, finds "secure" within it, then parses that partition's own
// nested header. Returns 0 on success, -1 on I/O error, -2 if the file
// isn't a valid XCI or has no "secure" partition.
static int xci_open_secure_partition_local(FILE **fpp, const char *path,
                                            const InstallLocalGate *gate, Hfs0 *out) {
    FILE *fp = *fpp;
    // static: XCI_SEARCH_WINDOW is far too big for a stack frame, and local
    // installs are serial (one file at a time from the explorer).
    static uint8_t window[XCI_SEARCH_WINDOW];
    if (fseek(fp, 0, SEEK_SET) != 0) return -1;
    size_t window_len = fread(window, 1, sizeof(window), fp);
    if (window_len < XCI_HEADER_SIZE) return -1;

    uint64_t root_offset = 0;
    if (!xci_find_root_hfs0(window, window_len, &root_offset, NULL, 0)) return -2;

    // Parsed from the same bytes the offset was found in - see
    // hfs0_parse_buffer's doc comment.
    Hfs0 root;
    int rc = hfs0_parse_buffer(window, window_len, root_offset, &root);
    if (rc != 0) return rc;

    int secure_index = hfs0_find_by_name(&root, "secure");
    if (secure_index < 0) return -2;

    uint64_t secure_offset = hfs0_entry_file_offset(&root, secure_index);
    // Its header is small, but it sits wherever the root table points -
    // for a still-downloading file that can be past the frontier. The gate
    // closes and reopens our handle, so pick it back up before reading.
    if (!gate_ensure(gate, fpp, path, secure_offset, HEADER_PREFETCH_BYTES) || !*fpp) return -1;
    return hfs0_parse_at(*fpp, secure_offset, out);
}

// ---- NSP ----

static bool import_ticket_nsp_local(FILE *src, const Pfs0 *pfs0, int tik_index, int cert_index,
                                     char *err_buf, size_t err_buf_size) {
    uint64_t tik_size = pfs0->entries[tik_index].file_size;
    uint64_t cert_size = pfs0->entries[cert_index].file_size;

    uint8_t *tik_buf = (uint8_t *)malloc((size_t)tik_size);
    uint8_t *cert_buf = (uint8_t *)malloc((size_t)cert_size);
    if (!tik_buf || !cert_buf) {
        free(tik_buf);
        free(cert_buf);
        if (err_buf) snprintf(err_buf, err_buf_size, "memoria insuficiente para el ticket");
        return false;
    }

    fseek(src, (long)pfs0_entry_file_offset(pfs0, tik_index), SEEK_SET);
    size_t tik_read = fread(tik_buf, 1, (size_t)tik_size, src);
    fseek(src, (long)pfs0_entry_file_offset(pfs0, cert_index), SEEK_SET);
    size_t cert_read = fread(cert_buf, 1, (size_t)cert_size, src);

    bool ok = false;
    if (tik_read == (size_t)tik_size && cert_read == (size_t)cert_size) {
        Result rc = es_import_ticket(tik_buf, (size_t)tik_size, cert_buf, (size_t)cert_size);
        if (R_SUCCEEDED(rc)) {
            ok = true;
        } else if (err_buf) {
            snprintf(err_buf, err_buf_size, "esImportTicket falló (0x%x)", rc);
        }
    } else if (err_buf) {
        snprintf(err_buf, err_buf_size, "lectura incompleta del ticket/cert");
    }

    free(tik_buf);
    free(cert_buf);
    return ok;
}

InstallLocalResult install_nsp_from_local_file_ex(const char *path, const InstallLocalGate *gate,
                                                   InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                                   void *userdata, char *err_buf, size_t err_buf_size) {
    // No reader handle to hand over yet - pfs0_open opens its own below,
    // once the downloader has let go of the file.
    if (!gate_ensure(gate, NULL, path, 0, HEADER_PREFETCH_BYTES)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "instalación cancelada");
        return INSTALL_LOCAL_ERR_CANCELED;
    }

    Pfs0 pfs0;
    if (pfs0_open(path, &pfs0) != 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "el archivo no es un NSP (PFS0) válido");
        return INSTALL_LOCAL_ERR_PARSE;
    }

    FILE *src = fopen(path, "rb");
    if (!src) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo abrir el archivo");
        return INSTALL_LOCAL_ERR_PARSE;
    }
    set_unbuffered_if_gated(src, gate);

    InstallLocalResult result = INSTALL_LOCAL_OK;

    Result rc = es_initialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio es (0x%x)", rc);
        fclose(src);
        return INSTALL_LOCAL_ERR_NCM;
    }
    rc = ns_record_initialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio ns (0x%x)", rc);
        es_exit();
        fclose(src);
        return INSTALL_LOCAL_ERR_NCM;
    }
    rc = ncmInitialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio ncm (0x%x)", rc);
        ns_record_exit();
        es_exit();
        fclose(src);
        return INSTALL_LOCAL_ERR_NCM;
    }

    NcmContentStorage cs;
    rc = ncmOpenContentStorage(&cs, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmOpenContentStorage falló (0x%x)", rc);
        ncmExit(); ns_record_exit(); es_exit(); fclose(src);
        return INSTALL_LOCAL_ERR_NCM;
    }

    NcmContentMetaDatabase db;
    rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmOpenContentMetaDatabase falló (0x%x)", rc);
        ncmContentStorageClose(&cs);
        ncmExit(); ns_record_exit(); es_exit(); fclose(src);
        return INSTALL_LOCAL_ERR_NCM;
    }

    // Handed to the content readers so they wait chunk by chunk. NULL for
    // an ordinary complete file, which keeps the SD explorer's path
    // byte-for-byte what it was.
    ReadGateCtx read_gate_ctx = { .gate = gate, .path = path };
    InstallReadGate read_gate_storage = { .ensure = read_gate_ensure, .user = &read_gate_ctx };
    const InstallReadGate *read_gate =
        (gate && gate->ensure_range) ? &read_gate_storage : NULL;

    int cnmt_index = pfs0_find_by_suffix(&pfs0, ".cnmt.nca", 0);
    if (cnmt_index < 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "el NSP no contiene ningún .cnmt.nca");
        result = INSTALL_LOCAL_ERR_PARSE;
    }

    if (result == INSTALL_LOCAL_OK && phase_cb) phase_cb(INSTALL_PHASE_INSTALLING, userdata);

    // One bar for the whole title rather than one per NCA - see
    // InstallAggProgressCtx.
    InstallAggProgressCtx agg = { .cb = cb, .userdata = userdata, .done_before = 0, .grand_total = 0 };

    while (cnmt_index >= 0 && result == INSTALL_LOCAL_OK) {
        NcmContentId registered_ids[NCM_MAX_CONTENT_INFOS + 1];
        int registered_count = 0;

        NcmContentId cnmt_id;
        if (!ncm_parse_content_id(pfs0.names[cnmt_index], &cnmt_id)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "nombre de archivo cnmt.nca inválido: %s", pfs0.names[cnmt_index]);
            result = INSTALL_LOCAL_ERR_PARSE;
            break;
        }

        uint64_t cnmt_offset = pfs0_entry_file_offset(&pfs0, cnmt_index);
        uint64_t cnmt_size = pfs0.entries[cnmt_index].file_size;

        agg.done_before = 0;
        agg.grand_total = 0;

        if (!gate_ensure(gate, &src, path, cnmt_offset, cnmt_size) || !src) {
            if (err_buf) snprintf(err_buf, err_buf_size, "instalación cancelada");
            result = INSTALL_LOCAL_ERR_CANCELED;
            break;
        }

        bool cnmt_fresh = false;
        if (!ncm_install_content(&cs, &cnmt_id, &src, cnmt_offset, cnmt_size, read_gate,
                                  install_agg_progress_cb, &agg, &cnmt_fresh, err_buf, err_buf_size)) {
            result = (err_buf && strstr(err_buf, "cancel")) ? INSTALL_LOCAL_ERR_CANCELED : INSTALL_LOCAL_ERR_NCM;
            break;
        }
        if (cnmt_fresh) registered_ids[registered_count++] = cnmt_id;

        ContentMetaInfo meta;
        if (!ncm_read_content_meta(&cs, &cnmt_id, &meta, err_buf, err_buf_size)) {
            result = INSTALL_LOCAL_ERR_NCM;
            rollback_registered(&cs, registered_ids, registered_count);
            break;
        }

        {
            uint64_t total_bytes = cnmt_size;
            for (int i = 0; i < meta.content_info_count; i++) {
                u64 piece = 0;
                ncmContentInfoSizeToU64(&meta.content_infos[i], &piece);
                total_bytes += piece;
            }
            agg.grand_total = total_bytes;
            agg.done_before = cnmt_size;
        }

        // Resolve every content piece to its PFS0 entry up front, then walk
        // them in ascending file-offset order rather than CNMT order. For
        // an ordinary complete file that changes nothing; for a gated one
        // it is what makes the install track the download instead of
        // fighting it, since the container fills front-to-back (see
        // InstallLocalGate).
        struct { int content_idx; int pfs0_idx; bool is_ncz; uint64_t offset; }
            pieces[NCM_MAX_CONTENT_INFOS];
        int piece_count = 0;
        bool content_ok = true;

        for (int i = 0; i < meta.content_info_count; i++) {
            char hex[33];
            ncm_format_content_id(&meta.content_infos[i].content_id, hex);
            char nca_filename[40];
            snprintf(nca_filename, sizeof(nca_filename), "%s.nca", hex);

            int nca_index = -1;
            for (int j = 0; j < pfs0.count; j++) {
                if (strcmp(pfs0.names[j], nca_filename) == 0) { nca_index = j; break; }
            }

            // Not found as a plain .nca - an NSZ replaces some content
            // pieces (typically everything but the tiny Meta NCA) with a
            // compressed ".ncz" of the same content id (see ncz.h), which
            // is the exact same PFS0/CNMT structure otherwise. Mirrors
            // install_nsp_native.c's own lookup for the network path.
            bool is_ncz = false;
            if (nca_index < 0) {
                char ncz_filename[40];
                snprintf(ncz_filename, sizeof(ncz_filename), "%s.ncz", hex);
                for (int j = 0; j < pfs0.count; j++) {
                    if (strcmp(pfs0.names[j], ncz_filename) == 0) { nca_index = j; is_ncz = true; break; }
                }
            }
            if (nca_index < 0) {
                if (err_buf) snprintf(err_buf, err_buf_size, "el NSP no incluye %s (referenciado por su .cnmt)", nca_filename);
                result = INSTALL_LOCAL_ERR_PARSE;
                content_ok = false;
                break;
            }

            pieces[piece_count].content_idx = i;
            pieces[piece_count].pfs0_idx = nca_index;
            pieces[piece_count].is_ncz = is_ncz;
            pieces[piece_count].offset = pfs0_entry_file_offset(&pfs0, nca_index);
            piece_count++;
        }

        // Insertion sort - piece_count is at most NCM_MAX_CONTENT_INFOS (32).
        for (int a = 1; a < piece_count; a++) {
            typeof(pieces[0]) key = pieces[a];
            int b = a - 1;
            while (b >= 0 && pieces[b].offset > key.offset) { pieces[b + 1] = pieces[b]; b--; }
            pieces[b + 1] = key;
        }

        for (int p = 0; p < piece_count && content_ok; p++) {
            int i = pieces[p].content_idx;
            int nca_index = pieces[p].pfs0_idx;
            bool is_ncz = pieces[p].is_ncz;

            uint64_t nca_offset = pfs0_entry_file_offset(&pfs0, nca_index);
            uint64_t nca_size = pfs0.entries[nca_index].file_size;
            u64 piece_size = 0;
            ncmContentInfoSizeToU64(&meta.content_infos[i], &piece_size);

            // Deliberately NOT gated on the whole [nca_offset, nca_size)
            // range here: these pieces are routinely the entire container
            // (one ~54 MB .ncz was what exposed this), so waiting for all
            // of it before starting is just "download first, install
            // after" again. The readers below gate chunk by chunk instead.
            bool nca_fresh = false;
            bool content_installed;
            if (is_ncz) {
                content_installed = ncm_install_ncz_content_from_file(&cs, &meta.content_infos[i].content_id, &src,
                                                                       nca_offset, nca_size, piece_size, read_gate,
                                                                       install_agg_progress_cb, &agg, &nca_fresh,
                                                                       err_buf, err_buf_size);
            } else {
                content_installed = ncm_install_content(&cs, &meta.content_infos[i].content_id, &src, nca_offset, nca_size,
                                                          read_gate, install_agg_progress_cb, &agg, &nca_fresh, err_buf, err_buf_size);
            }
            if (!content_installed) {
                result = (err_buf && strstr(err_buf, "cancel")) ? INSTALL_LOCAL_ERR_CANCELED : INSTALL_LOCAL_ERR_NCM;
                content_ok = false;
                break;
            }
            agg.done_before += piece_size;
            if (nca_fresh && registered_count < NCM_MAX_CONTENT_INFOS + 1) {
                registered_ids[registered_count++] = meta.content_infos[i].content_id;
            }
        }
        if (!content_ok) {
            rollback_registered(&cs, registered_ids, registered_count);
            break;
        }

        NcmContentInfo cnmt_content_info;
        memset(&cnmt_content_info, 0, sizeof(cnmt_content_info));
        cnmt_content_info.content_id = cnmt_id;
        ncmU64ToContentInfoSize(cnmt_size, &cnmt_content_info);
        cnmt_content_info.content_type = NcmContentType_Meta;

        if (!ncm_commit_content_meta(&db, &meta, &cnmt_content_info, err_buf, err_buf_size)) {
            result = INSTALL_LOCAL_ERR_NCM;
            rollback_registered(&cs, registered_ids, registered_count);
            break;
        }

        NsContentStorageRecord storage_record;
        storage_record.meta_record = meta.key;
        storage_record.storage_id = NcmStorageId_SdCard;

        rc = ns_push_application_record(application_id_for_meta(&meta.key), NsRecordType_Installed,
                                         &storage_record, 1);
        if (R_FAILED(rc)) {
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "el contenido se instaló, pero no se pudo registrar en el menú (0x%x)", rc);
            result = INSTALL_LOCAL_ERR_RECORD;
            break;
        }

        cnmt_index = pfs0_find_by_suffix(&pfs0, ".cnmt.nca", cnmt_index + 1);
    }

    if (result == INSTALL_LOCAL_OK) {
        int tik_indices[MAX_TICKET_PAIRS];
        int cert_indices[MAX_TICKET_PAIRS];
        int tik_count = 0, cert_count = 0;

        int idx = pfs0_find_by_suffix(&pfs0, ".tik", 0);
        while (idx >= 0 && tik_count < MAX_TICKET_PAIRS) {
            tik_indices[tik_count++] = idx;
            idx = pfs0_find_by_suffix(&pfs0, ".tik", idx + 1);
        }
        idx = pfs0_find_by_suffix(&pfs0, ".cert", 0);
        while (idx >= 0 && cert_count < MAX_TICKET_PAIRS) {
            cert_indices[cert_count++] = idx;
            idx = pfs0_find_by_suffix(&pfs0, ".cert", idx + 1);
        }

        if (tik_count != cert_count) {
            if (err_buf) snprintf(err_buf, err_buf_size, "el NSP tiene un número distinto de archivos .tik y .cert");
            result = INSTALL_LOCAL_ERR_TICKET;
        }

        for (int i = 0; i < tik_count && result == INSTALL_LOCAL_OK; i++) {
            // Tickets are tiny but sit wherever the packer put them, which
            // for a gated file can be past the download frontier.
            if (!gate_ensure(gate, &src, path, pfs0_entry_file_offset(&pfs0, tik_indices[i]),
                             pfs0.entries[tik_indices[i]].file_size) ||
                !gate_ensure(gate, &src, path, pfs0_entry_file_offset(&pfs0, cert_indices[i]),
                             pfs0.entries[cert_indices[i]].file_size) || !src) {
                if (err_buf) snprintf(err_buf, err_buf_size, "instalación cancelada");
                result = INSTALL_LOCAL_ERR_CANCELED;
                break;
            }
            if (!import_ticket_nsp_local(src, &pfs0, tik_indices[i], cert_indices[i], err_buf, err_buf_size)) {
                result = INSTALL_LOCAL_ERR_TICKET;
            }
        }
    }

    fclose(src);
    ncmContentMetaDatabaseClose(&db);
    ncmContentStorageClose(&cs);
    ncmExit();
    ns_record_exit();
    es_exit();

    return result;
}

InstallLocalResult install_nsp_from_local_file(const char *path,
                                                InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                                void *userdata, char *err_buf, size_t err_buf_size) {
    return install_nsp_from_local_file_ex(path, NULL, cb, phase_cb, userdata, err_buf, err_buf_size);
}

// ---- XCI ----

static bool import_ticket_xci_local(FILE *src, const Hfs0 *hfs0, int tik_index, int cert_index,
                                     char *err_buf, size_t err_buf_size) {
    uint64_t tik_size = hfs0->entries[tik_index].file_size;
    uint64_t cert_size = hfs0->entries[cert_index].file_size;

    uint8_t *tik_buf = (uint8_t *)malloc((size_t)tik_size);
    uint8_t *cert_buf = (uint8_t *)malloc((size_t)cert_size);
    if (!tik_buf || !cert_buf) {
        free(tik_buf);
        free(cert_buf);
        if (err_buf) snprintf(err_buf, err_buf_size, "memoria insuficiente para el ticket");
        return false;
    }

    fseek(src, (long)hfs0_entry_file_offset(hfs0, tik_index), SEEK_SET);
    size_t tik_read = fread(tik_buf, 1, (size_t)tik_size, src);
    fseek(src, (long)hfs0_entry_file_offset(hfs0, cert_index), SEEK_SET);
    size_t cert_read = fread(cert_buf, 1, (size_t)cert_size, src);

    bool ok = false;
    if (tik_read == (size_t)tik_size && cert_read == (size_t)cert_size) {
        Result rc = es_import_ticket(tik_buf, (size_t)tik_size, cert_buf, (size_t)cert_size);
        if (R_SUCCEEDED(rc)) {
            ok = true;
        } else if (err_buf) {
            snprintf(err_buf, err_buf_size, "esImportTicket falló (0x%x)", rc);
        }
    } else if (err_buf) {
        snprintf(err_buf, err_buf_size, "lectura incompleta del ticket/cert");
    }

    free(tik_buf);
    free(cert_buf);
    return ok;
}

InstallLocalResult install_xci_from_local_file_ex(const char *path, const InstallLocalGate *gate,
                                                   InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                                   void *userdata, char *err_buf, size_t err_buf_size) {
    FILE *src = fopen(path, "rb");
    if (!src) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo abrir el archivo");
        return INSTALL_LOCAL_ERR_PARSE;
    }
    set_unbuffered_if_gated(src, gate);

    // The gamecard header + root partition table live in the first
    // XCI_SEARCH_WINDOW bytes; the "secure" partition's own header sits
    // wherever that table points, which xci_open_secure_partition_local
    // seeks to directly - hence the gate is handed down into it.
    if (!gate_ensure(gate, &src, path, 0, HEADER_PREFETCH_BYTES) || !src) {
        if (src) fclose(src);
        if (err_buf) snprintf(err_buf, err_buf_size, "instalación cancelada");
        return INSTALL_LOCAL_ERR_CANCELED;
    }

    Hfs0 hfs0;
    if (xci_open_secure_partition_local(&src, path, gate, &hfs0) != 0) {
        if (src) fclose(src);
        if (err_buf) snprintf(err_buf, err_buf_size, "el archivo no es un XCI válido (partición 'secure' no encontrada)");
        return INSTALL_LOCAL_ERR_PARSE;
    }

    InstallLocalResult result = INSTALL_LOCAL_OK;

    Result rc = es_initialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio es (0x%x)", rc);
        fclose(src);
        return INSTALL_LOCAL_ERR_NCM;
    }
    rc = ns_record_initialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio ns (0x%x)", rc);
        es_exit();
        fclose(src);
        return INSTALL_LOCAL_ERR_NCM;
    }
    rc = ncmInitialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio ncm (0x%x)", rc);
        ns_record_exit();
        es_exit();
        fclose(src);
        return INSTALL_LOCAL_ERR_NCM;
    }

    NcmContentStorage cs;
    rc = ncmOpenContentStorage(&cs, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmOpenContentStorage falló (0x%x)", rc);
        ncmExit(); ns_record_exit(); es_exit(); fclose(src);
        return INSTALL_LOCAL_ERR_NCM;
    }

    NcmContentMetaDatabase db;
    rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmOpenContentMetaDatabase falló (0x%x)", rc);
        ncmContentStorageClose(&cs);
        ncmExit(); ns_record_exit(); es_exit(); fclose(src);
        return INSTALL_LOCAL_ERR_NCM;
    }

    ReadGateCtx read_gate_ctx = { .gate = gate, .path = path };
    InstallReadGate read_gate_storage = { .ensure = read_gate_ensure, .user = &read_gate_ctx };
    const InstallReadGate *read_gate =
        (gate && gate->ensure_range) ? &read_gate_storage : NULL;

    int cnmt_index = hfs0_find_by_suffix(&hfs0, ".cnmt.nca", 0);
    if (cnmt_index < 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "la partición 'secure' no contiene ningún .cnmt.nca");
        result = INSTALL_LOCAL_ERR_PARSE;
    }

    if (result == INSTALL_LOCAL_OK && phase_cb) phase_cb(INSTALL_PHASE_INSTALLING, userdata);

    // One bar for the whole title rather than one per NCA - see
    // InstallAggProgressCtx.
    InstallAggProgressCtx agg = { .cb = cb, .userdata = userdata, .done_before = 0, .grand_total = 0 };

    while (cnmt_index >= 0 && result == INSTALL_LOCAL_OK) {
        NcmContentId registered_ids[NCM_MAX_CONTENT_INFOS + 1];
        int registered_count = 0;

        NcmContentId cnmt_id;
        if (!ncm_parse_content_id(hfs0.names[cnmt_index], &cnmt_id)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "nombre de archivo cnmt.nca inválido: %s", hfs0.names[cnmt_index]);
            result = INSTALL_LOCAL_ERR_PARSE;
            break;
        }

        uint64_t cnmt_offset = hfs0_entry_file_offset(&hfs0, cnmt_index);
        uint64_t cnmt_size = hfs0.entries[cnmt_index].file_size;

        agg.done_before = 0;
        agg.grand_total = 0;

        if (!gate_ensure(gate, &src, path, cnmt_offset, cnmt_size) || !src) {
            if (err_buf) snprintf(err_buf, err_buf_size, "instalación cancelada");
            result = INSTALL_LOCAL_ERR_CANCELED;
            break;
        }

        bool cnmt_fresh = false;
        if (!ncm_install_content(&cs, &cnmt_id, &src, cnmt_offset, cnmt_size, read_gate,
                                  install_agg_progress_cb, &agg, &cnmt_fresh, err_buf, err_buf_size)) {
            result = (err_buf && strstr(err_buf, "cancel")) ? INSTALL_LOCAL_ERR_CANCELED : INSTALL_LOCAL_ERR_NCM;
            break;
        }
        if (cnmt_fresh) registered_ids[registered_count++] = cnmt_id;

        ContentMetaInfo meta;
        if (!ncm_read_content_meta(&cs, &cnmt_id, &meta, err_buf, err_buf_size)) {
            result = INSTALL_LOCAL_ERR_NCM;
            rollback_registered(&cs, registered_ids, registered_count);
            break;
        }

        {
            uint64_t total_bytes = cnmt_size;
            for (int i = 0; i < meta.content_info_count; i++) {
                u64 piece = 0;
                ncmContentInfoSizeToU64(&meta.content_infos[i], &piece);
                total_bytes += piece;
            }
            agg.grand_total = total_bytes;
            agg.done_before = cnmt_size;
        }

        // Same up-front resolve + ascending-file-offset walk as the NSP
        // path above - see the comment there.
        struct { int content_idx; int hfs0_idx; bool is_ncz; uint64_t offset; }
            pieces[NCM_MAX_CONTENT_INFOS];
        int piece_count = 0;
        bool content_ok = true;

        for (int i = 0; i < meta.content_info_count; i++) {
            char hex[33];
            ncm_format_content_id(&meta.content_infos[i].content_id, hex);
            char nca_filename[40];
            snprintf(nca_filename, sizeof(nca_filename), "%s.nca", hex);

            int nca_index = hfs0_find_by_name(&hfs0, nca_filename);

            // An XCZ compresses its content pieces to ".ncz" exactly like
            // an NSZ does inside an NSP - same CNMT, same content ids, only
            // the stored entry differs. See the NSP path above and ncz.h.
            bool is_ncz = false;
            if (nca_index < 0) {
                char ncz_filename[40];
                snprintf(ncz_filename, sizeof(ncz_filename), "%s.ncz", hex);
                nca_index = hfs0_find_by_name(&hfs0, ncz_filename);
                if (nca_index >= 0) is_ncz = true;
            }
            if (nca_index < 0) {
                if (err_buf) snprintf(err_buf, err_buf_size, "el XCI no incluye %s (referenciado por su .cnmt)", nca_filename);
                result = INSTALL_LOCAL_ERR_PARSE;
                content_ok = false;
                break;
            }

            pieces[piece_count].content_idx = i;
            pieces[piece_count].hfs0_idx = nca_index;
            pieces[piece_count].is_ncz = is_ncz;
            pieces[piece_count].offset = hfs0_entry_file_offset(&hfs0, nca_index);
            piece_count++;
        }

        for (int a = 1; a < piece_count; a++) {
            typeof(pieces[0]) key = pieces[a];
            int b = a - 1;
            while (b >= 0 && pieces[b].offset > key.offset) { pieces[b + 1] = pieces[b]; b--; }
            pieces[b + 1] = key;
        }

        for (int p = 0; p < piece_count && content_ok; p++) {
            int i = pieces[p].content_idx;
            int nca_index = pieces[p].hfs0_idx;
            bool is_ncz = pieces[p].is_ncz;

            uint64_t nca_offset = hfs0_entry_file_offset(&hfs0, nca_index);
            uint64_t nca_size = hfs0.entries[nca_index].file_size;
            u64 piece_size = 0;
            ncmContentInfoSizeToU64(&meta.content_infos[i], &piece_size);

            // Deliberately NOT gated on the whole [nca_offset, nca_size)
            // range here: these pieces are routinely the entire container
            // (one ~54 MB .ncz was what exposed this), so waiting for all
            // of it before starting is just "download first, install
            // after" again. The readers below gate chunk by chunk instead.
            bool nca_fresh = false;
            bool content_installed;
            if (is_ncz) {
                content_installed = ncm_install_ncz_content_from_file(&cs, &meta.content_infos[i].content_id, &src,
                                                                       nca_offset, nca_size, piece_size, read_gate,
                                                                       install_agg_progress_cb, &agg, &nca_fresh,
                                                                       err_buf, err_buf_size);
            } else {
                content_installed = ncm_install_content(&cs, &meta.content_infos[i].content_id, &src, nca_offset, nca_size,
                                                          read_gate, install_agg_progress_cb, &agg, &nca_fresh, err_buf, err_buf_size);
            }
            if (!content_installed) {
                result = (err_buf && strstr(err_buf, "cancel")) ? INSTALL_LOCAL_ERR_CANCELED : INSTALL_LOCAL_ERR_NCM;
                content_ok = false;
                break;
            }
            agg.done_before += piece_size;
            if (nca_fresh && registered_count < NCM_MAX_CONTENT_INFOS + 1) {
                registered_ids[registered_count++] = meta.content_infos[i].content_id;
            }
        }
        if (!content_ok) {
            rollback_registered(&cs, registered_ids, registered_count);
            break;
        }

        NcmContentInfo cnmt_content_info;
        memset(&cnmt_content_info, 0, sizeof(cnmt_content_info));
        cnmt_content_info.content_id = cnmt_id;
        ncmU64ToContentInfoSize(cnmt_size, &cnmt_content_info);
        cnmt_content_info.content_type = NcmContentType_Meta;

        if (!ncm_commit_content_meta(&db, &meta, &cnmt_content_info, err_buf, err_buf_size)) {
            result = INSTALL_LOCAL_ERR_NCM;
            rollback_registered(&cs, registered_ids, registered_count);
            break;
        }

        NsContentStorageRecord storage_record;
        storage_record.meta_record = meta.key;
        storage_record.storage_id = NcmStorageId_SdCard;

        rc = ns_push_application_record(application_id_for_meta(&meta.key), NsRecordType_Installed,
                                         &storage_record, 1);
        if (R_FAILED(rc)) {
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "el contenido se instaló, pero no se pudo registrar en el menú (0x%x)", rc);
            result = INSTALL_LOCAL_ERR_RECORD;
            break;
        }

        cnmt_index = hfs0_find_by_suffix(&hfs0, ".cnmt.nca", cnmt_index + 1);
    }

    if (result == INSTALL_LOCAL_OK) {
        int tik_indices[MAX_TICKET_PAIRS];
        int cert_indices[MAX_TICKET_PAIRS];
        int tik_count = 0, cert_count = 0;

        int idx = hfs0_find_by_suffix(&hfs0, ".tik", 0);
        while (idx >= 0 && tik_count < MAX_TICKET_PAIRS) {
            tik_indices[tik_count++] = idx;
            idx = hfs0_find_by_suffix(&hfs0, ".tik", idx + 1);
        }
        idx = hfs0_find_by_suffix(&hfs0, ".cert", 0);
        while (idx >= 0 && cert_count < MAX_TICKET_PAIRS) {
            cert_indices[cert_count++] = idx;
            idx = hfs0_find_by_suffix(&hfs0, ".cert", idx + 1);
        }

        if (tik_count != cert_count) {
            if (err_buf) snprintf(err_buf, err_buf_size, "el XCI tiene un número distinto de archivos .tik y .cert");
            result = INSTALL_LOCAL_ERR_TICKET;
        }

        for (int i = 0; i < tik_count && result == INSTALL_LOCAL_OK; i++) {
            if (!gate_ensure(gate, &src, path, hfs0_entry_file_offset(&hfs0, tik_indices[i]),
                             hfs0.entries[tik_indices[i]].file_size) ||
                !gate_ensure(gate, &src, path, hfs0_entry_file_offset(&hfs0, cert_indices[i]),
                             hfs0.entries[cert_indices[i]].file_size) || !src) {
                if (err_buf) snprintf(err_buf, err_buf_size, "instalación cancelada");
                result = INSTALL_LOCAL_ERR_CANCELED;
                break;
            }
            if (!import_ticket_xci_local(src, &hfs0, tik_indices[i], cert_indices[i], err_buf, err_buf_size)) {
                result = INSTALL_LOCAL_ERR_TICKET;
            }
        }
    }

    fclose(src);
    ncmContentMetaDatabaseClose(&db);
    ncmContentStorageClose(&cs);
    ncmExit();
    ns_record_exit();
    es_exit();

    return result;
}

InstallLocalResult install_xci_from_local_file(const char *path,
                                                InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                                void *userdata, char *err_buf, size_t err_buf_size) {
    return install_xci_from_local_file_ex(path, NULL, cb, phase_cb, userdata, err_buf, err_buf_size);
}
