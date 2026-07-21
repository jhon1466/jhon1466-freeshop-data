#pragma once
#include "app_entry.h"

typedef enum {
    CATALOG_OK = 0,
    CATALOG_ERR_NETWORK,
    CATALOG_ERR_PARSE,
    CATALOG_ERR_SCHEMA_VERSION,
} CatalogResult;

// Fetches "<base_url><CATALOG_API_PATH>", parses it, and allocates
// *out_entries (caller must call catalog_free). On any non-OK result,
// *out_entries is left NULL and err_buf holds a human-readable reason.
CatalogResult catalog_fetch(const char *base_url, AppEntry **out_entries, int *out_count,
                             char *err_buf, size_t err_buf_size);

void catalog_free(AppEntry *entries);
