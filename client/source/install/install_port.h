#pragma once
#include "install.h"
#include "../catalog/app_entry.h"

typedef enum {
    PORT_INSTALL_OK = 0,
    PORT_INSTALL_ERR_NO_SPACE,
    PORT_INSTALL_ERR_DOWNLOAD,
    PORT_INSTALL_ERR_HASH_MISMATCH,
    PORT_INSTALL_ERR_EXTRACT,
    PORT_INSTALL_ERR_CANCELED,
} PortInstallResult;

// Downloads entry's .zip from "<base_url><entry->download_url>" into
// sdmc:/switch/<entry->id>/<entry->filename>, verifies entry->sha256 if
// present, then extracts it in place - recreating whatever folder structure
// it has (the .nro plus its data files/subfolders, e.g. a native port's
// asset pack) - via zip_extract_to_dir, and deletes the .zip afterward.
//
// `phase_cb` (may be NULL) is called once, right before extraction starts,
// so the caller can switch its progress label from "downloading" to
// "installing" - see InstallPhaseCallback.
//
// Only handles entry->file_type == APP_FILE_TYPE_PORT.
PortInstallResult install_port(const AppEntry *entry, const char *base_url,
                                InstallProgressCallback cb, InstallPhaseCallback phase_cb, void *userdata,
                                char *err_buf, size_t err_buf_size);
