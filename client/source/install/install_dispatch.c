#include "install_dispatch.h"
#include "install_nsp_native.h"
#include "install_xci_native.h"
#include "install_port.h"

InstallOneResult install_one_entry(const AppEntry *entry,
                                   InstallProgressCallback cb, InstallPhaseCallback phase_cb, void *userdata,
                                   char *err_buf, size_t err_buf_size) {
    const char *base_url = entry->source_base_url;

    if (entry->file_type == APP_FILE_TYPE_NSP) {
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
    return type == APP_FILE_TYPE_NSP || type == APP_FILE_TYPE_XCI;
}
