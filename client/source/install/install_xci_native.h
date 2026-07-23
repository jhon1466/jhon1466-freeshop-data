#pragma once
#include "install.h"
#include "../catalog/app_entry.h"

typedef enum {
    XCI_INSTALL_OK = 0,
    XCI_INSTALL_ERR_NO_SPACE,
    XCI_INSTALL_ERR_DOWNLOAD,
    XCI_INSTALL_ERR_HASH_MISMATCH,
    XCI_INSTALL_ERR_PARSE,   // not a valid/supported XCI (root/secure HFS0), or an unexpected CNMT layout
    XCI_INSTALL_ERR_NCM,     // any ncm*/fs* call failed while writing content or committing the meta record
    XCI_INSTALL_ERR_TICKET,  // es_import_ticket failed
    XCI_INSTALL_ERR_RECORD,  // ns_push_application_record failed (content is installed, but won't show on hbmenu)
    XCI_INSTALL_ERR_CANCELED,
} XciInstallResult;

// Installs entry->filename (an XCI downloaded from
// "<base_url><entry->download_url>") directly into the console's own title
// database - no DBI hand-off. Locates the XCI's "secure" partition (see
// xci_container.h), then does exactly what install_nsp_native does from
// there: streams every referenced NCA into NcmContentStorage, commits the
// content-meta record, imports the ticket/cert if present, and pushes the
// application record.
//
// Downloads to the same sdmc:/switch/DBI/nsp-repo/ folder DBI itself reads
// from - on failure, the file is left there so "Instalar vía DBI" can be
// used as an immediate fallback. On success the file is deleted to reclaim
// SD space.
//
// `phase_cb` (may be NULL) is called once, right before content starts
// being written to NCM content storage - see InstallPhaseCallback.
//
// Only handles entry->file_type == APP_FILE_TYPE_XCI.
XciInstallResult install_xci_native(const AppEntry *entry, const char *base_url,
                                     InstallProgressCallback cb, InstallPhaseCallback phase_cb, void *userdata,
                                     char *err_buf, size_t err_buf_size);
