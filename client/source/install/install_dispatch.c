#include "install_dispatch.h"
#include "install_nsp_native.h"
#include "install_xci_native.h"
#include "install_port.h"
#include "install_torrent.h"

#include <stdio.h>

InstallOneResult install_one_entry(const AppEntry *entry,
                                   InstallProgressCallback cb, InstallPhaseCallback phase_cb, void *userdata,
                                   char *err_buf, size_t err_buf_size) {
    // Torrent-catalog entries (see sources.h's SOURCE_KIND_TORRENT_CATALOG)
    // carry a magnet: URI in download_url instead of an HTTP link and go
    // through an entirely different pipeline - checked first since
    // entry->source_base_url is meaningless for them (there is no relative
    // URL to resolve against a base).
    if (entry->via_torrent) {
        TorrentInstallResult r = install_torrent(entry, cb, phase_cb, userdata, err_buf, err_buf_size);
        if (r == TORRENT_INSTALL_OK) return INSTALL_ONE_OK;
        if (r == TORRENT_INSTALL_ERR_CANCELED) return INSTALL_ONE_CANCELED;
        return INSTALL_ONE_ERROR;
    }

    const char *base_url = entry->source_base_url;

    // NSZ shares the exact same PFS0/CNMT/ticket structure as NSP - the only
    // difference is that some content pieces are a compressed ".ncz" instead
    // of a plain ".nca", which install_nsp_native's own content loop already
    // detects and routes through ncm_install_ncz_content_from_url (see
    // ncz.h). No separate NSZ installer needed.
    if (entry->file_type == APP_FILE_TYPE_NSP || entry->file_type == APP_FILE_TYPE_NSZ) {
        NspInstallResult r = install_nsp_native(entry, base_url, cb, phase_cb, userdata, err_buf, err_buf_size);
        if (r == NSP_INSTALL_OK) return INSTALL_ONE_OK;
        if (r == NSP_INSTALL_ERR_CANCELED) return INSTALL_ONE_CANCELED;
        return INSTALL_ONE_ERROR;
    }
    if (entry->file_type == APP_FILE_TYPE_XCI) {
        XciInstallResult r = install_xci_native(entry, base_url, cb, phase_cb, userdata, err_buf, err_buf_size);
        if (r == XCI_INSTALL_OK) return INSTALL_ONE_OK;
        if (r == XCI_INSTALL_ERR_CANCELED) return INSTALL_ONE_CANCELED;
        return INSTALL_ONE_ERROR;
    }
    if (entry->file_type == APP_FILE_TYPE_PORT) {
        PortInstallResult r = install_port(entry, base_url, cb, phase_cb, userdata, err_buf, err_buf_size);
        if (r == PORT_INSTALL_OK) return INSTALL_ONE_OK;
        if (r == PORT_INSTALL_ERR_CANCELED) return INSTALL_ONE_CANCELED;
        return INSTALL_ONE_ERROR;
    }

    InstallResult r = install_app(entry, base_url, cb, userdata, err_buf, err_buf_size);
    if (r == INSTALL_OK) return INSTALL_ONE_OK;
    if (r == INSTALL_ERR_CANCELED) return INSTALL_ONE_CANCELED;
    return INSTALL_ONE_ERROR;
}

bool install_suggests_dbi_fallback(AppFileType type) {
    return type == APP_FILE_TYPE_NSP || type == APP_FILE_TYPE_XCI || type == APP_FILE_TYPE_NSZ;
}
