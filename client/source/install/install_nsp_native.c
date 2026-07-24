#include "install_nsp_native.h"
#include "install_common.h"
#include "pfs0.h"
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

// Shares DBI's own folder on purpose - see install_nsp_native.h's doc comment
// on the failure-fallback behavior.
#define DBI_DIR_PATH SWITCH_APPS_ROOT "/DBI"
#define NSP_REPO_DIR_PATH SWITCH_APPS_ROOT "/DBI/nsp-repo"

// Up to 4 ticket/cert pairs - every catalog NSP encountered so far has 0 or 1;
// generous headroom for multi-title bundles without unbounded allocation.
#define MAX_TICKET_PAIRS 4

// A PFS0 header/entry-table/string-table never exceeds
// 16 + PFS0_MAX_ENTRIES*24 + 64KB (~68.6KB - see pfs0.h/pfs0_open) - fetch a
// generous fixed prefix of the remote file in one request instead of
// guessing the exact size, then parse it with the same pfs0_open() the
// local-file path always used.
#define PFS0_HEADER_PREFETCH_BYTES (128 * 1024)

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
static bool import_ticket_from_url(ResolvedUrl *ru, const Pfs0 *pfs0, int tik_index, int cert_index,
                                    char *err_buf, size_t err_buf_size) {
    uint64_t tik_size = pfs0->entries[tik_index].file_size;
    uint64_t cert_size = pfs0->entries[cert_index].file_size;

    char *tik_buf = NULL, *cert_buf = NULL;
    size_t tik_len = 0, cert_len = 0;

    HttpResult hres = resolved_url_get_range(ru, pfs0_entry_file_offset(pfs0, tik_index), tik_size,
                                              &tik_buf, &tik_len, err_buf, err_buf_size);
    if (hres != HTTP_OK) return false;

    hres = resolved_url_get_range(ru, pfs0_entry_file_offset(pfs0, cert_index), cert_size,
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

static NspInstallResult install_from_url(ResolvedUrl *ru, const Pfs0 *pfs0,
                                          InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                          void *userdata,
                                          char *err_buf, size_t err_buf_size) {
    NspInstallResult result = NSP_INSTALL_OK;

    Result rc = es_initialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio es (0x%x)", rc);
        return NSP_INSTALL_ERR_NCM;
    }

    rc = ns_record_initialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio ns (0x%x)", rc);
        es_exit();
        return NSP_INSTALL_ERR_NCM;
    }

    rc = ncmInitialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio ncm (0x%x)", rc);
        ns_record_exit();
        es_exit();
        return NSP_INSTALL_ERR_NCM;
    }

    NcmContentStorage cs;
    rc = ncmOpenContentStorage(&cs, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmOpenContentStorage falló (0x%x)", rc);
        ncmExit();
        ns_record_exit();
        es_exit();
        return NSP_INSTALL_ERR_NCM;
    }

    NcmContentMetaDatabase db;
    rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmOpenContentMetaDatabase falló (0x%x)", rc);
        ncmContentStorageClose(&cs);
        ncmExit();
        ns_record_exit();
        es_exit();
        return NSP_INSTALL_ERR_NCM;
    }

    int cnmt_index = pfs0_find_by_suffix(pfs0, ".cnmt.nca", 0);
    if (cnmt_index < 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "el NSP no contiene ningún .cnmt.nca");
        result = NSP_INSTALL_ERR_PARSE;
    }

    if (result == NSP_INSTALL_OK && phase_cb) phase_cb(INSTALL_PHASE_INSTALLING, userdata);

    while (cnmt_index >= 0 && result == NSP_INSTALL_OK) {
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
        if (!ncm_parse_content_id(pfs0->names[cnmt_index], &cnmt_id)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "nombre de archivo cnmt.nca inválido: %s", pfs0->names[cnmt_index]);
            result = NSP_INSTALL_ERR_PARSE;
            break;
        }

        uint64_t cnmt_offset = pfs0_entry_file_offset(pfs0, cnmt_index);
        uint64_t cnmt_size = pfs0->entries[cnmt_index].file_size;

        bool cnmt_fresh = false;
        if (!ncm_install_content_from_url(&cs, &cnmt_id, ru, cnmt_offset, cnmt_size, cb, userdata,
                                           &cnmt_fresh, err_buf, err_buf_size)) {
            // ncm_install_content_from_url's own err_buf message distinguishes a
            // cancel from a real failure ("instalación cancelada" vs. an
            // ncm*/red failure) - map the former to NSP_INSTALL_ERR_CANCELED.
            result = (err_buf && strstr(err_buf, "cancel")) ? NSP_INSTALL_ERR_CANCELED : NSP_INSTALL_ERR_NCM;
            break; // nothing registered yet (a failed call cleans up its own placeholder) - no rollback needed
        }
        if (cnmt_fresh) registered_ids[registered_count++] = cnmt_id;

        ContentMetaInfo meta;
        if (!ncm_read_content_meta(&cs, &cnmt_id, &meta, err_buf, err_buf_size)) {
            result = NSP_INSTALL_ERR_NCM;
            rollback_registered(&cs, registered_ids, registered_count);
            break;
        }

        bool content_ok = true;
        for (int i = 0; i < meta.content_info_count && content_ok; i++) {
            char hex[33];
            ncm_format_content_id(&meta.content_infos[i].content_id, hex);
            char nca_filename[40];
            snprintf(nca_filename, sizeof(nca_filename), "%s.nca", hex);

            int nca_index = -1;
            for (int j = 0; j < pfs0->count; j++) {
                if (strcmp(pfs0->names[j], nca_filename) == 0) { nca_index = j; break; }
            }
            if (nca_index < 0) {
                if (err_buf) snprintf(err_buf, err_buf_size, "el NSP no incluye %s (referenciado por su .cnmt)", nca_filename);
                result = NSP_INSTALL_ERR_PARSE;
                content_ok = false;
                break;
            }

            uint64_t nca_offset = pfs0_entry_file_offset(pfs0, nca_index);
            uint64_t nca_size = pfs0->entries[nca_index].file_size;
            bool nca_fresh = false;
            if (!ncm_install_content_from_url(&cs, &meta.content_infos[i].content_id, ru, nca_offset, nca_size,
                                               cb, userdata, &nca_fresh, err_buf, err_buf_size)) {
                result = (err_buf && strstr(err_buf, "cancel")) ? NSP_INSTALL_ERR_CANCELED : NSP_INSTALL_ERR_NCM;
                content_ok = false;
                break;
            }
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
            result = NSP_INSTALL_ERR_NCM;
            rollback_registered(&cs, registered_ids, registered_count);
            break;
        }

        NsContentStorageRecord storage_record;
        storage_record.meta_record = meta.key;
        storage_record.storage_id = NcmStorageId_SdCard;

        rc = ns_push_application_record(application_id_for_meta(&meta.key), NsRecordType_Installed,
                                         &storage_record, 1);
        if (R_FAILED(rc)) {
            // Content + meta are already committed at this point - the title
            // is genuinely installed, it just won't show on hbmenu/home menu
            // yet. Surfaced as its own distinct error so this doesn't read
            // like a full install failure - and specifically NOT rolled
            // back, unlike every break above: the content is real now.
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "el contenido se instaló, pero no se pudo registrar en el menú (0x%x)", rc);
            result = NSP_INSTALL_ERR_RECORD;
            break;
        }

        cnmt_index = pfs0_find_by_suffix(pfs0, ".cnmt.nca", cnmt_index + 1);
    }

    if (result == NSP_INSTALL_OK) {
        int tik_indices[MAX_TICKET_PAIRS];
        int cert_indices[MAX_TICKET_PAIRS];
        int tik_count = 0, cert_count = 0;

        int idx = pfs0_find_by_suffix(pfs0, ".tik", 0);
        while (idx >= 0 && tik_count < MAX_TICKET_PAIRS) {
            tik_indices[tik_count++] = idx;
            idx = pfs0_find_by_suffix(pfs0, ".tik", idx + 1);
        }
        idx = pfs0_find_by_suffix(pfs0, ".cert", 0);
        while (idx >= 0 && cert_count < MAX_TICKET_PAIRS) {
            cert_indices[cert_count++] = idx;
            idx = pfs0_find_by_suffix(pfs0, ".cert", idx + 1);
        }

        // No ticket at all is fine (pre-installed/"standard crypto" content) -
        // only a mismatched count is treated as an error.
        if (tik_count != cert_count) {
            if (err_buf) snprintf(err_buf, err_buf_size, "el NSP tiene un número distinto de archivos .tik y .cert");
            result = NSP_INSTALL_ERR_TICKET;
        }

        for (int i = 0; i < tik_count && result == NSP_INSTALL_OK; i++) {
            if (!import_ticket_from_url(ru, pfs0, tik_indices[i], cert_indices[i], err_buf, err_buf_size)) {
                result = NSP_INSTALL_ERR_TICKET;
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

NspInstallResult install_nsp_native(const AppEntry *entry, const char *base_url,
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
            return NSP_INSTALL_ERR_NO_SPACE;
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

    // Fetch just the PFS0 header/table (a bounded, near-instant request) -
    // every content install below streams straight from `ru` into NCM, so
    // the (potentially multi-GB) NSP itself is never downloaded as a whole
    // file. This is what avoids FAT32's 4GB-per-file limit: NCM already
    // writes each NCA as its own placeholder file, so as long as no single
    // NCA is itself bigger than 4GB (true for essentially every catalog
    // title), nothing this installer writes to the SD ever crosses that
    // limit, regardless of how big the NSP as a whole is.
    char *hdr_buf = NULL;
    size_t hdr_len = 0;
    HttpResult hres = resolved_url_get_range(&ru, 0, PFS0_HEADER_PREFETCH_BYTES, &hdr_buf, &hdr_len, err_buf, err_buf_size);
    if (hres != HTTP_OK) {
        return NSP_INSTALL_ERR_DOWNLOAD;
    }

    char hdr_path[512];
    snprintf(hdr_path, sizeof(hdr_path), "%s/%s.hdr", NSP_REPO_DIR_PATH, entry->filename);

    FILE *hdr_fp = fopen(hdr_path, "wb");
    if (!hdr_fp) {
        free(hdr_buf);
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo escribir el encabezado temporal");
        return NSP_INSTALL_ERR_DOWNLOAD;
    }
    size_t hdr_written = fwrite(hdr_buf, 1, hdr_len, hdr_fp);
    fclose(hdr_fp);
    free(hdr_buf);
    if (hdr_written != hdr_len) {
        remove(hdr_path);
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo escribir el encabezado temporal");
        return NSP_INSTALL_ERR_DOWNLOAD;
    }

    Pfs0 pfs0;
    int prc = pfs0_open(hdr_path, &pfs0);
    remove(hdr_path);
    if (prc != 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "el archivo no es un NSP (PFS0) válido");
        return NSP_INSTALL_ERR_PARSE;
    }

    return install_from_url(&ru, &pfs0, cb, phase_cb, userdata, err_buf, err_buf_size);
}
