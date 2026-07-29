#include "ui_prefs.h"
#include "../config.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PREFS_DIR SWITCH_APPS_ROOT "/freeshop"
#define PREFS_PATH PREFS_DIR "/prefs.json"

// A hand-edited or corrupted file could be arbitrarily large - cap what
// we're willing to read into memory, same reasoning as sources.c.
#define PREFS_FILE_MAX_BYTES 4096

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

void ui_prefs_load(UiListPrefs *out) {
    memset(out, 0, sizeof(*out));

    size_t len = 0;
    char *buf = read_whole_file(PREFS_PATH, PREFS_FILE_MAX_BYTES, &len);
    if (!buf) return;

    json_error_t jerr;
    json_t *root = json_loadb(buf, len, 0, &jerr);
    free(buf);
    if (!root) return;

    const json_t *view_mode = json_object_get(root, "viewMode");
    if (json_is_integer(view_mode)) out->view_mode = (int)json_integer_value(view_mode);

    const json_t *sort_mode = json_object_get(root, "sortMode");
    if (json_is_integer(sort_mode)) out->sort_mode = (int)json_integer_value(sort_mode);

    const json_t *category_filter = json_object_get(root, "categoryFilter");
    if (json_is_string(category_filter)) {
        snprintf(out->category_filter, sizeof(out->category_filter), "%s", json_string_value(category_filter));
    }

    const json_t *effects_disabled = json_object_get(root, "effectsDisabled");
    if (json_is_boolean(effects_disabled)) out->effects_disabled = json_boolean_value(effects_disabled);

    const json_t *sound_disabled = json_object_get(root, "soundDisabled");
    if (json_is_boolean(sound_disabled)) out->sound_disabled = json_boolean_value(sound_disabled);

    json_decref(root);
}

void ui_prefs_save(const UiListPrefs *prefs) {
    mkdir(PREFS_DIR, 0777); // ignore EEXIST/any error - a real failure surfaces on fopen below

    json_t *root = json_object();
    json_object_set_new(root, "viewMode", json_integer(prefs->view_mode));
    json_object_set_new(root, "sortMode", json_integer(prefs->sort_mode));
    json_object_set_new(root, "categoryFilter", json_string(prefs->category_filter));
    json_object_set_new(root, "effectsDisabled", json_boolean(prefs->effects_disabled));
    json_object_set_new(root, "soundDisabled", json_boolean(prefs->sound_disabled));

    char *text = json_dumps(root, JSON_INDENT(2));
    json_decref(root);
    if (!text) return;

    FILE *fp = fopen(PREFS_PATH, "wb");
    if (fp) {
        fwrite(text, 1, strlen(text), fp);
        fclose(fp);
    }
    free(text);
}
