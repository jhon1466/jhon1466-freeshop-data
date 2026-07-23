#pragma once
#include <stdbool.h>
#include <stddef.h>

#define SOURCE_NAME_MAX 64
#define SOURCE_URL_MAX 256
#define SOURCES_MAX 8

typedef struct {
    char name[SOURCE_NAME_MAX];
    char base_url[SOURCE_URL_MAX];
    bool enabled;
    // True only for the bootstrap default (see sources_load) - the "Fuentes"
    // screen (ui_sources.c) never lists, selects, toggles, or deletes a
    // hidden source, so the operator's own server URL isn't exposed to
    // users there. Sources users add themselves are never hidden.
    bool hidden;
} CatalogSource;

typedef struct {
    CatalogSource items[SOURCES_MAX];
    int count;
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
