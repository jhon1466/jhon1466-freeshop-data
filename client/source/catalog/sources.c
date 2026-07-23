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

static void set_default(SourceList *out) {
    out->count = 1;
    snprintf(out->items[0].name, sizeof(out->items[0].name), "FreeShop");
    snprintf(out->items[0].base_url, sizeof(out->items[0].base_url), "%s", CATALOG_BASE_URL);
    out->items[0].enabled = true;
    out->items[0].hidden = true;
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

    size_t n = json_array_size(arr);
    for (size_t i = 0; i < n && out->count < SOURCES_MAX; i++) {
        const json_t *item = json_array_get(arr, i);
        const json_t *name = json_object_get(item, "name");
        const json_t *url = json_object_get(item, "baseUrl");
        const json_t *enabled = json_object_get(item, "enabled");
        const json_t *hidden = json_object_get(item, "hidden");
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
        out->count++;
    }

    json_decref(root);

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
        json_array_append_new(arr, item);
    }
    json_object_set_new(root, "sources", arr);

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
