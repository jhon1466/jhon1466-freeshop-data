#pragma once
#include <stddef.h>

#define APP_ENTRY_ID_MAX 64
#define APP_ENTRY_TITLE_MAX 128
#define APP_ENTRY_AUTHOR_MAX 128
#define APP_ENTRY_CATEGORY_MAX 64
#define APP_ENTRY_DESC_MAX 256
#define APP_ENTRY_LONGDESC_MAX 1024
#define APP_ENTRY_VERSION_MAX 32
#define APP_ENTRY_URL_MAX 512
#define APP_ENTRY_SHA256_LEN 64
#define APP_ENTRY_FILENAME_MAX 128

// Mirrors AppEntry from shared/catalog.schema.json. Fixed-size buffers are
// used instead of heap-allocated strings so ownership/lifetime never has to
// be reasoned about beyond the array itself (see catalog_free).
typedef struct {
    char id[APP_ENTRY_ID_MAX];
    char title[APP_ENTRY_TITLE_MAX];
    char author[APP_ENTRY_AUTHOR_MAX];
    char category[APP_ENTRY_CATEGORY_MAX];
    char description[APP_ENTRY_DESC_MAX];
    char long_description[APP_ENTRY_LONGDESC_MAX];
    char version[APP_ENTRY_VERSION_MAX];
    char icon_url[APP_ENTRY_URL_MAX];
    char download_url[APP_ENTRY_URL_MAX];
    long file_size;
    char sha256[APP_ENTRY_SHA256_LEN + 1];
    char nro_filename[APP_ENTRY_FILENAME_MAX];
} AppEntry;
