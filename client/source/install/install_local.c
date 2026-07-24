#include "install_local.h"
#include "pfs0.h"
#include "hfs0.h"
#include "xci_container.h"
#include "ncm_install.h"
#include "es_ticket.h"
#include "ns_record.h"

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Up to 4 ticket/cert pairs - matches install_nsp_native.c/install_xci_native.c.
#define MAX_TICKET_PAIRS 4

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
// NCAs/cnmt/tik/cert) from an already-open local file: parses the root
// partition table at the fixed XCI_ROOT_HFS0_OFFSET, finds "secure" within
// it, then parses that partition's own nested header. Returns 0 on success,
// -1 on I/O error, -2 if the file isn't a valid XCI or has no "secure"
// partition - same contract hfs0_parse_at() itself uses.
static int xci_open_secure_partition_local(FILE *fp, Hfs0 *out) {
    Hfs0 root;
    int rc = hfs0_parse_at(fp, XCI_ROOT_HFS0_OFFSET, &root);
    if (rc != 0) return rc;

    int secure_index = hfs0_find_by_name(&root, "secure");
    if (secure_index < 0) return -2;

    uint64_t secure_offset = hfs0_entry_file_offset(&root, secure_index);
    return hfs0_parse_at(fp, secure_offset, out);
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

InstallLocalResult install_nsp_from_local_file(const char *path,
                                                InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                                void *userdata, char *err_buf, size_t err_buf_size) {
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

    int cnmt_index = pfs0_find_by_suffix(&pfs0, ".cnmt.nca", 0);
    if (cnmt_index < 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "el NSP no contiene ningún .cnmt.nca");
        result = INSTALL_LOCAL_ERR_PARSE;
    }

    if (result == INSTALL_LOCAL_OK && phase_cb) phase_cb(INSTALL_PHASE_INSTALLING, userdata);

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

        bool cnmt_fresh = false;
        if (!ncm_install_content(&cs, &cnmt_id, src, cnmt_offset, cnmt_size, cb, userdata, &cnmt_fresh, err_buf, err_buf_size)) {
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

        bool content_ok = true;
        for (int i = 0; i < meta.content_info_count && content_ok; i++) {
            char hex[33];
            ncm_format_content_id(&meta.content_infos[i].content_id, hex);
            char nca_filename[40];
            snprintf(nca_filename, sizeof(nca_filename), "%s.nca", hex);

            int nca_index = -1;
            for (int j = 0; j < pfs0.count; j++) {
                if (strcmp(pfs0.names[j], nca_filename) == 0) { nca_index = j; break; }
            }
            if (nca_index < 0) {
                if (err_buf) snprintf(err_buf, err_buf_size, "el NSP no incluye %s (referenciado por su .cnmt)", nca_filename);
                result = INSTALL_LOCAL_ERR_PARSE;
                content_ok = false;
                break;
            }

            uint64_t nca_offset = pfs0_entry_file_offset(&pfs0, nca_index);
            uint64_t nca_size = pfs0.entries[nca_index].file_size;
            bool nca_fresh = false;
            if (!ncm_install_content(&cs, &meta.content_infos[i].content_id, src, nca_offset, nca_size,
                                      cb, userdata, &nca_fresh, err_buf, err_buf_size)) {
                result = (err_buf && strstr(err_buf, "cancel")) ? INSTALL_LOCAL_ERR_CANCELED : INSTALL_LOCAL_ERR_NCM;
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

InstallLocalResult install_xci_from_local_file(const char *path,
                                                InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                                void *userdata, char *err_buf, size_t err_buf_size) {
    FILE *src = fopen(path, "rb");
    if (!src) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo abrir el archivo");
        return INSTALL_LOCAL_ERR_PARSE;
    }

    Hfs0 hfs0;
    if (xci_open_secure_partition_local(src, &hfs0) != 0) {
        fclose(src);
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

    int cnmt_index = hfs0_find_by_suffix(&hfs0, ".cnmt.nca", 0);
    if (cnmt_index < 0) {
        if (err_buf) snprintf(err_buf, err_buf_size, "la partición 'secure' no contiene ningún .cnmt.nca");
        result = INSTALL_LOCAL_ERR_PARSE;
    }

    if (result == INSTALL_LOCAL_OK && phase_cb) phase_cb(INSTALL_PHASE_INSTALLING, userdata);

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

        bool cnmt_fresh = false;
        if (!ncm_install_content(&cs, &cnmt_id, src, cnmt_offset, cnmt_size, cb, userdata, &cnmt_fresh, err_buf, err_buf_size)) {
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

        bool content_ok = true;
        for (int i = 0; i < meta.content_info_count && content_ok; i++) {
            char hex[33];
            ncm_format_content_id(&meta.content_infos[i].content_id, hex);
            char nca_filename[40];
            snprintf(nca_filename, sizeof(nca_filename), "%s.nca", hex);

            int nca_index = hfs0_find_by_name(&hfs0, nca_filename);
            if (nca_index < 0) {
                if (err_buf) snprintf(err_buf, err_buf_size, "el XCI no incluye %s (referenciado por su .cnmt)", nca_filename);
                result = INSTALL_LOCAL_ERR_PARSE;
                content_ok = false;
                break;
            }

            uint64_t nca_offset = hfs0_entry_file_offset(&hfs0, nca_index);
            uint64_t nca_size = hfs0.entries[nca_index].file_size;
            bool nca_fresh = false;
            if (!ncm_install_content(&cs, &meta.content_infos[i].content_id, src, nca_offset, nca_size,
                                      cb, userdata, &nca_fresh, err_buf, err_buf_size)) {
                result = (err_buf && strstr(err_buf, "cancel")) ? INSTALL_LOCAL_ERR_CANCELED : INSTALL_LOCAL_ERR_NCM;
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
