#include "sources.h"
#include "../config.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SOURCES_DIR "sdmc:/switch/freeshop"
#define SOURCES_PATH SOURCES_DIR "/sources.json"

// A hand-edited or corrupted file could be arbitrarily large; cap what we're
// willing to read into memory rather than trusting the file size blindly.
#define SOURCES_FILE_MAX_BYTES 65536

// The user's switch-games torrent catalog fork (see torrent_resolver.h's
// doc comment for why only RuTracker's t-ru.org mirrors are trusted as
// trackers - this fork's magnets all carry one of those).
#define TORRENT_CATALOG_URL \
    "https://raw.githubusercontent.com/jhon1466/switch-games/refs/heads/main/switch_games.json"

// Appends the built-in torrent-catalog source to `out` (caller must have
// checked there's room). Shared by set_default (fresh install) and
// sources_load's one-shot migration (existing install).
static void add_torrent_source(SourceList *out) {
    CatalogSource *torrents = &out->items[out->count++];
    snprintf(torrents->name, sizeof(torrents->name), "Torrents (switch-games)");
    snprintf(torrents->base_url, sizeof(torrents->base_url), "%s", TORRENT_CATALOG_URL);
    torrents->enabled = true;
    // Visible (not hidden) and user-toggleable in "Fuentes": unlike the
    // FreeShop bootstrap source below, this one downloads real files over
    // BitTorrent (DHT, background threads, peer connections) - the user
    // should be able to see it and turn it off.
    torrents->hidden = false;
    torrents->kind = SOURCE_KIND_TORRENT_CATALOG;
    out->torrent_source_offered = true;
}

static void set_default(SourceList *out) {
    out->count = 0;
    out->torrent_source_offered = false;

    CatalogSource *freeshop = &out->items[out->count++];
    snprintf(freeshop->name, sizeof(freeshop->name), "FreeShop");
    snprintf(freeshop->base_url, sizeof(freeshop->base_url), "%s", CATALOG_BASE_URL);
    freeshop->enabled = true;
    freeshop->hidden = true;
    freeshop->kind = SOURCE_KIND_STANDARD;

    add_torrent_source(out);
}

static char *read_whole_file(const char *path, size_t max_bytes, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size <= 0 || (size_t)size > max_bytes) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    size_t read_n = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[read_n] = '\0';
    if (out_len) *out_len = read_n;
    return buf;
}

void sources_load(SourceList *out) {
    out->count = 0;

    size_t len = 0;
    char *buf = read_whole_file(SOURCES_PATH, SOURCES_FILE_MAX_BYTES, &len);
    if (!buf) {
        set_default(out);
        sources_save(out);
        return;
    }

    json_error_t jerr;
    json_t *root = json_loadb(buf, len, 0, &jerr);
    free(buf);

    const json_t *arr = root ? json_object_get(root, "sources") : NULL;
    if (!root || !json_is_array(arr)) {
        if (root) json_decref(root);
        set_default(out);
        sources_save(out);
        return;
    }

    const json_t *offered = json_object_get(root, "torrentSourceOffered");
    out->torrent_source_offered = json_is_boolean(offered) ? json_is_true(offered) : false;

    size_t n = json_array_size(arr);
    for (size_t i = 0; i < n && out->count < SOURCES_MAX; i++) {
        const json_t *item = json_array_get(arr, i);
        const json_t *name = json_object_get(item, "name");
        const json_t *url = json_object_get(item, "baseUrl");
        const json_t *enabled = json_object_get(item, "enabled");
        const json_t *hidden = json_object_get(item, "hidden");
        const json_t *kind = json_object_get(item, "kind");
        if (!json_is_string(url) || json_string_value(url)[0] == '\0') continue;

        CatalogSource *dst = &out->items[out->count];
        snprintf(dst->name, sizeof(dst->name), "%s",
                 json_is_string(name) ? json_string_value(name) : "(sin nombre)");
        snprintf(dst->base_url, sizeof(dst->base_url), "%s", json_string_value(url));
        dst->enabled = json_is_boolean(enabled) ? json_is_true(enabled) : true;
        // Defaults to false (visible) for anything hand-edited or written
        // before this field existed - only sources_load's own bootstrap
        // default (set_default) is ever hidden=true.
        dst->hidden = json_is_boolean(hidden) ? json_is_true(hidden) : false;
        // Defaults to STANDARD for anything written before this field
        // existed - only set_default's own torrent-catalog entry is ever
        // SOURCE_KIND_TORRENT_CATALOG.
        dst->kind = (json_is_integer(kind) && json_integer_value(kind) == SOURCE_KIND_TORRENT_CATALOG)
                        ? SOURCE_KIND_TORRENT_CATALOG : SOURCE_KIND_STANDARD;
        out->count++;
    }

    json_decref(root);

    // Migrate installs whose sources.json still has the pre-Worker default
    // baked in: only the hidden bootstrap entry (written by set_default,
    // never by the user - see the struct field comment in sources.h) is
    // ever rewritten, and only if it still matches the old URL exactly, so
    // a source the user deliberately pointed at raw.githubusercontent.com
    // themselves is left alone.
    bool migrated = false;
    for (int i = 0; i < out->count; i++) {
        if (out->items[i].hidden && strcmp(out->items[i].base_url, LEGACY_CATALOG_BASE_URL) == 0) {
            snprintf(out->items[i].base_url, sizeof(out->items[i].base_url), "%s", CATALOG_BASE_URL);
            migrated = true;
        }
    }
    // One-time migration for installs whose sources.json predates the
    // torrent catalog: append it once, gated on the "torrentSourceOffered"
    // flag (not on whether a TORRENT_CATALOG entry is currently present)
    // so a user who deletes it from "Fuentes" afterward keeps it gone
    // instead of it reappearing on the next load.
    if (!out->torrent_source_offered && out->count < SOURCES_MAX) {
        add_torrent_source(out);
        migrated = true;
    }

    if (migrated) sources_save(out);

    if (out->count == 0) {
        set_default(out);
        sources_save(out);
    }
}

bool sources_save(const SourceList *list) {
    mkdir(SOURCES_DIR, 0777); // ignore EEXIST/any error - a real failure surfaces on fopen below

    json_t *root = json_object();
    json_t *arr = json_array();
    for (int i = 0; i < list->count; i++) {
        json_t *item = json_object();
        json_object_set_new(item, "name", json_string(list->items[i].name));
        json_object_set_new(item, "baseUrl", json_string(list->items[i].base_url));
        json_object_set_new(item, "enabled", json_boolean(list->items[i].enabled));
        json_object_set_new(item, "hidden", json_boolean(list->items[i].hidden));
        json_object_set_new(item, "kind", json_integer(list->items[i].kind));
        json_array_append_new(arr, item);
    }
    json_object_set_new(root, "sources", arr);
    json_object_set_new(root, "torrentSourceOffered", json_boolean(list->torrent_source_offered));

    char *text = json_dumps(root, JSON_INDENT(2));
    json_decref(root);
    if (!text) return false;

    FILE *fp = fopen(SOURCES_PATH, "wb");
    if (!fp) {
        free(text);
        return false;
    }
    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, fp) == len;
    fclose(fp);
    free(text);
    return ok;
}
