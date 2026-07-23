#include "install_port.h"
#include "install_common.h"
#include "zip_extract.h"
#include "../config.h"
#include "../net/http.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

PortInstallResult install_port(const AppEntry *entry, const char *base_url,
                                InstallProgressCallback cb, InstallPhaseCallback phase_cb, void *userdata,
                                char *err_buf, size_t err_buf_size) {
    char dest_dir[300];
    snprintf(dest_dir, sizeof(dest_dir), "%s/%s", SWITCH_APPS_ROOT, entry->id);
    install_common_mkdir_ignore_exists(SWITCH_APPS_ROOT);
    install_common_mkdir_ignore_exists(dest_dir);

    struct statvfs st;
    if (statvfs("sdmc:/", &st) == 0) {
        unsigned long long free_bytes = (unsigned long long)st.f_bsize * st.f_bavail;
        // The zip needs room alongside the space its own extracted contents
        // will take (typically bigger than the compressed download, by an
        // unknown factor) - same conservative check every other installer
        // does with the one number the catalog provides.
        if (entry->file_size > 0 && free_bytes < (unsigned long long)entry->file_size) {
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "no hay suficiente espacio libre en la tarjeta SD (se necesitan %ld bytes)",
                                   entry->file_size);
            return PORT_INSTALL_ERR_NO_SPACE;
        }
    }

    char part_path[512];
    snprintf(part_path, sizeof(part_path), "%s/%s.part", dest_dir, entry->filename);

    char url[900];
    install_common_resolve_url(base_url, entry->download_url, url, sizeof(url));

    HttpResult hres = http_download_to_file(url, part_path, cb, userdata, err_buf, err_buf_size);
    if (hres == HTTP_ERR_CANCELED) {
        return PORT_INSTALL_ERR_CANCELED;
    }
    if (hres != HTTP_OK) {
        return PORT_INSTALL_ERR_DOWNLOAD;
    }

    if (entry->sha256[0] != '\0') {
        char actual_hex[65];
        if (install_common_sha256_file(part_path, actual_hex) != 0) {
            remove(part_path);
            if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo leer el archivo descargado para verificar el checksum");
            return PORT_INSTALL_ERR_DOWNLOAD;
        }
        if (strcasecmp(actual_hex, entry->sha256) != 0) {
            remove(part_path);
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "el checksum no coincide (esperado %s, obtenido %s) - la descarga está corrupta",
                                   entry->sha256, actual_hex);
            return PORT_INSTALL_ERR_HASH_MISMATCH;
        }
    }

    if (phase_cb) phase_cb(INSTALL_PHASE_INSTALLING, userdata);

    if (!zip_extract_to_dir(part_path, dest_dir, cb, userdata, err_buf, err_buf_size)) {
        bool canceled = err_buf && strstr(err_buf, "cancel") != NULL;
        remove(part_path);
        return canceled ? PORT_INSTALL_ERR_CANCELED : PORT_INSTALL_ERR_EXTRACT;
    }

    remove(part_path);
    return PORT_INSTALL_OK;
}
