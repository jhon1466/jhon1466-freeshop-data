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

// Fetches `url` directly (no path composition - this IS the JSON document,
// unlike catalog_fetch's base_url) as a switch_games.json-shaped array
// (title/magnet/size/year/genre/developer/publisher/cover/screenshots/
// description/image_format) and maps it into AppEntry, one per magnet that
// both names a supported install format (NSP/XCI/NSZ - not NRO/PORT, which
// install_torrent.c doesn't handle yet) and carries a tracker
// magnet_parse() accepts. Every entry gets AppEntry.via_torrent set and
// AppEntry.download_url holding the raw magnet: URI. Used for
// SOURCE_KIND_TORRENT_CATALOG sources - see sources.h.
CatalogResult catalog_fetch_torrent_json(const char *url, AppEntry **out_entries, int *out_count,
                                          char *err_buf, size_t err_buf_size);

void catalog_free(AppEntry *entries);
