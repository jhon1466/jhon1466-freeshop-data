#pragma once
#include <switch.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Content registered in NcmContentStorage (SD card) but not referenced by
// any committed content-meta record in NcmContentMetaDatabase - i.e. an
// install that registered NCAs and then failed/was interrupted before ever
// finishing. Installs from before today's automatic rollback (see
// install_nsp_native.c/install_xci_native.c/install_local.c's
// rollback_registered) could leave exactly this behind; new installs clean
// up after themselves now, so this is specifically for pre-existing damage.
// Silently consumes SD space forever with no way to see or reclaim it
// otherwise - nothing references it, so it never shows on hbmenu either.
#define NCM_CLEANUP_MAX_ORPHANS 1024

typedef struct {
    NcmContentId ids[NCM_CLEANUP_MAX_ORPHANS];
    int count;
    int64_t total_bytes;
    bool truncated; // more orphans exist than NCM_CLEANUP_MAX_ORPHANS could record
} NcmOrphanScanResult;

// Scans NcmStorageId_SdCard's content storage against its content-meta
// database and fills `out` with every content_id present in the former but
// not referenced by anything in the latter. Read-only - deletes nothing.
// Returns false only on an NCM service error (err_buf filled); finding zero
// orphans is a normal, successful result (out->count == 0).
bool ncm_cleanup_scan_orphans(NcmOrphanScanResult *out, char *err_buf, size_t err_buf_size);

// Deletes every content_id in `ids` from NcmStorageId_SdCard's content
// storage. Only ever call this with ids that came from
// ncm_cleanup_scan_orphans, from a scan done immediately before (nothing
// installed/uninstalled in between) - deleting anything else risks removing
// content a real, working title still depends on.
void ncm_cleanup_delete_orphans(const NcmContentId *ids, int count);
