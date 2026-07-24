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

// Installs entry->filename (an NSP at "<base_url><entry->download_url>")
// directly into the console's own title database - no DBI hand-off, and no
// intermediate download either. Fetches just the PFS0 header/table over the
// network (a small, bounded request), then streams every NCA it references
// straight from the network into NcmContentStorage (SD card) via
// ncm_install_content_from_url, commits the content-meta record, imports
// the ticket/cert if present, and pushes the application record so the
// title shows on hbmenu/home menu. The NSP itself is never written to the
// SD as a single file, which is what lets this install files bigger than
// 4GB even on a FAT32-formatted card - see ncm_install_content_from_url's
// doc comment for why.
//
// Because there's no longer a downloaded copy of the NSP sitting on the SD
// card, a failure here does NOT leave anything behind for "Instalar con
// DBI" (see install_nsp_and_launch_dbi) to pick up - that fallback does its
// own full download from scratch if used after this fails.
//
// `phase_cb` (may be NULL) is called once, right after the header is parsed
// and before any content install begins, so the caller can switch its
// progress label from "downloading" to "installing" - see
// InstallPhaseCallback. Note that unlike the old file-then-install flow,
// "installing" here also covers the network transfer itself (there's no
// separate whole-file download phase anymore).
//
// Only handles entry->file_type == APP_FILE_TYPE_NSP - see install_nsp.h for
// XCI, which still goes through DBI.
NspInstallResult install_nsp_native(const AppEntry *entry, const char *base_url,
                                     InstallProgressCallback cb, InstallPhaseCallback phase_cb, void *userdata,
                                     char *err_buf, size_t err_buf_size);
