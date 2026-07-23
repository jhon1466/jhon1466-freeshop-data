#pragma once
#include "install.h"
#include "../catalog/app_entry.h"

typedef enum {
    NSP_INSTALL_OK = 0,
    NSP_INSTALL_ERR_NO_SPACE,
    NSP_INSTALL_ERR_DOWNLOAD,
    NSP_INSTALL_ERR_HASH_MISMATCH,
    NSP_INSTALL_ERR_PARSE,   // not a valid/supported PFS0, or an unexpected CNMT layout
    NSP_INSTALL_ERR_NCM,     // any ncm*/fs* call failed while writing content or committing the meta record
    NSP_INSTALL_ERR_TICKET,  // es_import_ticket failed
    NSP_INSTALL_ERR_RECORD,  // ns_push_application_record failed (content is installed, but won't show on hbmenu)
    NSP_INSTALL_ERR_CANCELED,
} NspInstallResult;

// Installs entry->filename (an NSP downloaded from
// "<base_url><entry->download_url>") directly into the console's own title
// database - no DBI hand-off. Parses the PFS0 container, streams every NCA
// it references straight into NcmContentStorage (SD card), commits the
// content-meta record, imports the ticket/cert if present, and pushes the
// application record so the title shows on hbmenu/home menu.
//
// Downloads to the same sdmc:/switch/DBI/nsp-repo/ folder DBI itself reads
// from - on failure, the file is left there so "Instalar con DBI" (see
// install_nsp_and_launch_dbi) can be used as an immediate fallback without
// re-diagnosing anything. On success the file is deleted (it's now
// installed, DBI no longer needs it) to reclaim SD space.
//
// Only handles entry->file_type == APP_FILE_TYPE_NSP - see install_nsp.h for
// XCI, which still goes through DBI.
NspInstallResult install_nsp_native(const AppEntry *entry, const char *base_url,
                                     InstallProgressCallback cb, void *userdata,
                                     char *err_buf, size_t err_buf_size);
