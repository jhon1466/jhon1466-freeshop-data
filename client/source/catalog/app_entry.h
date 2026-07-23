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

// "nro": homebrew app, downloaded and installed directly by install_app()
// into sdmc:/switch/<id>/. "nsp"/"xci": installable title, installed
// natively via NCM/ES/NS (see install_nsp_native.h/install_xci_native.h),
// with a manual DBI (https://github.com/rashevskyv/dbi) fallback. "port": a
// homebrew app that isn't a single .nro - it ships as a .zip containing the
// .nro plus whatever data files/subfolders it needs (e.g. a native port's
// asset pack) - install_port.h downloads and extracts it into
// sdmc:/switch/<id>/, same destination convention as a plain "nro".
typedef enum {
    APP_FILE_TYPE_NRO = 0,
    APP_FILE_TYPE_NSP,
    APP_FILE_TYPE_XCI,
    APP_FILE_TYPE_PORT,
} AppFileType;

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
    char filename[APP_ENTRY_FILENAME_MAX];
    AppFileType file_type;
    // Empty for a normal/base entry. If set, this is DLC/an update for the
    // base game with this id - main.c filters entries with a non-empty
    // parent_id out of the main list/grid, surfacing them instead in that
    // game's detail screen (see ui_detail.h).
    char parent_id[APP_ENTRY_ID_MAX];
    // Only meaningful when parent_id is set - "dlc" or "update", just a
    // label for this entry in its parent's list. Empty is fine (generic
    // label shown instead).
    char content_type[16];
    // Base URL of the source this entry was fetched from (stamped by
    // catalog_fetch, not part of the server's JSON) - installs must use
    // this instead of a single global base URL now that multiple sources
    // can be merged into one list. See sources.h.
    char source_base_url[APP_ENTRY_URL_MAX];
} AppEntry;
