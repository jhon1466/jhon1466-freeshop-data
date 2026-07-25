#include "catalog.h"
#include "../config.h"
#include "../net/http.h"

#include <ctype.h>
#include <jansson.h>
#include <stdint.h>
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

// Fetches and parses the catalog document from `url` exactly (no path
// composition here - the caller decides that) - factored out of
// catalog_fetch() so it can be tried against more than one candidate path.
static CatalogResult fetch_and_parse(const char *url, const char *base_url,
                                      AppEntry **out_entries, int *out_count,
                                      char *err_buf, size_t err_buf_size) {
    *out_entries = NULL;
    *out_count = 0;

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
        } else if (strcmp(file_type_str, "nsz") == 0) {
            e->file_type = APP_FILE_TYPE_NSZ;
        } else {
            e->file_type = APP_FILE_TYPE_NRO;
        }

        copy_str_field(app_json, "parentId", e->parent_id, sizeof(e->parent_id));
        copy_str_field(app_json, "contentType", e->content_type, sizeof(e->content_type));

        // Stamped as the *source's* base_url (not the URL actually fetched,
        // which may have been the "/api/apps" fallback below) - installs
        // resolve relative downloadUrls against this, and a relative URL in
        // the catalog is always meant relative to the source itself.
        snprintf(e->source_base_url, sizeof(e->source_base_url), "%s", base_url);
    }

    json_decref(root);

    *out_entries = entries;
    *out_count = (int)count;
    return CATALOG_OK;
}

// Recognizes the handful of installable extensions this client understands
// by filename suffix (case-insensitive) - used only by
// try_fetch_raw_directory() below, where there's no catalog.json to say
// what a file is, just its name. "port" (.zip) is deliberately not included:
// a bare zip could be anything, so guessing would misfile it as installable
// when it isn't. .nsz entries only ever get installed via the DBI hand-off
// (see APP_FILE_TYPE_NSZ in app_entry.h) - they're still recognized here so
// a raw-folder source works the same way a JSON catalog's "fileType": "nsz"
// already does.
static bool file_type_from_ext(const char *name, AppFileType *out_type) {
    size_t len = strlen(name);
    if (len >= 4 && strcasecmp(name + len - 4, ".nro") == 0) { *out_type = APP_FILE_TYPE_NRO; return true; }
    if (len >= 4 && strcasecmp(name + len - 4, ".nsp") == 0) { *out_type = APP_FILE_TYPE_NSP; return true; }
    if (len >= 4 && strcasecmp(name + len - 4, ".xci") == 0) { *out_type = APP_FILE_TYPE_XCI; return true; }
    if (len >= 4 && strcasecmp(name + len - 4, ".nsz") == 0) { *out_type = APP_FILE_TYPE_NSZ; return true; }
    return false;
}

// Decodes %XX percent-escapes in place (directory listings percent-encode
// spaces and other punctuation in filenames). Deliberately does NOT treat
// '+' as a space - that's an HTML-form convention, not a URL-path one, and
// would mangle any filename that genuinely contains a '+' (e.g. "C++.nro").
static void url_decode(const char *src, char *dest, size_t dest_size) {
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dest_size; si++) {
        if (src[si] == '%' && isxdigit((unsigned char)src[si + 1]) && isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            dest[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dest[di++] = src[si];
        }
    }
    dest[di] = '\0';
}

// Tolerant scan for the next href="..." (or href='...') attribute value
// starting at/after `html`. Not a real HTML parser - but directory-listing
// pages (Apache/nginx autoindex, `python -m http.server`, most NAS/router
// file browsers) are simple enough that this holds up in practice, and any
// href we don't care about (sort-order links, "Parent Directory", a CSS
// file) is filtered out downstream by file_type_from_ext() instead of here.
// Returns a cursor to resume scanning from, or NULL once no more hrefs are
// found.
static const char *find_next_href(const char *html, char *out, size_t out_size) {
    const char *p = html;
    while ((p = strstr(p, "href")) != NULL) {
        p += 4;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        char quote = *p;
        if (quote != '"' && quote != '\'') continue;
        p++;
        const char *end = strchr(p, quote);
        if (!end) return NULL;
        size_t len = (size_t)(end - p);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, p, len);
        out[len] = '\0';
        return end + 1;
    }
    return NULL;
}

// Cap on how many files a single directory-listing source can contribute -
// generous for what's meant to be someone's personal folder of homebrew,
// and bounds the allocation below without needing a growable array.
#define RAW_DIR_MAX_ENTRIES 512

// Last-resort fallback when a source has neither a catalog.json nor a
// /api/apps: just a plain folder of raw install files behind an HTTP
// directory listing (Apache/nginx autoindex, `python -m http.server`, a
// NAS's built-in browser, etc.) - this is what users actually meant by
// "agregar mi propia fuente": point at a folder of .nro/.nsp/.xci with zero
// setup, no manifest required. Titles are just the (decoded) filename
// without its extension, generic category/author/version, no icon,
// description, or sha256 - installs still work fine without those (sha256
// check is already optional everywhere, see install.c), it's purely a
// presentation tradeoff for needing nothing on the hosting side.
static CatalogResult try_fetch_raw_directory(const char *base_url, AppEntry **out_entries,
                                              int *out_count, char *err_buf, size_t err_buf_size) {
    *out_entries = NULL;
    *out_count = 0;

    char *raw = NULL;
    size_t raw_len = 0;
    HttpResult hres = http_get(base_url, &raw, &raw_len, err_buf, err_buf_size);
    if (hres != HTTP_OK) {
        return CATALOG_ERR_NETWORK;
    }

    AppEntry *entries = (AppEntry *)calloc(RAW_DIR_MAX_ENTRIES, sizeof(AppEntry));
    if (!entries) {
        free(raw);
        if (err_buf) snprintf(err_buf, err_buf_size, "memoria insuficiente para las entradas del catálogo");
        return CATALOG_ERR_PARSE;
    }

    // scheme://host[:port] prefix of base_url, used to resolve any
    // site-absolute href (e.g. "/files/x.nsp") the listing might emit.
    char origin[256] = "";
    const char *scheme_end = strstr(base_url, "://");
    if (scheme_end) {
        const char *host_start = scheme_end + 3;
        const char *host_end = strchr(host_start, '/');
        size_t origin_len = host_end ? (size_t)(host_end - base_url) : strlen(base_url);
        if (origin_len >= sizeof(origin)) origin_len = sizeof(origin) - 1;
        memcpy(origin, base_url, origin_len);
        origin[origin_len] = '\0';
    }

    size_t base_len = strlen(base_url);
    bool base_has_slash = base_len > 0 && base_url[base_len - 1] == '/';

    int n = 0;
    char href[512];
    const char *cursor = raw;
    while (n < RAW_DIR_MAX_ENTRIES && (cursor = find_next_href(cursor, href, sizeof(href))) != NULL) {
        AppFileType ftype;
        if (!file_type_from_ext(href, &ftype)) continue;

        // basename - strip any directory components the href might contain.
        const char *slash = strrchr(href, '/');
        const char *fname = slash ? slash + 1 : href;

        char decoded[APP_ENTRY_FILENAME_MAX];
        url_decode(fname, decoded, sizeof(decoded));

        // Many directory-listing pages emit more than one href for the same
        // file (an icon link next to the name link, a sort-by-name header
        // that happens to repeat the current entry, etc.) - skip anything
        // whose filename we've already recorded rather than trying to
        // out-guess every listing generator's markup.
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(entries[j].filename, decoded) == 0) { dup = true; break; }
        }
        if (dup) continue;

        AppEntry *e = &entries[n];

        if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) {
            snprintf(e->download_url, sizeof(e->download_url), "%s", href);
        } else if (href[0] == '/') {
            snprintf(e->download_url, sizeof(e->download_url), "%s%s", origin, href);
        } else {
            snprintf(e->download_url, sizeof(e->download_url), "%s%s%s", base_url,
                     base_has_slash ? "" : "/", href);
        }

        snprintf(e->filename, sizeof(e->filename), "%s", decoded);

        // id: decoded filename with path-unsafe characters swapped out -
        // this doubles as a directory name under sdmc:/switch/ for
        // nro/port installs (see install_app.c/install_port.c).
        snprintf(e->id, sizeof(e->id), "%s", decoded);
        for (char *p = e->id; *p; p++) {
            if (*p == ' ' || *p == '/' || *p == '\\') *p = '_';
        }

        // title: same, minus the extension.
        snprintf(e->title, sizeof(e->title), "%s", decoded);
        char *dot = strrchr(e->title, '.');
        if (dot) *dot = '\0';

        snprintf(e->category, sizeof(e->category), "Sin categoría");
        snprintf(e->author, sizeof(e->author), "-");
        snprintf(e->version, sizeof(e->version), "-");
        e->file_type = ftype;
        e->file_size = 0;
        snprintf(e->source_base_url, sizeof(e->source_base_url), "%s", base_url);

        // The listing page itself never states file sizes, so ask the
        // server directly (HEAD, no body) - one extra request per file, but
        // it only happens on catalog load, and this whole source type is
        // aimed at small personal folders, not thousand-entry catalogs.
        // Left at 0 (meaning "unknown", already handled everywhere else -
        // see install.c) if the server doesn't answer or omits
        // Content-Length, rather than failing the whole source over it.
        int64_t head_size = -1;
        if (http_head_content_length(e->download_url, &head_size, NULL, 0) == HTTP_OK && head_size > 0) {
            e->file_size = (long)head_size;
        }

        n++;
    }

    free(raw);

    if (n == 0) {
        free(entries);
        if (err_buf) snprintf(err_buf, err_buf_size, "no se encontraron archivos .nro/.nsp/.xci en el listado");
        return CATALOG_ERR_PARSE;
    }

    AppEntry *shrunk = (AppEntry *)realloc(entries, (size_t)n * sizeof(AppEntry));
    *out_entries = shrunk ? shrunk : entries;
    *out_count = n;
    return CATALOG_OK;
}

CatalogResult catalog_fetch(const char *base_url, AppEntry **out_entries, int *out_count,
                             char *err_buf, size_t err_buf_size) {
    char url[600];
    snprintf(url, sizeof(url), "%s%s", base_url, CATALOG_API_PATH);
    CatalogResult r = fetch_and_parse(url, base_url, out_entries, out_count, err_buf, err_buf_size);
    if (r == CATALOG_OK) return r;

    // CATALOG_API_PATH ("/data/catalog.json") is what a static-file host
    // (the default GitHub data repo) serves the catalog at - but a
    // self-hosted instance of this project's own server (exactly what
    // "Fuentes > agregar" invites users to point at, e.g.
    // "http://192.168.1.10:8080") exposes the identical document at
    // "/api/apps" instead (see server/src/routes/apps.ts - same
    // schemaVersion+apps shape either way). Without this fallback, adding
    // that kind of source silently did nothing: the wrong path 404s, the
    // response isn't valid JSON, catalog_fetch fails, and
    // fetch_merged_catalog() in main.c skips a failed source rather than
    // erroring out the whole load - so nothing from it ever appeared, with
    // no indication why.
    snprintf(url, sizeof(url), "%s/api/apps", base_url);
    r = fetch_and_parse(url, base_url, out_entries, out_count, err_buf, err_buf_size);
    if (r == CATALOG_OK) return r;

    // Neither of the above worked - last resort, treat base_url itself as a
    // plain folder of raw files behind an HTTP directory listing (see
    // try_fetch_raw_directory). This is the case users actually asked
    // about: "las fuentes que quieren agregar son folder donde estan los
    // archivos en bruto... no son json".
    return try_fetch_raw_directory(base_url, out_entries, out_count, err_buf, err_buf_size);
}

void catalog_free(AppEntry *entries) {
    free(entries);
}
