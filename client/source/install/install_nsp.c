#include "install_nsp.h"
#include "install_common.h"
#include "../config.h"
#include "../net/http.h"

#include <switch.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define DBI_NRO_PATH SWITCH_APPS_ROOT "/DBI/dbi.nro"
#define DBI_DIR_PATH SWITCH_APPS_ROOT "/DBI"
#define NSP_REPO_DIR_PATH SWITCH_APPS_ROOT "/DBI/nsp-repo"

NspHandoffResult install_nsp_and_launch_dbi(const AppEntry *entry, const char *base_url,
                                             InstallProgressCallback cb, void *userdata,
                                             char *err_buf, size_t err_buf_size) {
    const char *dbi_dir = DBI_DIR_PATH;
    const char *dest_dir = NSP_REPO_DIR_PATH;

    install_common_mkdir_ignore_exists(SWITCH_APPS_ROOT);
    install_common_mkdir_ignore_exists(dbi_dir);
    install_common_mkdir_ignore_exists(dest_dir);

    struct statvfs st;
    if (statvfs("sdmc:/", &st) == 0) {
        unsigned long long free_bytes = (unsigned long long)st.f_bsize * st.f_bavail;
        if (entry->file_size > 0 && free_bytes < (unsigned long long)entry->file_size) {
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "no hay suficiente espacio libre en la tarjeta SD (se necesitan %ld bytes)",
                                   entry->file_size);
            return NSP_HANDOFF_ERR_NO_SPACE;
        }
    }

    char part_path[512];
    snprintf(part_path, sizeof(part_path), "%s/%s.part", dest_dir, entry->filename);

    char url[900];
    install_common_resolve_url(base_url, entry->download_url, url, sizeof(url));

    InstallProgressThunkCtx thunk_ctx = { .cb = cb, .userdata = userdata };
    HttpResult hres = http_download_to_file(url, part_path, install_common_progress_thunk, &thunk_ctx,
                                             err_buf, err_buf_size);
    if (hres == HTTP_ERR_CANCELED) {
        return NSP_HANDOFF_ERR_CANCELED;
    }
    if (hres != HTTP_OK) {
        return NSP_HANDOFF_ERR_DOWNLOAD;
    }

    // sha256 is optional in the catalog now - skip the check (and the cost
    // of hashing a potentially multi-GB file) when the entry doesn't have one.
    if (entry->sha256[0] != '\0') {
        char actual_hex[65];
        if (install_common_sha256_file(part_path, actual_hex) != 0) {
            remove(part_path);
            if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo leer el archivo descargado para verificar el checksum");
            return NSP_HANDOFF_ERR_DOWNLOAD;
        }

        if (strcasecmp(actual_hex, entry->sha256) != 0) {
            remove(part_path);
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "el checksum no coincide (esperado %s, obtenido %s) - la descarga está corrupta",
                                   entry->sha256, actual_hex);
            return NSP_HANDOFF_ERR_HASH_MISMATCH;
        }
    }

    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/%s", dest_dir, entry->filename);

    // Clear out a stale file from a previous attempt first - some FS drivers
    // refuse to rename() onto an existing destination.
    remove(final_path);

    if (rename(part_path, final_path) != 0) {
        int rename_errno = errno;
        // Fall back to a manual copy+delete - less atomic, but works even
        // when the underlying sdmc driver's rename() doesn't behave like
        // POSIX expects.
        if (install_common_copy_file(part_path, final_path) != 0) {
            int copy_errno = errno;
            remove(part_path);
            if (err_buf) {
                snprintf(err_buf, err_buf_size,
                         "no se pudo mover el archivo descargado a su ubicación final (rename: %s, copy: %s)",
                         strerror(rename_errno), strerror(copy_errno));
            }
            return NSP_HANDOFF_ERR_RENAME;
        }
        remove(part_path);
    }

    struct stat dbi_st;
    if (stat(DBI_NRO_PATH, &dbi_st) != 0) {
        if (err_buf) {
            snprintf(err_buf, err_buf_size,
                     "guardado en %s, pero DBI no está instalado en %s - instala DBI "
                     "(https://github.com/rashevskyv/dbi) primero, y luego ábrelo manualmente "
                     "para instalar este archivo",
                     final_path, DBI_NRO_PATH);
        }
        return NSP_HANDOFF_ERR_NO_DBI;
    }

    // Takes effect once this process exits normally - see install_nsp.h.
    envSetNextLoad(DBI_NRO_PATH, DBI_NRO_PATH);
    return NSP_HANDOFF_OK;
}
