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
// version, out_asset_url (may be NULL) gets the direct download URL of the
// CLIENT_RELEASE_ASSET_NAME asset attached to that release, and
// out_release_notes (may be NULL) gets that release's own description
// (GitHub's "body" field - whatever changelog text was written when
// publishing it), truncated to fit out_release_notes_size - empty string if
// the release has no description at all.
SelfUpdateCheckResult self_update_check(char *out_new_version, size_t out_version_size,
                                         char *out_asset_url, size_t out_asset_url_size,
                                         char *out_release_notes, size_t out_release_notes_size,
                                         char *err_buf, size_t err_buf_size);

typedef enum {
    SELF_UPDATE_APPLY_OK,
    SELF_UPDATE_APPLY_ERR_DOWNLOAD,
    SELF_UPDATE_APPLY_ERR_NO_SELF_PATH, // self_path wasn't usable (e.g. launched via nxlink, not hbmenu)
} SelfUpdateApplyResult;

// Suffix used for the staging copy self_update_apply() downloads to and
// self_update_is_staging_copy()/self_update_finish_swap() look for. Exposed
// so main() can build the same path both functions agree on.
#define SELF_UPDATE_STAGING_SUFFIX ".update"

// Downloads `asset_url` to `self_path` + SELF_UPDATE_STAGING_SUFFIX (does NOT
// touch `self_path` itself - see the comment on self_update_finish_swap()
// below for why not) and writes that staging path to out_staging_path. The
// caller is expected to envSetNextLoad() into it and exit immediately so a
// fresh process picks up the swap on its next launch.
SelfUpdateApplyResult self_update_apply(const char *self_path, const char *asset_url,
                                         char *out_staging_path, size_t out_staging_path_size,
                                         HttpProgressCallback cb, void *userdata,
                                         char *err_buf, size_t err_buf_size);

// True if `self_path` is a SELF_UPDATE_STAGING_SUFFIX-suffixed staging copy
// (i.e. this process was itself chain-loaded from self_update_apply()'s
// output) rather than the app's normal, canonical launch path.
bool self_update_is_staging_copy(const char *self_path);

typedef enum {
    SELF_UPDATE_SWAP_OK,
    SELF_UPDATE_SWAP_ERR_REPLACE,
} SelfUpdateSwapResult;

// Second hop of an update, called (only) when self_update_is_staging_copy()
// is true: copies this running staging file onto the canonical path (i.e.
// `self_path` with the suffix stripped, written to out_canonical_path) and
// deletes the staging file. Confirmed on real hardware that a running .nro
// can't remove()/rename() *itself* (nx-hbloader appears to keep the file
// open/locked for the life of the process, unlike the "loads fully into
// memory upfront" assumption the old single-hop version of this code made) -
// this function is only ever called from a process running out of the
// staging file, so the canonical path it's replacing here is a different,
// not-currently-executing file, which is safe. On success the caller should
// envSetNextLoad() into out_canonical_path and exit so the final process
// runs from the "real" filename hbmenu shows, rather than staying on the
// staging copy forever.
SelfUpdateSwapResult self_update_finish_swap(const char *self_path, char *out_canonical_path,
                                              size_t out_canonical_path_size,
                                              char *err_buf, size_t err_buf_size);
