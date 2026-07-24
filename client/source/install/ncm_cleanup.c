#include "ncm_cleanup.h"

#include <stdio.h>
#include <string.h>

// Installed titles/updates/DLCs count as content-meta records - 256 is
// generous headroom for even a well-stocked homebrew library.
#define MAX_META_KEYS 256
// Total NCAs referenced across every content-meta record (a handful per
// title/update/DLC, typically) and total NCAs actually sitting in content
// storage - both capped the same generous way.
#define MAX_REFERENCED_IDS 4096
#define MAX_STORAGE_IDS 4096
#define MAX_CONTENT_INFOS_PER_KEY 64

static bool content_id_in_list(const NcmContentId *id, const NcmContentId *list, int count) {
    for (int i = 0; i < count; i++) {
        if (memcmp(id->c, list[i].c, sizeof(id->c)) == 0) return true;
    }
    return false;
}

bool ncm_cleanup_scan_orphans(NcmOrphanScanResult *out, char *err_buf, size_t err_buf_size) {
    memset(out, 0, sizeof(*out));

    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo conectar con el servicio ncm (0x%x)", rc);
        return false;
    }

    NcmContentStorage cs;
    rc = ncmOpenContentStorage(&cs, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmOpenContentStorage falló (0x%x)", rc);
        ncmExit();
        return false;
    }

    NcmContentMetaDatabase db;
    rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmOpenContentMetaDatabase falló (0x%x)", rc);
        ncmContentStorageClose(&cs);
        ncmExit();
        return false;
    }

    // ---- Step 1: every content_id referenced by any committed content-meta
    // record (the meta/cnmt content itself, plus every NCA it lists) - this
    // is "everything a real, working install still needs". ----
    static NcmContentMetaKey meta_keys[MAX_META_KEYS];
    s32 total_keys = 0, written_keys = 0;
    rc = ncmContentMetaDatabaseList(&db, &total_keys, &written_keys, meta_keys, MAX_META_KEYS,
                                     NcmContentMetaType_Unknown, 0, 0, UINT64_MAX, NcmContentInstallType_Full);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentMetaDatabaseList falló (0x%x)", rc);
        ncmContentMetaDatabaseClose(&db);
        ncmContentStorageClose(&cs);
        ncmExit();
        return false;
    }

    static NcmContentId referenced_ids[MAX_REFERENCED_IDS];
    int referenced_count = 0;

    static NcmContentInfo infos[MAX_CONTENT_INFOS_PER_KEY];
    for (s32 k = 0; k < written_keys; k++) {
        // ListContentInfo below only returns the content a meta *references*
        // (its NCAs) - the meta/cnmt content itself isn't in that list, so
        // it has to be fetched separately or it'd look orphaned too.
        NcmContentId meta_content_id;
        Result meta_rc = ncmContentMetaDatabaseGetContentIdByType(&db, &meta_content_id, &meta_keys[k],
                                                                    NcmContentType_Meta);
        if (R_SUCCEEDED(meta_rc) && referenced_count < MAX_REFERENCED_IDS) {
            referenced_ids[referenced_count++] = meta_content_id;
        }

        s32 info_written = 0;
        Result info_rc = ncmContentMetaDatabaseListContentInfo(&db, &info_written, infos,
                                                                 MAX_CONTENT_INFOS_PER_KEY, &meta_keys[k], 0);
        if (R_FAILED(info_rc)) continue; // best-effort - skip a key we can't read rather than aborting the scan

        for (s32 i = 0; i < info_written && referenced_count < MAX_REFERENCED_IDS; i++) {
            referenced_ids[referenced_count++] = infos[i].content_id;
        }
    }

    // ---- Step 2: every content_id actually sitting in storage. ----
    static NcmContentId storage_ids[MAX_STORAGE_IDS];
    s32 storage_count = 0;
    rc = ncmContentStorageListContentId(&cs, storage_ids, MAX_STORAGE_IDS, &storage_count, 0);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "ncmContentStorageListContentId falló (0x%x)", rc);
        ncmContentMetaDatabaseClose(&db);
        ncmContentStorageClose(&cs);
        ncmExit();
        return false;
    }

    // ---- Step 3: anything in storage but not referenced is orphaned. ----
    for (s32 i = 0; i < storage_count; i++) {
        if (content_id_in_list(&storage_ids[i], referenced_ids, referenced_count)) continue;

        if (out->count >= NCM_CLEANUP_MAX_ORPHANS) {
            out->truncated = true;
            break;
        }

        s64 size = 0;
        ncmContentStorageGetSizeFromContentId(&cs, &size, &storage_ids[i]);

        out->ids[out->count++] = storage_ids[i];
        out->total_bytes += size;
    }

    ncmContentMetaDatabaseClose(&db);
    ncmContentStorageClose(&cs);
    ncmExit();

    return true;
}

void ncm_cleanup_delete_orphans(const NcmContentId *ids, int count) {
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) return;

    NcmContentStorage cs;
    rc = ncmOpenContentStorage(&cs, NcmStorageId_SdCard);
    if (R_FAILED(rc)) {
        ncmExit();
        return;
    }

    for (int i = 0; i < count; i++) {
        ncmContentStorageDelete(&cs, &ids[i]);
    }

    ncmContentStorageClose(&cs);
    ncmExit();
}
