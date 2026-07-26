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

// Installs entry->filename (an XCI at "<base_url><entry->download_url>")
// directly into the console's own title database - no DBI hand-off, and no
// intermediate download either. Locates the XCI's "secure" partition over
// the network (three small, bounded Range requests: the gamecard header to
// find where the root partition table actually is, that root table itself,
// then the "secure" entry's own nested header - see
// xci_container.h/install_xci_native.c's xci_open_secure_partition_from_url),
// then does exactly what
// install_nsp_native does from there: streams every referenced NCA straight
// from the network into NcmContentStorage via ncm_install_content_from_url,
// commits the content-meta record, imports the ticket/cert if present, and
// pushes the application record. The XCI itself is never written to the SD
// as a single file, which is what lets this install files bigger than 4GB
// even on a FAT32-formatted card.
//
// Because there's no longer a downloaded copy of the XCI sitting on the SD
// card, a failure here does NOT leave anything behind for "Instalar vía
// DBI" (see install_nsp_and_launch_dbi - it works for any file type despite
// the name) to pick up - that fallback does its own full download from
// scratch if used after this fails.
//
// `phase_cb` (may be NULL) is called once, right after the secure
// partition's header is parsed and before any content install begins - see
// InstallPhaseCallback. Note that unlike the old file-then-install flow,
// "installing" here also covers the network transfer itself.
//
// Only handles entry->file_type == APP_FILE_TYPE_XCI.
XciInstallResult install_xci_native(const AppEntry *entry, const char *base_url,
                                     InstallProgressCallback cb, InstallPhaseCallback phase_cb, void *userdata,
                                     char *err_buf, size_t err_buf_size);
