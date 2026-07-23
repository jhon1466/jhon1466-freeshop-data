#include "catalog.h"
#include "../config.h"
#include "../net/http.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_str_field(const json_t *obj, const char *key, char *dest, size_t dest_size) {
    const json_t *item = json_object_get(obj, key);
    if (json_is_string(item)) {
        snprintf(dest, dest_size, "%s", json_string_value(item));
    } else {
        dest[0] = '\0';
    }
}

static long get_int_field(const json_t *obj, const char *key) {
    const json_t *item = json_object_get(obj, key);
    if (json_is_integer(item)) return (long)json_integer_value(item);
    if (json_is_real(item)) return (long)json_real_value(item);
    return 0;
}

CatalogResult catalog_fetch(const char *base_url, AppEntry **out_entries, int *out_count,
                             char *err_buf, size_t err_buf_size) {
    *out_entries = NULL;
    *out_count = 0;

    char url[600];
    snprintf(url, sizeof(url), "%s%s", base_url, CATALOG_API_PATH);

    char *raw = NULL;
    size_t raw_len = 0;
    HttpResult hres = http_get(url, &raw, &raw_len, err_buf, err_buf_size);
    if (hres != HTTP_OK) {
        return CATALOG_ERR_NETWORK;
    }

    json_error_t jerr;
    json_t *root = json_loadb(raw, raw_len, 0, &jerr);
    free(raw);
    if (!root) {
        if (err_buf) snprintf(err_buf, err_buf_size, "JSON del catálogo malformado: %s", jerr.text);
        return CATALOG_ERR_PARSE;
    }

    const json_t *schema_version = json_object_get(root, "schemaVersion");
    if (!json_is_integer(schema_version) || json_integer_value(schema_version) != 1) {
        json_decref(root);
        if (err_buf) snprintf(err_buf, err_buf_size, "schemaVersion del catálogo no soportada (se esperaba 1)");
        return CATALOG_ERR_SCHEMA_VERSION;
    }

    const json_t *apps = json_object_get(root, "apps");
    if (!json_is_array(apps)) {
        json_decref(root);
        if (err_buf) snprintf(err_buf, err_buf_size, "al JSON del catálogo le falta el arreglo \"apps\"");
        return CATALOG_ERR_PARSE;
    }

    size_t count = json_array_size(apps);
    AppEntry *entries = (AppEntry *)calloc(count, sizeof(AppEntry));
    if (!entries) {
        json_decref(root);
        if (err_buf) snprintf(err_buf, err_buf_size, "memoria insuficiente para las entradas del catálogo");
        return CATALOG_ERR_PARSE;
    }

    for (size_t i = 0; i < count; i++) {
        const json_t *app_json = json_array_get(apps, i);
        AppEntry *e = &entries[i];
        copy_str_field(app_json, "id", e->id, sizeof(e->id));
        copy_str_field(app_json, "title", e->title, sizeof(e->title));
        copy_str_field(app_json, "author", e->author, sizeof(e->author));
        copy_str_field(app_json, "category", e->category, sizeof(e->category));
        copy_str_field(app_json, "description", e->description, sizeof(e->description));
        copy_str_field(app_json, "longDescription", e->long_description, sizeof(e->long_description));
        copy_str_field(app_json, "version", e->version, sizeof(e->version));
        copy_str_field(app_json, "iconUrl", e->icon_url, sizeof(e->icon_url));
        copy_str_field(app_json, "downloadUrl", e->download_url, sizeof(e->download_url));
        copy_str_field(app_json, "sha256", e->sha256, sizeof(e->sha256));
        copy_str_field(app_json, "filename", e->filename, sizeof(e->filename));
        e->file_size = get_int_field(app_json, "fileSize");

        char file_type_str[8];
        copy_str_field(app_json, "fileType", file_type_str, sizeof(file_type_str));
        if (strcmp(file_type_str, "nsp") == 0) {
            e->file_type = APP_FILE_TYPE_NSP;
        } else if (strcmp(file_type_str, "xci") == 0) {
            e->file_type = APP_FILE_TYPE_XCI;
        } else if (strcmp(file_type_str, "port") == 0) {
            e->file_type = APP_FILE_TYPE_PORT;
        } else {
            e->file_type = APP_FILE_TYPE_NRO;
        }

        copy_str_field(app_json, "parentId", e->parent_id, sizeof(e->parent_id));
        copy_str_field(app_json, "contentType", e->content_type, sizeof(e->content_type));

        snprintf(e->source_base_url, sizeof(e->source_base_url), "%s", base_url);
    }

    json_decref(root);

    *out_entries = entries;
    *out_count = (int)count;
    return CATALOG_OK;
}

void catalog_free(AppEntry *entries) {
    free(entries);
}
