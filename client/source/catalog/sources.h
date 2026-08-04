#pragma once
#include <stdbool.h>
#include <stddef.h>

#define SOURCE_NAME_MAX 64
#define SOURCE_URL_MAX 256
#define SOURCES_MAX 8

// SOURCE_KIND_STANDARD: base_url is a FreeShop-shaped catalog root (tried
// as <base_url>/data/catalog.json, then <base_url>/api/apps, then a raw
// directory listing - see catalog.c's catalog_fetch).
// SOURCE_KIND_TORRENT_CATALOG: base_url is the direct URL of a
// switch_games.json-shaped array (title/magnet/size/year/genre/...) -
// fetched as-is, no path appended. See catalog.c's catalog_fetch_torrent_json.
// Entries from it carry AppEntry.via_torrent and download through
// install_torrent.c instead of a native HTTP installer.
typedef enum {
    SOURCE_KIND_STANDARD = 0,
    SOURCE_KIND_TORRENT_CATALOG,
} SourceKind;

typedef struct {
    char name[SOURCE_NAME_MAX];
    char base_url[SOURCE_URL_MAX];
    bool enabled;
    // True only for the bootstrap default (see sources_load) - the "Fuentes"
    // screen (ui_sources.c) never lists, selects, toggles, or deletes a
    // hidden source, so the operator's own server URL isn't exposed to
    // users there. Sources users add themselves are never hidden.
    bool hidden;
    SourceKind kind;
} CatalogSource;

typedef struct {
    CatalogSource items[SOURCES_MAX];
    int count;
    // One-shot latch: sources_load has already offered (added, once) the
    // built-in torrent-catalog source to this list. Kept separate from
    // "is a SOURCE_KIND_TORRENT_CATALOG entry currently present" so a user
    // who removes it via "Fuentes" doesn't get it silently re-added on the
    // next load.
    bool torrent_source_offered;
} SourceList;

// Loads sdmc:/switch/freeshop/sources.json into `out`. If the file is
// missing, unreadable, or fails to parse, initializes `out` with a single
// default source (name "FreeShop", baseUrl = CATALOG_BASE_URL from
// config.h) and writes it out immediately, so there is always at least one
// usable, persisted source to fall back to.
void sources_load(SourceList *out);

// Overwrites sdmc:/switch/freeshop/sources.json with `list`. Returns false
// on failure (e.g. read-only SD) - caller's in-memory list is unaffected
// either way.
bool sources_save(const SourceList *list);
