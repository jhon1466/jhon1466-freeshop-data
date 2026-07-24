#include "self_update.h"
#include "../config.h"
#include "../install/install_common.h"

#include <errno.h>
#include <jansson.h>
#include <stdio.h>
#include <string.h>

SelfUpdateCheckResult self_update_check(char *out_new_version, size_t out_version_size,
                                         char *out_asset_url, size_t out_asset_url_size,
                                         char *out_release_notes, size_t out_release_notes_size,
                                         char *err_buf, size_t err_buf_size) {
    char *buf = NULL;
    size_t len = 0;
    HttpResult hres = http_get(CLIENT_RELEASES_API_URL, &buf, &len, err_buf, err_buf_size);
    if (hres != HTTP_OK) {
        return SELF_UPDATE_ERR_CHECK;
    }

    json_error_t jerr;
    json_t *root = json_loads(buf, 0, &jerr);
    free(buf);
    if (!root) {
        if (err_buf) snprintf(err_buf, err_buf_size, "respuesta de GitHub inválida: %s", jerr.text);
        return SELF_UPDATE_ERR_CHECK;
    }

    const json_t *tag_json = json_object_get(root, "tag_name");
    const char *tag = json_is_string(tag_json) ? json_string_value(tag_json) : NULL;
    if (!tag) {
        json_decref(root);
        if (err_buf) snprintf(err_buf, err_buf_size, "la última release no tiene tag_name");
        return SELF_UPDATE_ERR_CHECK;
    }

    const char *version = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;

    if (strcmp(version, CLIENT_VERSION) == 0) {
        json_decref(root);
        return SELF_UPDATE_NONE;
    }

    const char *asset_url = NULL;
    json_t *assets = json_object_get(root, "assets");
    if (json_is_array(assets)) {
        size_t i;
        json_t *asset;
        json_array_foreach(assets, i, asset) {
            const json_t *name_json = json_object_get(asset, "name");
            const char *name = json_is_string(name_json) ? json_string_value(name_json) : NULL;
            if (name && strcmp(name, CLIENT_RELEASE_ASSET_NAME) == 0) {
                const json_t *url_json = json_object_get(asset, "browser_download_url");
                asset_url = json_is_string(url_json) ? json_string_value(url_json) : NULL;
                break;
            }
        }
    }

    if (!asset_url) {
        if (err_buf) snprintf(err_buf, err_buf_size,
                               "la release %s no tiene un archivo %s adjunto", tag, CLIENT_RELEASE_ASSET_NAME);
        json_decref(root);
        return SELF_UPDATE_ERR_CHECK;
    }

    if (out_new_version) snprintf(out_new_version, out_version_size, "%s", version);
    if (out_asset_url) snprintf(out_asset_url, out_asset_url_size, "%s", asset_url);
    if (out_release_notes) {
        const json_t *body_json = json_object_get(root, "body");
        const char *body = json_is_string(body_json) ? json_string_value(body_json) : "";
        snprintf(out_release_notes, out_release_notes_size, "%s", body);
    }

    json_decref(root);
    return SELF_UPDATE_AVAILABLE;
}

SelfUpdateApplyResult self_update_apply(const char *self_path, const char *asset_url,
                                         HttpProgressCallback cb, void *userdata,
                                         char *err_buf, size_t err_buf_size) {
    if (!self_path || self_path[0] == '\0') {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo determinar la ubicación del ejecutable actual");
        return SELF_UPDATE_APPLY_ERR_NO_SELF_PATH;
    }

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.update", self_path);

    HttpResult hres = http_download_to_file(asset_url, tmp_path, cb, userdata, err_buf, err_buf_size);
    if (hres != HTTP_OK) {
        remove(tmp_path);
        return SELF_UPDATE_APPLY_ERR_DOWNLOAD;
    }

    // Some sdmc filesystem driver states refuse rename() onto an existing
    // destination - clear it first, same reasoning as install_nsp.c. Its
    // own success/failure isn't fatal by itself (rename/fopen below might
    // still work either way) but its errno is worth keeping in case both
    // fallbacks fail, to actually see why instead of guessing.
    int remove_errno = 0;
    if (remove(self_path) != 0) remove_errno = errno;

    if (rename(tmp_path, self_path) != 0) {
        int rename_errno = errno;
        if (install_common_copy_file(tmp_path, self_path) != 0) {
            int copy_errno = errno;
            remove(tmp_path);
            if (err_buf) {
                snprintf(err_buf, err_buf_size,
                         "no se pudo reemplazar el archivo actual (remove: %s, rename: %s, copy: %s)",
                         remove_errno ? strerror(remove_errno) : "ok",
                         strerror(rename_errno), strerror(copy_errno));
            }
            return SELF_UPDATE_APPLY_ERR_REPLACE;
        }
        remove(tmp_path);
    }

    return SELF_UPDATE_APPLY_OK;
}
