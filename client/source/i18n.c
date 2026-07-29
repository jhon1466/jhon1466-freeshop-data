#include "i18n.h"

#include <switch.h>

typedef enum {
    LANG_ES = 0,
    LANG_EN,
    LANG_COUNT,
} Lang;

static Lang g_lang = LANG_ES;

void i18n_init(void) {
    g_lang = LANG_ES; // default/fallback - this project's home audience

    if (R_FAILED(setInitialize())) return;

    u64 code = 0;
    SetLanguage lang = SetLanguage_ENUS;
    if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &lang))) {
        // Spanish stays Spanish (SetLanguage_ES and the Latin American
        // variant both map here); everything else - English, but also
        // French/German/Japanese/etc., which this app doesn't have strings
        // for yet - falls back to English rather than Spanish, since a
        // console set to any of those is far more likely to have a user who
        // reads English than one who reads Spanish.
        g_lang = (lang == SetLanguage_ES || lang == SetLanguage_ES419) ? LANG_ES : LANG_EN;
    }

    setExit();
}

// {Spanish, English}, indexed by StrId - see i18n.h for what each id is and
// where its format placeholders (if any) come from.
static const char *const kStrings[STR_COUNT][LANG_COUNT] = {
    // ---- ui_list.c ----
    [STR_LIST_SORT_CATEGORY]              = { "Categoría", "Category" },
    [STR_LIST_SORT_VERSION]                = { "Versión", "Version" },
    [STR_LIST_SORT_TITLE]                  = { "Título", "Title" },
    [STR_LIST_EMPTY_CATALOG]               = { "(el catálogo está vacío)", "(the catalog is empty)" },
    [STR_LIST_EMPTY_SEARCH_IN_CATEGORY]    = { "(sin resultados para esta búsqueda en esta categoría)",
                                                "(no results for this search in this category)" },
    [STR_LIST_EMPTY_SEARCH]                = { "(sin resultados para esta búsqueda)", "(no results for this search)" },
    [STR_LIST_EMPTY_CATEGORY]              = { "(sin apps en esta categoría)", "(no apps in this category)" },
    [STR_LIST_STORAGE_UNAVAILABLE]         = { "no disponible", "unavailable" },
    [STR_LIST_FREE_TEMPLATE]               = { "%s libres", "%s free" },
    [STR_LIST_NO_CONNECTION]               = { "Sin conexión", "No connection" },
    [STR_LIST_NO_CATEGORIES]               = { "(sin categorías)", "(no categories)" },
    [STR_LIST_SEARCH_KBD_HEADER]           = { "Buscar en el catálogo", "Search the catalog" },
    [STR_LIST_SEARCH_KBD_GUIDE]            = { "Título del juego/app (vacío para quitar el filtro)",
                                                "Game/app title (empty to clear the filter)" },
    [STR_LIST_HEADER_SEARCHING_TEMPLATE]   = { "- Catálogo - buscando \"%s\"", "- Catalog - searching \"%s\"" },
    [STR_LIST_HEADER_CATALOG]              = { "- Catálogo", "- Catalog" },
    [STR_LIST_SD_CARD]                     = { "Tarjeta SD", "SD Card" },
    [STR_LIST_COL_NAME]                    = { "Nombre", "Name" },
    [STR_LIST_COL_TYPE]                    = { "Tipo", "Type" },
    [STR_LIST_COL_VERSION]                 = { "Versión", "Version" },
    [STR_LIST_COL_CATEGORY]                = { "Categoría", "Category" },
    [STR_LIST_COL_SIZE]                    = { "Tamaño", "Size" },
    [STR_LIST_FOOTER1_TEMPLATE]            = { "D-Pad: navegar    A: instalar    ZL/ZR: categoría    "
                                                "Y: vista %s    X: ordenar (%s)",
                                                "D-Pad: navigate    A: install    ZL/ZR: category    "
                                                "Y: view %s    X: sort (%s)" },
    [STR_LIST_VIEW_GRID]                   = { "cuadrícula", "grid" },
    [STR_LIST_VIEW_LIST]                   = { "lista", "list" },
    [STR_LIST_FOOTER2_QUEUE_TEMPLATE]      = { "R: buscar%s    -: fuentes    L: acerca de    Stick R: explorador    "
                                                "Stick L: recargar    +: cola (%d)    B: salir",
                                                "R: search%s    -: sources    L: about    Right stick: explorer    "
                                                "Left stick: reload    +: queue (%d)    B: exit" },
    [STR_LIST_FOOTER2_TEMPLATE]            = { "R: buscar%s    -: fuentes    L: acerca de    Stick R: explorador    "
                                                "Stick L: recargar    +: cola    B: salir",
                                                "R: search%s    -: sources    L: about    Right stick: explorer    "
                                                "Left stick: reload    +: queue    B: exit" },
    [STR_LIST_SEARCH_ACTIVE_SUFFIX]        = { " (activa)", " (active)" },

    // ---- ui_detail.c ----
    [STR_DETAIL_UNKNOWN_SIZE]              = { "tamaño desconocido", "unknown size" },
    [STR_DETAIL_HEADER_TEMPLATE]           = { "por %s   -   v%s   -   %s", "by %s   -   v%s   -   %s" },
    [STR_DETAIL_QUEUED_BADGE]              = { "En cola", "Queued" },
    [STR_DETAIL_NSZ_APPLET_HINT]           = { "NSZ: instálalo en modo applet (abre FreeShop desde el álbum)",
                                                "NSZ: install it in applet mode (open FreeShop from the Album)" },
    [STR_DETAIL_DLC_SECTION_TEMPLATE]      = { "DLC y actualizaciones (%d)", "DLC and updates (%d)" },
    [STR_DETAIL_HINT_DLC_FOCUS]            = { "Arriba/Abajo: elegir    A: instalar seleccionado    B: volver al juego",
                                                "Up/Down: choose    A: install selected    B: back to game" },
    [STR_DETAIL_HINT_NATIVE_WITH_DLC_TEMPLATE] = { "A: instalar    X: instalar vía DBI    "
                                                    "Y: ver DLC/actualizaciones (%d)    B: volver",
                                                    "A: install    X: install via DBI    "
                                                    "Y: view DLC/updates (%d)    B: back" },
    [STR_DETAIL_HINT_NATIVE]               = { "A: instalar    X: instalar vía DBI    B: volver",
                                                "A: install    X: install via DBI    B: back" },
    [STR_DETAIL_HINT_DLC_TEMPLATE]         = { "A: instalar    Y: ver DLC/actualizaciones (%d)    B: volver",
                                                "A: install    Y: view DLC/updates (%d)    B: back" },
    [STR_DETAIL_HINT_PLAIN]                = { "A: instalar    B: volver", "A: install    B: back" },
    [STR_DETAIL_QUEUE_REMOVE_HINT]         = { "+: quitar de la cola", "+: remove from queue" },
    [STR_DETAIL_QUEUE_ADD_HINT]            = { "+: agregar a la cola", "+: add to queue" },

    // ---- ui_sources.c ----
    [STR_SOURCES_NEW_URL_HEADER]           = { "Nueva fuente - dirección del servidor", "New source - server address" },
    [STR_SOURCES_NEW_URL_GUIDE]            = { "Ej: http://192.168.1.10:8080", "E.g.: http://192.168.1.10:8080" },
    [STR_SOURCES_NEW_NAME_HEADER]          = { "Nueva fuente - nombre (opcional)", "New source - name (optional)" },
    [STR_SOURCES_NEW_NAME_GUIDE]           = { "Cómo se muestra en la lista", "How it's shown in the list" },
    [STR_SOURCES_ADDED_FOUND_TEMPLATE]     = { "Fuente agregada: se encontraron %d app%s.",
                                                "Source added: found %d app%s." },
    [STR_SOURCES_ADDED_ERROR_TEMPLATE]     = { "Fuente agregada, pero no se pudo leer el catálogo:\n%s",
                                                "Source added, but its catalog couldn't be read:\n%s" },
    [STR_SOURCES_TITLE]                    = { "Fuentes", "Sources" },
    [STR_SOURCES_NONE_CONFIGURED]          = { "(sin fuentes configuradas)", "(no sources configured)" },
    [STR_SOURCES_ACTIVE]                   = { "[activa]", "[active]" },
    [STR_SOURCES_INACTIVE]                 = { "[inactiva]", "[inactive]" },
    [STR_SOURCES_FOOTER]                   = { "A: activar/desactivar    Y: agregar    X: eliminar    B: volver",
                                                "A: enable/disable    Y: add    X: remove    B: back" },

    // ---- ui_about.c ----
    [STR_ABOUT_HEADER]                     = { "- Acerca de", "- About" },
    [STR_ABOUT_VERSION_TEMPLATE]           = { "Versión %s", "Version %s" },
    [STR_ABOUT_DESCRIPTION]                = { "Catálogo de homebrew para Nintendo Switch - lista, descarga e "
                                                "instala juegos, ports, DLC/actualizaciones y updates de la propia "
                                                "app, todo desde aquí mismo.",
                                                "A homebrew catalog for Nintendo Switch - browse, download, and "
                                                "install games, ports, DLC/updates, and updates for the app "
                                                "itself, all from right here." },
    [STR_ABOUT_DONATE_QUESTION]            = { "¿Te sirvió FreeShop?", "Found FreeShop useful?" },
    [STR_ABOUT_DONATE_TEXT]                = { "Este proyecto lo mantengo en mi tiempo libre, sin nada a cambio - "
                                                "si te ha gustado y quieres ayudarme a seguir dedicándole horas (y "
                                                "café), cualquier aporte por PayPal se agradece muchísimo. No es "
                                                "obligatorio, pero sí que se siente. ¡Gracias por usar la app!",
                                                "I maintain this project in my spare time, for nothing in return - "
                                                "if you've enjoyed it and want to help me keep putting hours (and "
                                                "coffee) into it, any contribution via PayPal is deeply "
                                                "appreciated. It's never required, but it's always felt. Thanks "
                                                "for using the app!" },
    [STR_ABOUT_DONATE_SCAN]                = { "Escanea el QR o dona por PayPal a:",
                                                "Scan the QR code or donate via PayPal to:" },
    [STR_ABOUT_FOOTER]                     = { "B/+: volver", "B/+: back" },
    [STR_ABOUT_FOOTER_TOGGLES]             = { "X: efectos (%s)    Y: sonido (%s)    B/+: volver",
                                                "X: effects (%s)    Y: sound (%s)    B/+: back" },
    [STR_ON]                               = { "activado", "on" },
    [STR_OFF]                              = { "desactivado", "off" },

    // ---- ui_queue.c ----
    [STR_QUEUE_STATUS_DOWNLOADING]         = { "Descargando...", "Downloading..." },
    [STR_QUEUE_STATUS_INSTALLED]           = { "Instalado", "Installed" },
    [STR_QUEUE_STATUS_ERROR]               = { "Error", "Error" },
    [STR_QUEUE_STATUS_WAITING]             = { "En espera", "Waiting" },
    [STR_QUEUE_TITLE]                      = { "Cola de descargas", "Download queue" },
    [STR_QUEUE_PHASE_INSTALLING]           = { "Instalando", "Installing" },
    [STR_QUEUE_PHASE_DOWNLOADING]          = { "Descargando", "Downloading" },
    [STR_QUEUE_ITEM_OF_TEMPLATE]           = { "%s %d de %d: %s", "%s %d of %d: %s" },
    [STR_QUEUE_CANCEL_HINT]                = { "B: cancelar", "B: cancel" },
    [STR_QUEUE_CANCELED_TEMPLATE]          = { "Cancelado - %d de %d instalados.", "Canceled - %d of %d installed." },
    [STR_QUEUE_DONE_TEMPLATE]              = { "Terminado - %d de %d instalados.", "Done - %d of %d installed." },
    [STR_QUEUE_RESULTS_HINT]               = { "A/B/+: volver", "A/B/+: back" },
    [STR_QUEUE_TITLE_COUNT_TEMPLATE]       = { "Cola de descargas (%d)", "Download queue (%d)" },
    [STR_QUEUE_EMPTY_LINE1]                = { "No hay nada en la cola.", "The queue is empty." },
    [STR_QUEUE_EMPTY_LINE2]                = { "Entra a una app y presiona + para agregarla a la cola.",
                                                "Open an app and press + to add it to the queue." },
    [STR_QUEUE_FOOTER_WITH_ITEMS]          = { "D-Pad/Stick: navegar    A: empezar    X: quitar    B/+: volver",
                                                "D-Pad/Stick: navigate    A: start    X: remove    B/+: back" },
    [STR_QUEUE_FOOTER_EMPTY]               = { "B/+: volver", "B/+: back" },

    // ---- main.c ----
    [STR_MAIN_CATALOG_LOAD_ERROR_TEMPLATE] = { "No se pudo cargar el catálogo:\n%s\n\nPuedes seguir usando el "
                                                "explorador de archivos y la limpieza sin conexión - vuelve a "
                                                "intentar el catálogo con el stick L o desde Fuentes.",
                                                "Couldn't load the catalog:\n%s\n\nYou can still use the file "
                                                "explorer and the cleanup tool offline - retry the catalog with "
                                                "the left stick or from Sources." },
    [STR_MAIN_CATALOG_RELOAD_KEEP_OLD_TEMPLATE] = { "No se pudo actualizar el catálogo:\n%s\n\nSigues viendo la "
                                                     "versión anterior.",
                                                     "Couldn't refresh the catalog:\n%s\n\nStill showing the "
                                                     "previous version." },
};

const char *tr(StrId id) {
    if (id < 0 || id >= STR_COUNT) return "";
    return kStrings[id][g_lang];
}
