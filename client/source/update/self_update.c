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
                                         char *out_staging_path, size_t out_staging_path_size,
                                         HttpProgressCallback cb, void *userdata,
                                         char *err_buf, size_t err_buf_size) {
    if (!self_path || self_path[0] == '\0') {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo determinar la ubicación del ejecutable actual");
        return SELF_UPDATE_APPLY_ERR_NO_SELF_PATH;
    }

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s%s", self_path, SELF_UPDATE_STAGING_SUFFIX);

    HttpResult hres = http_download_to_file(asset_url, tmp_path, cb, userdata, err_buf, err_buf_size);
    if (hres != HTTP_OK) {
        remove(tmp_path);
        return SELF_UPDATE_APPLY_ERR_DOWNLOAD;
    }

    // Deliberately NOT touching self_path here - see self_update_finish_swap()'s
    // doc comment for why (a running .nro can't remove()/rename() itself on
    // real hardware). The caller chain-loads into tmp_path next; the swap
    // onto self_path happens from over there, once nothing is executing
    // out of self_path anymore.
    if (out_staging_path) snprintf(out_staging_path, out_staging_path_size, "%s", tmp_path);
    return SELF_UPDATE_APPLY_OK;
}

bool self_update_is_staging_copy(const char *self_path) {
    if (!self_path) return false;
    size_t len = strlen(self_path);
    size_t suffix_len = strlen(SELF_UPDATE_STAGING_SUFFIX);
    return len > suffix_len && strcmp(self_path + (len - suffix_len), SELF_UPDATE_STAGING_SUFFIX) == 0;
}

SelfUpdateSwapResult self_update_finish_swap(const char *self_path, char *out_canonical_path,
                                              size_t out_canonical_path_size,
                                              char *err_buf, size_t err_buf_size) {
    size_t len = strlen(self_path);
    size_t suffix_len = strlen(SELF_UPDATE_STAGING_SUFFIX);
    char canonical[512];
    snprintf(canonical, sizeof(canonical), "%.*s", (int)(len - suffix_len), self_path);

    // Same rename-refuses-an-existing-destination gotcha as before - but this
    // time it's genuinely safe to clear first, since `canonical` isn't the
    // file this process is running from (that's `self_path`, the staging
    // copy).
    remove(canonical);

    int rename_errno = 0;
    if (rename(self_path, canonical) != 0) {
        rename_errno = errno;
        if (install_common_copy_file(self_path, canonical) != 0) {
            int copy_errno = errno;
            if (err_buf) {
                snprintf(err_buf, err_buf_size,
                         "no se pudo completar la actualización (rename: %s, copy: %s)",
                         strerror(rename_errno), strerror(copy_errno));
            }
            return SELF_UPDATE_SWAP_ERR_REPLACE;
        }
        remove(self_path);
    }

    if (out_canonical_path) snprintf(out_canonical_path, out_canonical_path_size, "%s", canonical);
    return SELF_UPDATE_SWAP_OK;
}
