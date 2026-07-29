#include "install.h"
#include "install_common.h"
#include "../config.h"
#include "../net/http.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

InstallResult install_app(const AppEntry *entry, const char *base_url,
                           InstallProgressCallback cb, void *userdata,
                           char *err_buf, size_t err_buf_size) {
    char dest_dir[300];
    snprintf(dest_dir, sizeof(dest_dir), "%s/%s", SWITCH_APPS_ROOT, entry->id);

    install_common_mkdir_ignore_exists(SWITCH_APPS_ROOT);
    install_common_mkdir_ignore_exists(dest_dir);

    struct statvfs st;
    if (statvfs("sdmc:/", &st) == 0) {
        unsigned long long free_bytes = (unsigned long long)st.f_bsize * st.f_bavail;
        if (entry->file_size > 0 && free_bytes < (unsigned long long)entry->file_size) {
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "no hay suficiente espacio libre en la tarjeta SD (se necesitan %ld bytes)",
                                   entry->file_size);
            return INSTALL_ERR_NO_SPACE;
        }
    }

    char part_path[512];
    snprintf(part_path, sizeof(part_path), "%s/%s.part", dest_dir, entry->filename);

    char resolved[900];
    install_common_resolve_url(base_url, entry->download_url, resolved, sizeof(resolved));
    char url[900];
    install_common_direct_download_url(resolved, url, sizeof(url));

    InstallProgressThunkCtx thunk_ctx = { .cb = cb, .userdata = userdata };
    HttpResult hres = http_download_to_file(url, part_path, install_common_progress_thunk, &thunk_ctx,
                                             err_buf, err_buf_size);
    if (hres == HTTP_ERR_CANCELED) {
        return INSTALL_ERR_CANCELED;
    }
    if (hres != HTTP_OK) {
        return INSTALL_ERR_DOWNLOAD;
    }

    // sha256 is optional in the catalog now - skip the check (and the cost
    // of hashing a potentially multi-GB file) when the entry doesn't have one.
    if (entry->sha256[0] != '\0') {
        char actual_hex[65];
        if (install_common_sha256_file(part_path, actual_hex) != 0) {
            remove(part_path);
            if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo leer el archivo descargado para verificar el checksum");
            return INSTALL_ERR_DOWNLOAD;
        }

        if (strcasecmp(actual_hex, entry->sha256) != 0) {
            remove(part_path);
            if (err_buf) snprintf(err_buf, err_buf_size,
                                   "el checksum no coincide (esperado %s, obtenido %s) - la descarga está corrupta",
                                   entry->sha256, actual_hex);
            return INSTALL_ERR_HASH_MISMATCH;
        }
    }

    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/%s", dest_dir, entry->filename);

    // Clear out a stale file from a previous install/update attempt first -
    // some FS drivers refuse to rename() onto an existing destination.
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
            return INSTALL_ERR_RENAME;
        }
        remove(part_path);
    }

    return INSTALL_OK;
}
