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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

// Shares DBI's own folder on purpose, same as install_nsp_native.c - see
// install_xci_native.h's doc comment on the failure-fallback behavior.
#define DBI_DIR_PATH SWITCH_APPS_ROOT "/DBI"
#define NSP_REPO_DIR_PATH SWITCH_APPS_ROOT "/DBI/nsp-repo"

// Up to 4 ticket/cert pairs - generous headroom, matching install_nsp_native.c.
#define MAX_TICKET_PAIRS 4

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

static XciInstallResult install_from_local_file(const char *xci_path,
                                                  InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                                  void *userdata,
                                                  char *err_buf, size_t err_buf_size) {
    Hfs0 hfs0;
    int prc = xci_open_secure_partition(xci_path, &hfs0);
    if (prc != 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "el archivo no es un XCI válido (partición 'secure' no encontrada)");
        return XCI_INSTALL_ERR_PARSE;
    }

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

    FILE *src = fopen(xci_path, "rb");
    if (!src) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo reabrir el archivo descargado");
        ncmContentMetaDatabaseClose(&db);
        ncmContentStorageClose(&cs);
        ncmExit();
        ns_record_exit();
        es_exit();
        return XCI_INSTALL_ERR_PARSE;
    }

    int cnmt_index = hfs0_find_by_suffix(&hfs0, ".cnmt.nca", 0);
    if (cnmt_index < 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "la partición 'secure' no contiene ningún .cnmt.nca");
        result = XCI_INSTALL_ERR_PARSE;
    }

    if (result == XCI_INSTALL_OK && phase_cb) phase_cb(INSTALL_PHASE_INSTALLING, userdata);

    while (cnmt_index >= 0 && result == XCI_INSTALL_OK) {
        NcmContentId cnmt_id;
        if (!ncm_parse_content_id(hfs0.names[cnmt_index], &cnmt_id)) {
            if (err_buf) snprintf(err_buf, err_buf_size, "nombre de archivo cnmt.nca inválido: %s", hfs0.names[cnmt_index]);
            result = XCI_INSTALL_ERR_PARSE;
            break;
        }

        uint64_t cnmt_offset = hfs0_entry_file_offset(&hfs0, cnmt_index);
        uint64_t cnmt_size = hfs0.entries[cnmt_index].file_size;

        if (!ncm_install_content(&cs, &cnmt_id, src, cnmt_offset, cnmt_size, cb, userdata, err_buf, err_buf_size)) {
            result = (err_buf && strstr(err_buf, "cancel")) ? XCI_INSTALL_ERR_CANCELED : XCI_INSTALL_ERR_NCM;
            break;
        }

        ContentMetaInfo meta;
        if (!ncm_read_content_meta(&cs, &cnmt_id, &meta, err_buf, err_buf_size)) {
            result = XCI_INSTALL_ERR_NCM;
            break;
        }

        bool content_ok = true;
        for (int i = 0; i < meta.content_info_count && content_ok; i++) {
            char hex[33];
            ncm_format_content_id(&meta.content_infos[i].content_id, hex);
            char nca_filename[40];
            snprintf(nca_filename, sizeof(nca_filename), "%s.nca", hex);

            int nca_index = hfs0_find_by_name(&hfs0, nca_filename);
            if (nca_index < 0) {
                if (err_buf) snprintf(err_buf, err_buf_size, "el XCI no incluye %s (referenciado por su .cnmt)", nca_filename);
                result = XCI_INSTALL_ERR_PARSE;
                content_ok = false;
                break;
            }

            uint64_t nca_offset = hfs0_entry_file_offset(&hfs0, nca_index);
            uint64_t nca_size = hfs0.entries[nca_index].file_size;
            if (!ncm_install_content(&cs, &meta.content_infos[i].content_id, src, nca_offset, nca_size,
                                      cb, userdata, err_buf, err_buf_size)) {
                result = (err_buf && strstr(err_buf, "cancel")) ? XCI_INSTALL_ERR_CANCELED : XCI_INSTALL_ERR_NCM;
                content_ok = false;
                break;
            }
        }
        if (!content_ok) break;

        NcmContentInfo cnmt_content_info;
        memset(&cnmt_content_info, 0, sizeof(cnmt_content_info));
        cnmt_content_info.content_id = cnmt_id;
        ncmU64ToContentInfoSize(cnmt_size, &cnmt_content_info);
        cnmt_content_info.content_type = NcmContentType_Meta;

        if (!ncm_commit_content_meta(&db, &meta, &cnmt_content_info, err_buf, err_buf_size)) {
            result = XCI_INSTALL_ERR_NCM;
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
            result = XCI_INSTALL_ERR_RECORD;
            break;
        }

        cnmt_index = hfs0_find_by_suffix(&hfs0, ".cnmt.nca", cnmt_index + 1);
    }

    if (result == XCI_INSTALL_OK) {
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

        // No ticket at all is normal for a real game-card dump (retail XCI
        // content typically isn't rights-ID/ticket based) - only a
        // mismatched count is treated as an error.
        if (tik_count != cert_count) {
            if (err_buf) snprintf(err_buf, err_buf_size, "el XCI tiene un número distinto de archivos .tik y .cert");
            result = XCI_INSTALL_ERR_TICKET;
        }

        for (int i = 0; i < tik_count && result == XCI_INSTALL_OK; i++) {
            uint64_t tik_size = hfs0.entries[tik_indices[i]].file_size;
            uint64_t cert_size = hfs0.entries[cert_indices[i]].file_size;

            uint8_t *tik_buf = (uint8_t *)malloc((size_t)tik_size);
            uint8_t *cert_buf = (uint8_t *)malloc((size_t)cert_size);
            if (!tik_buf || !cert_buf) {
                free(tik_buf);
                free(cert_buf);
                if (err_buf) snprintf(err_buf, err_buf_size, "memoria insuficiente para el ticket");
                result = XCI_INSTALL_ERR_TICKET;
                break;
            }

            fseek(src, (long)hfs0_entry_file_offset(&hfs0, tik_indices[i]), SEEK_SET);
            size_t tik_read = fread(tik_buf, 1, (size_t)tik_size, src);
            fseek(src, (long)hfs0_entry_file_offset(&hfs0, cert_indices[i]), SEEK_SET);
            size_t cert_read = fread(cert_buf, 1, (size_t)cert_size, src);

            if (tik_read == (size_t)tik_size && cert_read == (size_t)cert_size) {
                rc = es_import_ticket(tik_buf, (size_t)tik_size, cert_buf, (size_t)cert_size);
            } else {
                rc = -1;
            }

            free(tik_buf);
            free(cert_buf);

            if (R_FAILED(rc)) {
                if (err_buf) snprintf(err_buf, err_buf_size, "esImportTicket falló (0x%x)", rc);
                result = XCI_INSTALL_ERR_TICKET;
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

    char part_path[512];
    snprintf(part_path, sizeof(part_path), "%s/%s.part", NSP_REPO_DIR_PATH, entry->filename);

    char url[900];
    install_common_resolve_url(base_url, entry->download_url, url, sizeof(url));

    InstallProgressThunkCtx thunk_ctx = { .cb = cb, .userdata = userdata };
    HttpResult hres = http_download_to_file(url, part_path, install_common_progress_thunk, &thunk_ctx,
                                             err_buf, err_buf_size);
    if (hres == HTTP_ERR_CANCELED) {
        return XCI_INSTALL_ERR_CANCELED;
    }
    if (hres != HTTP_OK) {
        return XCI_INSTALL_ERR_DOWNLOAD;
    }

    if (entry->sha256[0] != '\0') {
        char actual_hex[65];
        if (install_common_sha256_file(part_path, actual_hex) != 0) {
            remove(part_path);
            if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo leer el archivo descargado para verificar el checksum");
            return XCI_INSTALL_ERR_DOWNLOAD;
        }
        if (strcasecmp(actual_hex, entry->sha256) != 0) {
            remove(part_path);
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "el checksum no coincide (esperado %s, obtenido %s) - la descarga está corrupta",
                                   entry->sha256, actual_hex);
            return XCI_INSTALL_ERR_HASH_MISMATCH;
        }
    }

    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/%s", NSP_REPO_DIR_PATH, entry->filename);

    remove(final_path);
    if (rename(part_path, final_path) != 0) {
        if (install_common_copy_file(part_path, final_path) != 0) {
            remove(part_path);
            if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo mover el archivo descargado a su ubicación final");
            return XCI_INSTALL_ERR_DOWNLOAD;
        }
        remove(part_path);
    }

    // From here on, final_path is deliberately left in place on any failure -
    // see install_xci_native.h's doc comment on the DBI fallback.
    XciInstallResult result = install_from_local_file(final_path, cb, phase_cb, userdata, err_buf, err_buf_size);
    if (result == XCI_INSTALL_OK) {
        remove(final_path);
    }
    return result;
}
