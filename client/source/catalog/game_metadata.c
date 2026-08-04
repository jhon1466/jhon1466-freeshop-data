#include "game_metadata.h"
#include "../net/http.h"
#include "text_sanitize.h"

#include <ctype.h>
#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define METADATA_MANIFEST_URL \
    "https://github.com/i3sey/pipensx-metadata/releases/latest/download/manifest.json"

typedef struct {
    char info_hash_hex[41];
    char *name;        // malloc'd, NULL if the index didn't carry one
    char *description; // malloc'd, NULL if the index didn't carry one
    char *icon_url;     // malloc'd, NULL if the index didn't carry one
} game_metadata_entry_t;

static game_metadata_entry_t *g_entries = NULL;
static size_t g_count = 0;
static bool g_load_attempted = false;
// Guards the three globals above. Only matters if more than one enabled
// source is SOURCE_KIND_TORRENT_CATALOG (today's UI never creates that -
// only the built-in bootstrap source ever gets that kind - but sources.json
// is hand-editable, and fetch_merged_catalog fetches every enabled source
// concurrently, one thread each, so a second one is a real if exotic case
// rather than dead code).
static pthread_mutex_t g_load_mutex = PTHREAD_MUTEX_INITIALIZER;

static int compare_entries(const void *a, const void *b) {
    return strcmp(((const game_metadata_entry_t*)a)->info_hash_hex,
                  ((const game_metadata_entry_t*)b)->info_hash_hex);
}

static void normalize_hash(const char *in, char out[41]) {
    size_t i = 0;
    for (; i < 40 && in[i]; i++)
        out[i] = (char)toupper((unsigned char)in[i]);
    out[i] = '\0';
}

// Sanitized (see text_sanitize.h) - this index's descriptions are
// eShop-style copy that routinely uses emoji/dingbat bullets as section
// markers ("mountain", a controller, a star, ...), none of which this
// client's font has a glyph for. len+1 is a safe upper bound for the
// sanitized copy since stripping can only ever shrink the byte count.
static char *dup_json_string(const json_t *obj, const char *key) {
    const json_t *v = json_object_get(obj, key);
    if (!json_is_string(v)) return NULL;
    const char *s = json_string_value(v);
    if (!s || !*s) return NULL;
    size_t len = strlen(s);
    char *copy = (char*)malloc(len + 1);
    if (copy) utf8_strip_unrenderable(s, copy, len + 1);
    return (copy && copy[0]) ? copy : (free(copy), NULL);
}

bool game_metadata_ensure_loaded(void) {
    pthread_mutex_lock(&g_load_mutex);
    if (g_load_attempted) {
        bool have = g_entries != NULL;
        pthread_mutex_unlock(&g_load_mutex);
        return have;
    }
    g_load_attempted = true;
    pthread_mutex_unlock(&g_load_mutex);

    // Everything below runs unlocked - it only touches locals until the
    // very end, where the result is published under the lock in one shot.
    char err[160];
    char *manifest_raw = NULL;
    size_t manifest_len = 0;
    if (http_get(METADATA_MANIFEST_URL, &manifest_raw, &manifest_len, err, sizeof(err)) != HTTP_OK)
        return false;

    json_error_t jerr;
    json_t *manifest = json_loadb(manifest_raw, manifest_len, 0, &jerr);
    free(manifest_raw);
    if (!manifest) return false;

    const json_t *index = json_object_get(manifest, "index");
    const json_t *index_url_json = json_is_object(index) ? json_object_get(index, "url") : NULL;
    if (!json_is_string(index_url_json)) {
        json_decref(manifest);
        return false;
    }
    char index_url[600];
    snprintf(index_url, sizeof(index_url), "%s", json_string_value(index_url_json));
    json_decref(manifest);

    char *index_raw = NULL;
    size_t index_len = 0;
    if (http_get(index_url, &index_raw, &index_len, err, sizeof(err)) != HTTP_OK)
        return false;

    json_t *root = json_loadb(index_raw, index_len, 0, &jerr);
    free(index_raw);
    if (!root || !json_is_array(root)) {
        if (root) json_decref(root);
        return false;
    }

    size_t total = json_array_size(root);
    game_metadata_entry_t *entries =
        (game_metadata_entry_t*)calloc(total > 0 ? total : 1, sizeof(game_metadata_entry_t));
    if (!entries) {
        json_decref(root);
        return false;
    }

    size_t count = 0;
    for (size_t i = 0; i < total; i++) {
        const json_t *item = json_array_get(root, i);
        const json_t *hash_json = json_object_get(item, "infoHash");
        if (!json_is_string(hash_json)) continue;
        const char *hash_str = json_string_value(hash_json);
        if (strlen(hash_str) != 40) continue;

        game_metadata_entry_t *e = &entries[count];
        normalize_hash(hash_str, e->info_hash_hex);
        e->name = dup_json_string(item, "name");
        e->description = dup_json_string(item, "description");
        e->icon_url = dup_json_string(item, "iconUrl");
        if (!e->name && !e->description && !e->icon_url) continue; // nothing usable from this entry
        count++;
    }
    json_decref(root);

    if (count == 0) {
        free(entries);
        return false;
    }
    qsort(entries, count, sizeof(entries[0]), compare_entries);

    pthread_mutex_lock(&g_load_mutex);
    g_entries = entries;
    g_count = count;
    pthread_mutex_unlock(&g_load_mutex);
    return true;
}

static const game_metadata_entry_t *find_entry(const char *info_hash_hex) {
    pthread_mutex_lock(&g_load_mutex);
    game_metadata_entry_t *snapshot_entries = g_entries;
    size_t snapshot_count = g_count;
    pthread_mutex_unlock(&g_load_mutex);

    if (!snapshot_entries || !info_hash_hex) return NULL;
    game_metadata_entry_t needle;
    memset(&needle, 0, sizeof(needle));
    normalize_hash(info_hash_hex, needle.info_hash_hex);
    return (const game_metadata_entry_t*)bsearch(&needle, snapshot_entries, snapshot_count,
                                                 sizeof(snapshot_entries[0]), compare_entries);
}

const char *game_metadata_find_name(const char *info_hash_hex) {
    const game_metadata_entry_t *e = find_entry(info_hash_hex);
    return e ? e->name : NULL;
}

const char *game_metadata_find_description(const char *info_hash_hex) {
    const game_metadata_entry_t *e = find_entry(info_hash_hex);
    return e ? e->description : NULL;
}

const char *game_metadata_find_icon_url(const char *info_hash_hex) {
    const game_metadata_entry_t *e = find_entry(info_hash_hex);
    return e ? e->icon_url : NULL;
}
