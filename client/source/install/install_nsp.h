#pragma once
#include "install.h"
#include "../catalog/app_entry.h"

typedef enum {
    // Downloaded + verified, DBI chain-load armed - caller must exit its
    // main loop promptly (see below) so hbloader performs the chain-load.
    NSP_HANDOFF_OK = 0,
    NSP_HANDOFF_ERR_NO_SPACE,
    NSP_HANDOFF_ERR_DOWNLOAD,
    NSP_HANDOFF_ERR_HASH_MISMATCH,
    NSP_HANDOFF_ERR_RENAME,
    // Download+verify succeeded (file is on the SD card, ready to install)
    // but sdmc:/switch/DBI/dbi.nro wasn't found.
    NSP_HANDOFF_ERR_NO_DBI,
    NSP_HANDOFF_ERR_CANCELED, // user canceled - not a real failure, don't alarm them.
} NspHandoffResult;

// This project doesn't reimplement NSP/XCI title installation (NCM/ES
// services, ticket handling) - too easy to get wrong in a way that corrupts
// a NAND/SD install. Instead: downloads entry's .nsp or .xci from
// "<base_url><entry->download_url>" to sdmc:/switch/DBI/nsp-repo/<entry->filename>
// (same folder for both - DBI installs either format from there), verifies
// entry->sha256 if present, then arms a chain-load into
// sdmc:/switch/DBI/dbi.nro via envSetNextLoad.
//
// On NSP_HANDOFF_OK the caller MUST exit its main loop and let main() return
// (after its own cleanup, e.g. ui_app_shutdown()) promptly - the chain-load
// only takes effect once this process exits normally. Nothing is actually
// installed yet: the user still has to pick the file in DBI
// (Browse SD Card > DBI > nsp-repo) and confirm the install there.
NspHandoffResult install_nsp_and_launch_dbi(const AppEntry *entry, const char *base_url,
                                             InstallProgressCallback cb, void *userdata,
                                             char *err_buf, size_t err_buf_size);
