#pragma once
#include "install.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    INSTALL_LOCAL_OK = 0,
    INSTALL_LOCAL_ERR_PARSE,    // not a valid/supported NSP (PFS0) or XCI, or an unexpected CNMT layout
    INSTALL_LOCAL_ERR_NCM,      // any ncm*/fs* call failed while writing content or committing the meta record
    INSTALL_LOCAL_ERR_TICKET,   // es_import_ticket failed
    INSTALL_LOCAL_ERR_RECORD,   // ns_push_application_record failed (content is installed, but won't show on hbmenu)
    INSTALL_LOCAL_ERR_CANCELED,
} InstallLocalResult;

// Lets a caller install from a file that is still being written, by
// gating every read on the bytes it needs actually being there yet.
//
// This is what makes a torrent install overlap its own download instead of
// waiting for it (see install_torrent.c): the torrent writes the container
// front-to-back (strict piece order) while the installer walks it in
// ascending file order, blocking in `ensure_range` whenever it gets ahead
// of the download. Nothing else about the install differs - it is the same
// NCM writes, the same rollback, reading from the same real, seekable file.
//
// Deliberately a "wait for this range" gate rather than install_stream.h's
// push model: that one requires strictly sequential delivery (it was built
// for MTP/FTP, where bytes arrive in wire order and can never be
// re-requested), and BitTorrent piece completion is only *roughly*
// ordered - within the strict-order lookahead window pieces still finish
// out of order. Reading from the file on disk sidesteps that entirely.
typedef struct {
    // Blocks until [offset, offset+len) of the file is readable, driving
    // whatever is still filling it. Returns false to give up (user
    // canceled, or the download failed) - the install is then rolled back
    // like any other mid-install failure.
    bool (*ensure_range)(void *user, uint64_t offset, uint64_t len);
    void *user;
} InstallLocalGate;

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

// Same, but for a file still being written - see InstallLocalGate. `gate`
// may be NULL, which is exactly what the two functions above pass.
InstallLocalResult install_nsp_from_local_file_ex(const char *path, const InstallLocalGate *gate,
                                                   InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                                   void *userdata, char *err_buf, size_t err_buf_size);
InstallLocalResult install_xci_from_local_file_ex(const char *path, const InstallLocalGate *gate,
                                                   InstallProgressCallback cb, InstallPhaseCallback phase_cb,
                                                   void *userdata, char *err_buf, size_t err_buf_size);
