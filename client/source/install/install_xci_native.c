#include "install_xci_native.h"
#include "install_common.h"
#include "hfs0.h"
#include "xci_container.h"
#include "ncm_install.h"
#include "es_ticket.h"
#include "ns_record.h"
#include "../config.h"
#include "../net/http.h"

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

// Shares DBI's own folder on purpose, same as install_nsp_native.c - see
// install_xci_native.h's doc comment on the failure-fallback behavior.
#define DBI_DIR_PATH SWITCH_APPS_ROOT "/DBI"
#define NSP_REPO_DIR_PATH SWITCH_APPS_ROOT "/DBI/nsp-repo"

// Up to 4 ticket/cert pairs - generous headroom, matching install_nsp_native.c.
#define MAX_TICKET_PAIRS 4

// An HFS0 header/entry-table/string-table never exceeds 16 +
// HFS0_MAX_ENTRIES*sizeof(Hfs0FileEntry) + 64KB (Hfs0FileEntry is 64 bytes,
// bigger than PFS0's 24 - see hfs0.h - but the total is still well under
// this) - same "generous fixed prefix" approach as
// install_nsp_native.c's PFS0_HEADER_PREFETCH_BYTES.
#define HFS0_HEADER_PREFETCH_BYTES (128 * 1024)

static uint64_t application_id_for_meta(const NcmContentMetaKey *key) {
    // Same public Nintendo title-id convention as install_nsp_native.c.
    if (key->type == NcmContentMetaType_Patch) {
        return key->id ^ 0x800ULL;
    }
    if (key->type == NcmContentMetaType_AddOnContent) {
        return (key->id ^ 0x1000ULL) & ~0xFFFULL;
    }
    return key->id;
}

// Fetches and parses the HFS0 header/entry-table/string-table starting at
// absolute offset `header_offset` within the remote `ru` - the network
// equivalent of hfs0_parse_at(), via a single bounded Range GET into
// `tmp_path` (reusing hfs0_parse_at unmodified on that small local file),
// then shifting the parsed data_region_offset from "relative to tmp_path's
// start" to "absolute within the real remote file", since hfs0_parse_at()
// itself has no idea this temp file is really a slice starting partway
// through a bigger remote one.
static int hfs0_parse_at_url(ResolvedUrl *ru, uint64_t header_offset, const char *tmp_path,
                              Hfs0 *out, char *err_buf, size_t err_buf_size) {
    char *buf = NULL;
    size_t len = 0;
    HttpResult hres = resolved_url_get_range(ru, header_offset, HFS0_HEADER_PREFETCH_BYTES, &buf, &len, err_buf, err_buf_size);
    if (hres != HTTP_OK) return -1;

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        free(buf);
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo escribir el encabezado temporal");
        return -1;
    }
    size_t written = fwrite(buf, 1, len, fp);
    fclose(fp);
    free(buf);
    if (written != len) {
        remove(tmp_path);
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo escribir el encabezado temporal");
        return -1;
    }

    FILE *tfp = fopen(tmp_path, "rb");
    if (!tfp) {
        remove(tmp_path);
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo reabrir el encabezado temporal");
        return -1;
    }
    int rc = hfs0_parse_at(tfp, 0, out);
    fclose(tfp);
    remove(tmp_path);

    if (rc == 0) {
        out->data_region_offset += header_offset;
    } else if (err_buf) {
        // hfs0_parse_at() itself takes no err_buf (it's shared with the
        // local-file install path, which has no use for one) - filled in
        // here instead, so a failure at this step never leaves whatever
        // was in err_buf from some earlier, unrelated call still showing
        // (that's what "descarga cancelada" turning up for a completely
        // different failure turned out to be: this exact gap).
        if (rc == -2) {
            snprintf(err_buf, err_buf_size,
                     "encabezado HFS0 inválido en el offset 0x%llx (firma incorrecta o tabla malformada)",
                     (unsigned long long)header_offset);
        } else {
            snprintf(err_buf, err_buf_size, "no se pudo leer el encabezado HFS0 en el offset 0x%llx",
                     (unsigned long long)header_offset);
        }
    }
    return rc;
}

// Network equivalent of the old xci_open_secure_partition(): fetches the
// file's opening window once, locates and parses the root partition table
// inside it, finds "secure", then fetches that partition's own nested
// header - two bounded Range GETs total, regardless of how big the XCI is.
//
// The root table is parsed straight out of the window that was already
// fetched and validated, never re-requested. An earlier version re-fetched
// it by offset just to parse it, which meant a root table that verified
// fine in the first response could still fail to parse in the second -
// producing an "invalid HFS0 header" pointing at an offset that had, in
// fact, just been confirmed good.
static int xci_open_secure_partition_from_url(ResolvedUrl *ru, const char *tmp_path, Hfs0 *out,
                                               char *err_buf, size_t err_buf_size) {
    char *window = NULL;
    size_t window_len = 0;
    HttpResult hres = resolved_url_get_range(ru, 0, XCI_SEARCH_WINDOW, &window, &window_len, err_buf, err_buf_size);
    if (hres != HTTP_OK) return -1;

    uint64_t root_offset = 0;
    if (!xci_find_root_hfs0((const uint8_t *)window, window_len, &root_offset, err_buf, err_buf_size)) {
        free(window);
        return -2;
    }

    Hfs0 root;
    int rc = hfs0_parse_buffer((const uint8_t *)window, window_len, root_offset, &root);
    free(window);
    if (rc != 0) {
        if (err_buf) {
            snprintf(err_buf, err_buf_size,
                     "la tabla de particiones del XCI (offset 0x%llx) está malformada",
                     (unsigned long long)root_offset);
        }
        return -2;
    }

    int secure_index = hfs0_find_by_name(&root, "secure");
    if (secure_index < 0) {
        // Names found, listed: distinguishes "parsed fine but genuinely has
        // no 'secure'" from "parsed into garbage", which are identical from
        // a flat "not found" message alone.
        if (err_buf) {
            char names[256] = "";
            for (int i = 0; i < root.count && i < 8; i++) {
                if (i > 0) strncat(names, ", ", sizeof(names) - strlen(names) - 1);
                strncat(names, root.names[i], sizeof(names) - strlen(names) - 1);
            }
            snprintf(err_buf, err_buf_size,
                     "el archivo no es un XCI válido (partición 'secure' no encontrada; se detectaron %d "
                     "particiones: %s)",
                     root.count, root.count > 0 ? names : "ninguna");
        }
        return -2;
    }

    uint64_t secure_offset = hfs0_entry_file_offset(&root, secure_index);
    return hfs0_parse_at_url(ru, secure_offset, tmp_path, out, err_buf, err_buf_size);
}

// Deletes every content_id this install attempt itself registered for the
// cnmt currently being processed, before that cnmt's content-meta ever got
// committed - i.e. it never became a real, usable install. Without this, a
// download that fails or gets canceled partway through a multi-NCA title
// (a common case - each NCA is its own network request) leaves those
// already-registered NCAs sitting in NcmContentStorage forever: not visible
// on hbmenu (no committed meta, no app record), not cleaned up by anything,
// just permanently consuming SD space.
static void rollback_registered(NcmContentStorage *cs, const NcmContentId *ids, int count) {
    for (int i = 0; i < count; i++) {
        ncmContentStorageDelete(cs, &ids[i]);
    }
}

// Fetches a .tik/.cert pair's bytes directly over the network (a Range GET
// each - these are always tiny, a few KB) and imports them. Mirrors the
// local-file path's fseek+fread, just sourced from `ru` instead of `src`.
static bool import_ticket_from_url(ResolvedUrl *ru, const Hfs0 *hfs0, int tik_index, int cert_index,
                                    char *err_buf, size_t err_buf_size) {
    uint64_t tik_size = hfs0->entries[tik_index].file_size;
    uint64_t cert_size = hfs0->entries[cert_index].file_size;

    char *tik_buf = NULL, *cert_buf = NULL;
    size_t tik_len = 0, cert_len = 0;

    HttpResult hres = resolved_url_get_range(ru, hfs0_entry_file_offset(hfs0, tik_index), tik_size,
                                              &tik_buf, &tik_len, err_buf, err_buf_size);
    if (hres != HTTP_OK) return false;

    hres = resolved_url_get_range(ru, hfs0_entry_file_offset(hfs0, cert_index), cert_size,
                                   &cert_buf, &cert_len, err_buf, err_buf_size);
    if (hres != HTTP_OK) {
        free(tik_buf);
        return false;
    }

    bool ok = false;
    if (tik_len == tik_size && cert_len == cert_size) {
        Result rc = es_import_ticket((const uint8_t *)tik_buf, tik_len, (const uint8_t *)cert_buf, cert_len);
        if (R_SUCCEEDED(rc)) {
            ok = true;
        } else if (err_buf) {
            snprintf(err_buf, err_buf_size, "esImportTicket falló (0x%x)", rc);
        }
    } else if (err_buf) {
        snprintf(err_buf, err_buf_size, "descarga incompleta del ticket/cert");
    }

    free(tik_buf);
    free(cert_buf);
    return ok;
}

static XciInstallResult install_from_url(ResolvedUrl *ru, const Hfs0 *hfs0,
                                          InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                          void *userdata,
                                          char *err_buf, size_t err_buf_size) {
    XciInstallResult result = XCI_INSTALL_OK;

    Result rc = es_initialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio es (0x%x)", rc);
        return XCI_INSTALL_ERR_NCM;
    }

    rc = ns_record_initialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio ns (0x%x)", rc);
        es_exit();
        return XCI_INSTALL_ERR_NCM;
    }

    rc = ncmInitialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio ncm (0x%x)", rc);
        ns_record_exit();
        es_exit();
        return XCI_INSTALL_ERR_NCM;
    }

    NcmContentStorage cs;
    rc = ncmOpenContentStorage(&cs, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmOpenContentStorage falló (0x%x)", rc);
        ncmExit();
        ns_record_exit();
        es_exit();
        return XCI_INSTALL_ERR_NCM;
    }

    NcmContentMetaDatabase db;
    rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmOpenContentMetaDatabase falló (0x%x)", rc);
        ncmContentStorageClose(&cs);
        ncmExit();
        ns_record_exit();
        es_exit();
        return XCI_INSTALL_ERR_NCM;
    }

    int cnmt_index = hfs0_find_by_suffix(hfs0, ".cnmt.nca", 0);
    if (cnmt_index < 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "la partición 'secure' no contiene ningún .cnmt.nca");
        result = XCI_INSTALL_ERR_PARSE;
    }

    if (result == XCI_INSTALL_OK && phase_cb) phase_cb(INSTALL_PHASE_INSTALLING, userdata);

    // One bar for the whole title rather than one per NCA - see
    // InstallAggProgressCtx, and install_nsp_native.c which does the same.
    InstallAggProgressCtx agg = { .cb = cb, .userdata = userdata, .done_before = 0, .grand_total = 0 };

    while (cnmt_index >= 0 && result == XCI_INSTALL_OK) {
        // Content this iteration freshly registers (not content that was
        // already there, shared from some other installed title - see
        // ncm_install_content_from_url's out_registered doc comment) - rolled
        // back with ncmContentStorageDelete if this cnmt doesn't make it to
        // ncm_commit_content_meta. Once that commits, the content is real
        // and nothing past that point rolls it back, even if pushing the
        // hbmenu record afterward fails.
        NcmContentId registered_ids[NCM_MAX_CONTENT_INFOS + 1];
        int registered_count = 0;

        NcmContentId cnmt_id;
        if (!ncm_parse_content_id(hfs0->names[cnmt_index], &cnmt_id)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "nombre de archivo cnmt.nca inválido: %s", hfs0->names[cnmt_index]);
            result = XCI_INSTALL_ERR_PARSE;
            break;
        }

        uint64_t cnmt_offset = hfs0_entry_file_offset(hfs0, cnmt_index);
        uint64_t cnmt_size = hfs0->entries[cnmt_index].file_size;

        // Fresh aggregate per cnmt - see install_nsp_native.c for why.
        agg.done_before = 0;
        agg.grand_total = 0;

        bool cnmt_fresh = false;
        if (!ncm_install_content_from_url(&cs, &cnmt_id, ru, cnmt_offset, cnmt_size,
                                           install_agg_progress_cb, &agg,
                                           &cnmt_fresh, err_buf, err_buf_size)) {
            result = (err_buf && strstr(err_buf, "cancel")) ? XCI_INSTALL_ERR_CANCELED : XCI_INSTALL_ERR_NCM;
            break; // nothing registered yet (a failed call cleans up its own placeholder) - no rollback needed
        }
        if (cnmt_fresh) registered_ids[registered_count++] = cnmt_id;

        ContentMetaInfo meta;
        if (!ncm_read_content_meta(&cs, &cnmt_id, &meta, err_buf, err_buf_size)) {
            result = XCI_INSTALL_ERR_NCM;
            rollback_registered(&cs, registered_ids, registered_count);
            break;
        }

        // Only now (the cnmt states them) is this title's total known.
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

        bool content_ok = true;
        for (int i = 0; i < meta.content_info_count && content_ok; i++) {
            char hex[33];
            ncm_format_content_id(&meta.content_infos[i].content_id, hex);
            char nca_filename[40];
            snprintf(nca_filename, sizeof(nca_filename), "%s.nca", hex);

            int nca_index = hfs0_find_by_name(hfs0, nca_filename);
            if (nca_index < 0) {
                if (err_buf) snprintf(err_buf, err_buf_size, "el XCI no incluye %s (referenciado por su .cnmt)", nca_filename);
                result = XCI_INSTALL_ERR_PARSE;
                content_ok = false;
                break;
            }

            uint64_t nca_offset = hfs0_entry_file_offset(hfs0, nca_index);
            uint64_t nca_size = hfs0->entries[nca_index].file_size;
            u64 piece_size = 0;
            ncmContentInfoSizeToU64(&meta.content_infos[i], &piece_size);

            bool nca_fresh = false;
            if (!ncm_install_content_from_url(&cs, &meta.content_infos[i].content_id, ru, nca_offset, nca_size,
                                               install_agg_progress_cb, &agg, &nca_fresh, err_buf, err_buf_size)) {
                result = (err_buf && strstr(err_buf, "cancel")) ? XCI_INSTALL_ERR_CANCELED : XCI_INSTALL_ERR_NCM;
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
            result = XCI_INSTALL_ERR_NCM;
            rollback_registered(&cs, registered_ids, registered_count);
            break;
        }

        NsContentStorageRecord storage_record;
        storage_record.meta_record = meta.key;
        storage_record.storage_id = NcmStorageId_SdCard;

        rc = ns_push_application_record(application_id_for_meta(&meta.key), NsRecordType_Installed,
                                         &storage_record, 1);
        if (R_FAILED(rc)) {
            // Content + meta are already committed at this point - not
            // rolled back, unlike every break above: the content is real now.
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "el contenido se instaló, pero no se pudo registrar en el menú (0x%x)", rc);
            result = XCI_INSTALL_ERR_RECORD;
            break;
        }

        cnmt_index = hfs0_find_by_suffix(hfs0, ".cnmt.nca", cnmt_index + 1);
    }

    if (result == XCI_INSTALL_OK) {
        int tik_indices[MAX_TICKET_PAIRS];
        int cert_indices[MAX_TICKET_PAIRS];
        int tik_count = 0, cert_count = 0;

        int idx = hfs0_find_by_suffix(hfs0, ".tik", 0);
        while (idx >= 0 && tik_count < MAX_TICKET_PAIRS) {
            tik_indices[tik_count++] = idx;
            idx = hfs0_find_by_suffix(hfs0, ".tik", idx + 1);
        }
        idx = hfs0_find_by_suffix(hfs0, ".cert", 0);
        while (idx >= 0 && cert_count < MAX_TICKET_PAIRS) {
            cert_indices[cert_count++] = idx;
            idx = hfs0_find_by_suffix(hfs0, ".cert", idx + 1);
        }

        // No ticket at all is normal for a real game-card dump (retail XCI
        // content typically isn't rights-ID/ticket based) - only a
        // mismatched count is treated as an error.
        if (tik_count != cert_count) {
            if (err_buf) snprintf(err_buf, err_buf_size, "el XCI tiene un número distinto de archivos .tik y .cert");
            result = XCI_INSTALL_ERR_TICKET;
        }

        for (int i = 0; i < tik_count && result == XCI_INSTALL_OK; i++) {
            if (!import_ticket_from_url(ru, hfs0, tik_indices[i], cert_indices[i], err_buf, err_buf_size)) {
                result = XCI_INSTALL_ERR_TICKET;
            }
        }
    }

    ncmContentMetaDatabaseClose(&db);
    ncmContentStorageClose(&cs);
    ncmExit();
    ns_record_exit();
    es_exit();

    return result;
}

XciInstallResult install_xci_native(const AppEntry *entry, const char *base_url,
                                     InstallProgressCallback cb, InstallPhaseCallback phase_cb, void *userdata,
                                     char *err_buf, size_t err_buf_size) {
    install_common_mkdir_ignore_exists(SWITCH_APPS_ROOT);
    install_common_mkdir_ignore_exists(DBI_DIR_PATH);
    install_common_mkdir_ignore_exists(NSP_REPO_DIR_PATH);

    struct statvfs st;
    if (statvfs("sdmc:/", &st) == 0) {
        unsigned long long free_bytes = (unsigned long long)st.f_bsize * st.f_bavail;
        if (entry->file_size > 0 && free_bytes < (unsigned long long)entry->file_size) {
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "no hay suficiente espacio libre en la tarjeta SD (se necesitan %ld bytes)",
                                   entry->file_size);
            return XCI_INSTALL_ERR_NO_SPACE;
        }
    }

    char url[900];
    install_common_resolve_url(base_url, entry->download_url, url, sizeof(url));

    // ru remembers the direct CDN link a self-resolving proxy (like this
    // catalog's /api/dl/mediafire) resolves to on the first request below,
    // so every later request in this install (one per NCA, plus tickets)
    // reuses it instead of paying that resolve cost - and re-triggering
    // MediaFire's own page fetch/TLS handshake - again and again. See
    // ResolvedUrl's doc comment in install_common.h.
    ResolvedUrl ru;
    resolved_url_init(&ru, url);

    // Fetches just the root partition table + the "secure" partition's own
    // header (two small, near-instant requests) - every content install
    // below streams straight from `ru` into NCM, so the (potentially
    // multi-GB) XCI itself is never downloaded as a whole file. Same
    // reasoning as install_nsp_native.c: NCM already writes each NCA as its
    // own placeholder file, so as long as no single NCA is itself bigger
    // than 4GB, nothing this installer writes to the SD ever crosses
    // FAT32's 4GB-per-file limit, regardless of how big the XCI as a whole is.
    char hdr_path[512];
    snprintf(hdr_path, sizeof(hdr_path), "%s/%s.hdr", NSP_REPO_DIR_PATH, entry->filename);

    Hfs0 secure;
    int prc = xci_open_secure_partition_from_url(&ru, hdr_path, &secure, err_buf, err_buf_size);
    if (prc == -2) {
        // err_buf is already filled with the detailed diagnostic (which
        // partitions were actually found) by xci_open_secure_partition_from_url
        // itself - not overwritten here with a generic message.
        return XCI_INSTALL_ERR_PARSE;
    }
    if (prc != 0) {
        return XCI_INSTALL_ERR_DOWNLOAD;
    }

    // Same reasoning as install_nsp_native.c's identical log - see there.
    download_debug_log("install_xci_native: %s - %d secure partition entries:", entry->filename, secure.count);
    for (int i = 0; i < secure.count; i++) {
        download_debug_log("  [%d] %s (%llu bytes)", i, secure.names[i],
                            (unsigned long long)secure.entries[i].file_size);
    }

    return install_from_url(&ru, &secure, cb, phase_cb, userdata, err_buf, err_buf_size);
}
