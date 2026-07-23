#pragma once
#include "../net/http.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SELF_UPDATE_NONE,      // already on the latest published version
    SELF_UPDATE_AVAILABLE, // a different version is published - out_new_version/out_asset_url filled
    SELF_UPDATE_ERR_CHECK, // couldn't reach or parse the GitHub Releases API
} SelfUpdateCheckResult;

// Checks jhon1466-freeshop-data's latest GitHub Release (see
// CLIENT_RELEASES_API_URL in config.h) and compares its tag (a leading 'v'
// is stripped, either form works) against CLIENT_VERSION as a plain string -
// NOT semver-aware, so this only detects "different", not "newer"; don't
// publish a release with an older version string than what's currently out.
// On SELF_UPDATE_AVAILABLE, out_new_version (may be NULL) gets the tag's
// version and out_asset_url (may be NULL) gets the direct download URL of
// the CLIENT_RELEASE_ASSET_NAME asset attached to that release.
SelfUpdateCheckResult self_update_check(char *out_new_version, size_t out_version_size,
                                         char *out_asset_url, size_t out_asset_url_size,
                                         char *err_buf, size_t err_buf_size);

typedef enum {
    SELF_UPDATE_APPLY_OK,
    SELF_UPDATE_APPLY_ERR_DOWNLOAD,
    SELF_UPDATE_APPLY_ERR_NO_SELF_PATH, // self_path wasn't usable (e.g. launched via nxlink, not hbmenu)
    SELF_UPDATE_APPLY_ERR_REPLACE,
} SelfUpdateApplyResult;

// Downloads `asset_url` and overwrites `self_path` (the currently-running
// .nro's own path - hbmenu passes this as argv[0]) with it. Safe to do while
// still running: nx-hbloader loads the whole .nro into memory upfront and
// doesn't keep the file open during execution, so this process keeps
// working normally - the new version only takes effect the next time the
// app is launched from hbmenu.
SelfUpdateApplyResult self_update_apply(const char *self_path, const char *asset_url,
                                         HttpProgressCallback cb, void *userdata,
                                         char *err_buf, size_t err_buf_size);
