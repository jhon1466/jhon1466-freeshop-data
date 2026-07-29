#pragma once

// Minimal i18n layer: a flat enum of string ids plus a same-indexed {es, en}
// table in i18n.c, and a single tr(id) lookup that every screen calls
// instead of embedding a literal directly. Spanish is the fallback/default
// (this project's home audience) - English is the only other language
// translated so far, picked as the widest-reach second language a
// non-Spanish-speaking user is likely to have their console set to, and the
// one this project has the most confidence translating correctly without a
// native speaker to check it. Adding a third language later is just another
// table column plus another SetLanguage mapping in i18n_init() - nothing
// about call sites changes.
//
// Format strings (containing %s/%d/%.1f etc.) are still just strings here -
// tr() only supplies the localized template, the caller snprintf's it
// exactly as it always has. See each id's comment in i18n.c for its
// placeholders.

// Detects the console's system language (setGetSystemLanguage) and picks
// whichever of this app's translations is the closest match, defaulting to
// Spanish for anything not recognized. Call once at startup, before the
// first screen that calls tr().
void i18n_init(void);

typedef enum {
    // ---- ui_list.c (main catalog screen) ----
    STR_LIST_SORT_CATEGORY = 0,
    STR_LIST_SORT_VERSION,
    STR_LIST_SORT_TITLE,
    STR_LIST_EMPTY_CATALOG,
    STR_LIST_EMPTY_SEARCH_IN_CATEGORY,
    STR_LIST_EMPTY_SEARCH,
    STR_LIST_EMPTY_CATEGORY,
    STR_LIST_STORAGE_UNAVAILABLE,
    STR_LIST_FREE_TEMPLATE,              // "%s libres" - %s = a formatted byte count
    STR_LIST_NO_CONNECTION,
    STR_LIST_NO_CATEGORIES,
    STR_LIST_SEARCH_KBD_HEADER,
    STR_LIST_SEARCH_KBD_GUIDE,
    STR_LIST_HEADER_SEARCHING_TEMPLATE,  // "- Catálogo - buscando \"%s\"" - %s = the query
    STR_LIST_HEADER_CATALOG,
    STR_LIST_SD_CARD,
    STR_LIST_COL_NAME,
    STR_LIST_COL_TYPE,
    STR_LIST_COL_VERSION,
    STR_LIST_COL_CATEGORY,
    STR_LIST_COL_SIZE,
    STR_LIST_FOOTER1_TEMPLATE,           // "...Y: vista %s    X: ordenar (%s)" - %s = view name, sort label
    STR_LIST_VIEW_GRID,
    STR_LIST_VIEW_LIST,
    STR_LIST_FOOTER2_QUEUE_TEMPLATE,     // "...+: cola (%d)    B: salir" - %s = search-active suffix, %d = queue count
    STR_LIST_FOOTER2_TEMPLATE,           // same, no queue count
    STR_LIST_SEARCH_ACTIVE_SUFFIX,

    // ---- ui_detail.c (app detail screen) ----
    STR_DETAIL_UNKNOWN_SIZE,
    STR_DETAIL_HEADER_TEMPLATE,          // "por %s   -   v%s   -   %s" - author, version, size
    STR_DETAIL_QUEUED_BADGE,
    STR_DETAIL_NSZ_APPLET_HINT,
    STR_DETAIL_DLC_SECTION_TEMPLATE,     // "DLC y actualizaciones (%d)"
    STR_DETAIL_HINT_DLC_FOCUS,
    STR_DETAIL_HINT_NATIVE_WITH_DLC_TEMPLATE, // "...Y: ver DLC/actualizaciones (%d)..."
    STR_DETAIL_HINT_NATIVE,
    STR_DETAIL_HINT_DLC_TEMPLATE,
    STR_DETAIL_HINT_PLAIN,
    STR_DETAIL_QUEUE_REMOVE_HINT,
    STR_DETAIL_QUEUE_ADD_HINT,

    // ---- ui_sources.c ----
    STR_SOURCES_NEW_URL_HEADER,
    STR_SOURCES_NEW_URL_GUIDE,
    STR_SOURCES_NEW_NAME_HEADER,
    STR_SOURCES_NEW_NAME_GUIDE,
    STR_SOURCES_ADDED_FOUND_TEMPLATE,    // "Fuente agregada: se encontraron %d app%s."
    STR_SOURCES_ADDED_ERROR_TEMPLATE,    // "Fuente agregada, pero no se pudo leer el catálogo:\n%s"
    STR_SOURCES_TITLE,
    STR_SOURCES_NONE_CONFIGURED,
    STR_SOURCES_ACTIVE,
    STR_SOURCES_INACTIVE,
    STR_SOURCES_FOOTER,

    // ---- ui_about.c ----
    STR_ABOUT_HEADER,
    STR_ABOUT_VERSION_TEMPLATE,          // "Versión %s"
    STR_ABOUT_DESCRIPTION,
    STR_ABOUT_DONATE_QUESTION,
    STR_ABOUT_DONATE_TEXT,
    STR_ABOUT_DONATE_SCAN,
    STR_ABOUT_FOOTER,
    STR_ABOUT_FOOTER_TOGGLES,            // "X: efectos (%s)    Y: sonido (%s)    B/+: volver"
    STR_ON,
    STR_OFF,

    // ---- ui_queue.c ----
    STR_QUEUE_STATUS_DOWNLOADING,
    STR_QUEUE_STATUS_INSTALLED,
    STR_QUEUE_STATUS_ERROR,
    STR_QUEUE_STATUS_WAITING,
    STR_QUEUE_TITLE,
    STR_QUEUE_PHASE_INSTALLING,
    STR_QUEUE_PHASE_DOWNLOADING,
    STR_QUEUE_ITEM_OF_TEMPLATE,          // "%s %d de %d: %s" - phase, index, total, title
    STR_QUEUE_CANCEL_HINT,
    STR_QUEUE_CANCELED_TEMPLATE,         // "Cancelado - %d de %d instalados."
    STR_QUEUE_DONE_TEMPLATE,             // "Terminado - %d de %d instalados."
    STR_QUEUE_RESULTS_HINT,
    STR_QUEUE_TITLE_COUNT_TEMPLATE,      // "Cola de descargas (%d)"
    STR_QUEUE_EMPTY_LINE1,
    STR_QUEUE_EMPTY_LINE2,
    STR_QUEUE_FOOTER_WITH_ITEMS,
    STR_QUEUE_FOOTER_EMPTY,

    // ---- main.c ----
    STR_MAIN_CATALOG_LOAD_ERROR_TEMPLATE, // "No se pudo cargar el catálogo:\n%s\n\n..."
    STR_MAIN_CATALOG_RELOAD_KEEP_OLD_TEMPLATE,

    STR_COUNT,
} StrId;

// Returns the string for `id` in whichever language i18n_init() picked.
// Every id is defined in both languages, so this never returns NULL/empty
// for a valid id.
const char *tr(StrId id);
