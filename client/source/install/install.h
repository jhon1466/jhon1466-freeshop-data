#pragma once
#include "../catalog/app_entry.h"
#include <stdbool.h>

typedef enum {
    INSTALL_OK = 0,
    INSTALL_ERR_NO_SPACE,
    INSTALL_ERR_MKDIR,
    INSTALL_ERR_DOWNLOAD,
    INSTALL_ERR_HASH_MISMATCH,
    INSTALL_ERR_RENAME,
    INSTALL_ERR_CANCELED, // user canceled - not a real failure, don't alarm them.
} InstallResult;

// Called periodically with total/downloaded byte counts so the caller can
// drive a progress bar. Return false to cancel the install (e.g. the user
// pressed a cancel button) - true to keep going. May be NULL.
typedef bool (*InstallProgressCallback)(long total, long now, void *userdata);

typedef enum {
    INSTALL_PHASE_DOWNLOADING, // fetching the file over the network
    INSTALL_PHASE_INSTALLING,  // writing already-downloaded content into the console's own storage (NCM)
} InstallPhase;

// Called once when a multi-phase install (download, then write content to
// the console's storage) switches phase, so the caller can update its UI
// label (e.g. "Descargando..." -> "Instalando..."). May be NULL. Installers
// that are pure downloads (install_app, install_nsp_and_launch_dbi) never
// call this - their whole visible progress is the download itself.
typedef void (*InstallPhaseCallback)(InstallPhase phase, void *userdata);

// Downloads entry's .nro from "<base_url><entry->download_url>" to
// sdmc:/switch/<entry->id>/<entry->filename>, verifying entry->sha256
// before the file is moved into its final location. Never leaves a
// partially-written or corrupt file at the final path. Only handles
// entry->file_type == APP_FILE_TYPE_NRO - see install_nsp.h for NSP entries.
InstallResult install_app(const AppEntry *entry, const char *base_url,
                           InstallProgressCallback cb, void *userdata,
                           char *err_buf, size_t err_buf_size);
