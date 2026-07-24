#pragma once
#include "install.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    INSTALL_LOCAL_OK = 0,
    INSTALL_LOCAL_ERR_PARSE,    // not a valid/supported NSP (PFS0) or XCI, or an unexpected CNMT layout
    INSTALL_LOCAL_ERR_NCM,      // any ncm*/fs* call failed while writing content or committing the meta record
    INSTALL_LOCAL_ERR_TICKET,   // es_import_ticket failed
    INSTALL_LOCAL_ERR_RECORD,   // ns_push_application_record failed (content is installed, but won't show on hbmenu)
    INSTALL_LOCAL_ERR_CANCELED,
} InstallLocalResult;

// Installs an NSP/XCI already sitting on the SD card at `path` directly into
// the console's own title database - the local-file counterpart of
// install_nsp_native.c/install_xci_native.c's network installers (same NCM
// writes, same rollback-on-failure behavior), for files the user placed on
// the SD themselves (via USB, backed up from elsewhere, or left over from a
// canceled catalog download) rather than fetched from a catalog entry. Used
// by the SD card file explorer (ui_explorer.c) - no DBI hand-off, no
// network involved at all.
InstallLocalResult install_nsp_from_local_file(const char *path,
                                                InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                                void *userdata, char *err_buf, size_t err_buf_size);
InstallLocalResult install_xci_from_local_file(const char *path,
                                                InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                                void *userdata, char *err_buf, size_t err_buf_size);
