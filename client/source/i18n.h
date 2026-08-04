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
    STR_LIST_HEADER_CATALOG_COUNT_TEMPLATE, // "- Catálogo (%d juegos)" - %d = total root game count
    STR_LIST_SHELF_RECENT,   // "Recién agregados" - the year-sorted shelf on the catalog home screen
    STR_LIST_SHELF_SEE_ALL,  // "Ver todo" - the trailing virtual card on every shelf
    STR_LIST_SHELF_FEATURED, // "Destacado" - the hero banner's kicker label
    STR_LIST_SD_CARD,
    STR_LIST_COL_NAME,
    STR_LIST_COL_TYPE,
    STR_LIST_COL_VERSION,
    STR_LIST_COL_CATEGORY,
    STR_LIST_COL_SIZE,
    STR_LIST_VIEW_GRID,
    STR_LIST_VIEW_LIST,
    STR_LIST_SEARCH_ACTIVE_SUFFIX,

    // ---- ui_list.c (sidebar + footer button-chip hints) ----
    // Each footer line used to be one fully-templated string; it's now
    // composed at draw time from draw_footer_hint() calls (a little
    // bordered "key cap" box plus one of these as its label) so the
    // buttons read as chips instead of plain "A: " text - see
    // ui_show_list's footer rendering and the sidebar footer just below.
    STR_LIST_SIDEBAR_CATALOG,
    STR_LIST_SIDEBAR_EXPLORER,
    STR_LIST_SIDEBAR_QUEUE,
    STR_LIST_SIDEBAR_SAVES,
    STR_LIST_SIDEBAR_MTP,
    STR_LIST_SIDEBAR_FTP,
    STR_LIST_SIDEBAR_SOURCES,
    STR_LIST_SIDEBAR_ABOUT,
    STR_LIST_SIDEBAR_COLLAPSE,
    STR_LIST_SIDEBAR_EXPAND,
    STR_LIST_HINT_NAVIGATE,
    STR_LIST_HINT_INSTALL,
    STR_LIST_HINT_CATEGORY,
    STR_LIST_HINT_VIEW_TEMPLATE,          // "vista %s" - %s = view name
    STR_LIST_HINT_SORT_TEMPLATE,          // "ordenar (%s)" - %s = sort label
    STR_LIST_HINT_SEARCH_TEMPLATE,        // "buscar%s" - %s = search-active suffix
    STR_LIST_HINT_MENU,
    STR_LIST_HINT_PANEL_TEMPLATE,         // "%s panel" - %s = collapse/expand label
    STR_LIST_HINT_RELOAD,
    STR_LIST_HINT_EXIT,
    STR_LIST_HINT_OPEN,
    STR_LIST_HINT_BACK_TO_CATALOG,

    // ---- ui_detail.c (app detail screen) ----
    STR_DETAIL_UNKNOWN_SIZE,
    STR_DETAIL_HEADER_TEMPLATE,          // "por %s   -   v%s   -   %s" - author, version, size
    STR_DETAIL_HEADER_TEMPLATE_YEAR,     // "por %s   -   %s   -   %s" - author, year (no "v" prefix - it's not a version), size
    STR_DETAIL_QUEUED_BADGE,
    STR_DETAIL_NSZ_APPLET_HINT,
    STR_DETAIL_DLC_SECTION_TEMPLATE,     // "DLC y actualizaciones (%d)"
    STR_DETAIL_HINT_CHOOSE,
    STR_DETAIL_HINT_INSTALL_SELECTED,
    STR_DETAIL_HINT_BACK_TO_GAME,
    STR_DETAIL_HINT_INSTALL,
    STR_DETAIL_HINT_INSTALL_DBI,
    STR_DETAIL_HINT_DLC_TEMPLATE,         // "ver DLC/actualizaciones (%d)"
    STR_DETAIL_QUEUE_REMOVE_HINT,
    STR_DETAIL_QUEUE_ADD_HINT,
    STR_DETAIL_FACT_AUTHOR,   // "Autor" - facts-table row label
    STR_DETAIL_FACT_YEAR,     // "Año" - facts-table row label, torrent-catalog entries (see via_torrent)
    STR_DETAIL_INSTALL_BUTTON, // "INSTALAR" - the big install button, distinct from the footer hint text

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
    STR_SOURCES_HINT_TOGGLE,
    STR_SOURCES_HINT_ADD,
    STR_SOURCES_HINT_REMOVE,

    // ---- ui_about.c ----
    STR_ABOUT_HEADER,
    STR_ABOUT_VERSION_TEMPLATE,          // "Versión %s"
    STR_ABOUT_DESCRIPTION,
    STR_ABOUT_DONATE_QUESTION,
    STR_ABOUT_DONATE_TEXT,
    STR_ABOUT_DONATE_SCAN,
    STR_ABOUT_FOOTER,
    STR_ABOUT_HINT_EFFECTS_TEMPLATE,     // "efectos (%s)"
    STR_ABOUT_HINT_SOUND_TEMPLATE,       // "sonido (%s)"
    STR_ABOUT_HINT_BACK,
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
    STR_QUEUE_CANCELED_TEMPLATE,         // "Cancelado - %d de %d instalados."
    STR_QUEUE_DONE_TEMPLATE,             // "Terminado - %d de %d instalados."
    STR_QUEUE_TITLE_COUNT_TEMPLATE,      // "Cola de descargas (%d)"
    STR_QUEUE_EMPTY_LINE1,
    STR_QUEUE_EMPTY_LINE2,
    STR_QUEUE_HINT_NAVIGATE,
    STR_QUEUE_HINT_START,
    STR_QUEUE_HINT_REMOVE,

    // ---- main.c ----
    STR_MAIN_CATALOG_LOAD_ERROR_TEMPLATE, // "No se pudo cargar el catálogo:\n%s\n\n..."
    STR_MAIN_CATALOG_RELOAD_KEEP_OLD_TEMPLATE,

    // ---- ui_saves.c ----
    STR_SAVES_TITLE,
    STR_SAVES_SCANNING,
    STR_SAVES_NONE_FOUND,
    STR_SAVES_COUNT_TEMPLATE,            // "%d guardados"
    STR_SAVES_BACKUPS_TITLE,
    STR_SAVES_LIVE_SIZE_TEMPLATE,        // "Guardado actual: %s" - %s = formatted byte count
    STR_SAVES_BACKUP_COUNT_TEMPLATE,     // "%d backups"
    STR_SAVES_MOST_RECENT,
    STR_SAVES_NO_BACKUPS,
    STR_SAVES_HINT_NAVIGATE,
    STR_SAVES_HINT_VIEW_BACKUPS,
    STR_SAVES_HINT_BACKUP_NOW,
    STR_SAVES_HINT_RESTORE,
    STR_SAVES_HINT_DELETE,
    STR_SAVES_HINT_BACK,
    STR_SAVES_HINT_CANCEL,
    STR_SAVES_BACKING_UP,
    STR_SAVES_RESTORING,

    // ---- ui_mtp.c ----
    STR_MTP_TITLE,
    STR_MTP_WAITING_USB,
    STR_MTP_WAITING_HOST,
    STR_MTP_READY,
    STR_MTP_WAITING_USB_HELP,
    STR_MTP_WAITING_HOST_HELP,
    STR_MTP_HELP,
    STR_MTP_FORMATS,
    STR_MTP_RECEIVING,
    STR_MTP_INSTALLING_NOW,
    STR_MTP_SESSION_COUNT_TEMPLATE, // "%d instalados en esta sesión"
    STR_MTP_QUEUE_TITLE,
    STR_MTP_QUEUE_EMPTY,
    STR_MTP_HINT_EXIT,
    STR_MTP_START_FAILED_TEMPLATE,   // "No se pudo iniciar MTP:\n%s"

    // ---- ui_ftp.c ----
    STR_FTP_TITLE,
    STR_FTP_WAITING_NETWORK,
    STR_FTP_WAITING_NETWORK_HELP,
    STR_FTP_LISTENING_TEMPLATE, // "Listo - conectate a ftp://%s:5000 desde tu PC."
    STR_FTP_LISTENING_HELP,
    STR_FTP_CONNECTED_TEMPLATE, // "Conectado: %s"
    STR_FTP_CONNECTED_HELP,
    STR_FTP_FORMATS,
    STR_FTP_TRANSFERRING,
    STR_FTP_INSTALLING_NOW,
    STR_FTP_SESSION_COUNT_TEMPLATE, // "%d instalados en esta sesión"
    STR_FTP_QUEUE_TITLE,
    STR_FTP_QUEUE_EMPTY,
    STR_FTP_HINT_EXIT,
    STR_FTP_START_FAILED_TEMPLATE, // "No se pudo iniciar el servidor FTP:\n%s"
    STR_FTP_STATUS_UPLOADED,
    STR_FTP_STATUS_DOWNLOADED,
    STR_SAVES_BACKUP_NOW_CONFIRM_TEMPLATE, // "¿Crear un backup del guardado de \"%s\"?"
    STR_SAVES_BACKUP_DONE,
    STR_SAVES_BACKUP_FAILED_TEMPLATE,      // "No se pudo crear el backup:\n%s"
    STR_SAVES_RESTORE_CONFIRM_TEMPLATE,    // "¿Restaurar el backup \"%s\"?\n\n..." - %s = formatted date/time
    STR_SAVES_RESTORE_DONE,
    STR_SAVES_RESTORE_FAILED_TEMPLATE,     // "No se pudo restaurar el guardado:\n%s"
    STR_SAVES_DELETE_CONFIRM_TEMPLATE,     // "¿Eliminar el backup \"%s\"?\n\n..." - %s = formatted date/time

    STR_COUNT,
} StrId;

// Returns the string for `id` in whichever language i18n_init() picked.
// Every id is defined in both languages, so this never returns NULL/empty
// for a valid id.
const char *tr(StrId id);
