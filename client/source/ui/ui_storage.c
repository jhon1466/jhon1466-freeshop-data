#include "ui_storage.h"

#include <stdio.h>

void ui_storage_refresh(StorageInfo *out) {
    out->sd_total = 0;
    out->sd_free = 0;
    out->nand_total = 0;
    out->nand_free = 0;
    out->sd_ok = false;
    out->nand_ok = false;

    // SD card: ns is documented as SD-card-only for these two calls.
    Result rc = nsInitialize();
    if (R_SUCCEEDED(rc)) {
        Result rc_total = nsGetTotalSpaceSize(NcmStorageId_SdCard, &out->sd_total);
        Result rc_free = nsGetFreeSpaceSize(NcmStorageId_SdCard, &out->sd_free);
        out->sd_ok = R_SUCCEEDED(rc_total) && R_SUCCEEDED(rc_free);
        nsExit();
    }

    // NAND (the "User" BIS partition - what Tinfoil/DBI show as NAND):
    // ns's space calls don't support this, so go through fs's BIS API.
    FsFileSystem bis_fs;
    rc = fsOpenBisFileSystem(&bis_fs, FsBisPartitionId_User, "");
    if (R_SUCCEEDED(rc)) {
        Result rc_total = fsFsGetTotalSpace(&bis_fs, "/", &out->nand_total);
        Result rc_free = fsFsGetFreeSpace(&bis_fs, "/", &out->nand_free);
        out->nand_ok = R_SUCCEEDED(rc_total) && R_SUCCEEDED(rc_free);
        fsFsClose(&bis_fs);
    }
}

void format_bytes(s64 bytes, char *out, size_t out_size) {
    double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
    snprintf(out, out_size, "%.1f GB", gb);
}
