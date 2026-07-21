#pragma once
#include "../catalog/app_entry.h"

typedef enum {
    INSTALL_OK = 0,
    INSTALL_ERR_NO_SPACE,
    INSTALL_ERR_MKDIR,
    INSTALL_ERR_DOWNLOAD,
    INSTALL_ERR_HASH_MISMATCH,
    INSTALL_ERR_RENAME,
} InstallResult;

typedef void (*InstallProgressCallback)(long total, long now, void *userdata);

// Downloads entry's .nro from "<base_url><entry->download_url>" to
// sdmc:/switch/<entry->id>/<entry->nro_filename>, verifying entry->sha256
// before the file is moved into its final location. Never leaves a
// partially-written or corrupt file at the final path.
InstallResult install_app(const AppEntry *entry, const char *base_url,
                           InstallProgressCallback cb, void *userdata,
                           char *err_buf, size_t err_buf_size);
