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
    [STR_LIST_HEADER_CATALOG_COUNT_TEMPLATE] = { "- Catálogo (%d juegos)", "- Catalog (%d games)" },
    [STR_LIST_SHELF_RECENT]                = { "Recién agregados", "Recently added" },
    [STR_LIST_SHELF_SEE_ALL]               = { "Ver todo", "See all" },
    [STR_LIST_SHELF_FEATURED]              = { "Destacado", "Featured" },
    [STR_LIST_SD_CARD]                     = { "Tarjeta SD", "SD Card" },
    [STR_LIST_COL_NAME]                    = { "Nombre", "Name" },
    [STR_LIST_COL_TYPE]                    = { "Tipo", "Type" },
    [STR_LIST_COL_VERSION]                 = { "Año", "Year" },
    [STR_LIST_COL_CATEGORY]                = { "Categoría", "Category" },
    [STR_LIST_COL_SIZE]                    = { "Tamaño", "Size" },
    [STR_LIST_VIEW_GRID]                   = { "cuadrícula", "grid" },
    [STR_LIST_VIEW_LIST]                   = { "lista", "list" },
    [STR_LIST_SEARCH_ACTIVE_SUFFIX]        = { " (activa)", " (active)" },

    // ---- ui_list.c (sidebar + footer button-chip hints) ----
    [STR_LIST_SIDEBAR_CATALOG]             = { "Catálogo", "Catalog" },
    [STR_LIST_SIDEBAR_EXPLORER]            = { "Explorador", "Explorer" },
    [STR_LIST_SIDEBAR_QUEUE]               = { "Cola", "Queue" },
    [STR_LIST_SIDEBAR_SAVES]               = { "Guardados", "Saves" },
    [STR_LIST_SIDEBAR_MTP]                 = { "MTP", "MTP" },
    [STR_LIST_SIDEBAR_FTP]                 = { "FTP", "FTP" },
    [STR_LIST_SIDEBAR_SOURCES]             = { "Fuentes", "Sources" },
    [STR_LIST_SIDEBAR_ABOUT]               = { "Acerca de", "About" },
    [STR_LIST_SIDEBAR_COLLAPSE]            = { "colapsar", "collapse" },
    [STR_LIST_SIDEBAR_EXPAND]              = { "expandir", "expand" },
    [STR_LIST_HINT_NAVIGATE]               = { "navegar", "navigate" },
    [STR_LIST_HINT_INSTALL]                = { "instalar", "install" },
    [STR_LIST_HINT_CATEGORY]               = { "categoría", "category" },
    [STR_LIST_HINT_VIEW_TEMPLATE]          = { "vista %s", "view %s" },
    [STR_LIST_HINT_SORT_TEMPLATE]          = { "ordenar (%s)", "sort (%s)" },
    [STR_LIST_HINT_SEARCH_TEMPLATE]        = { "buscar%s", "search%s" },
    [STR_LIST_HINT_MENU]                   = { "menú", "menu" },
    [STR_LIST_HINT_PANEL_TEMPLATE]         = { "%s panel", "%s panel" },
    [STR_LIST_HINT_RELOAD]                 = { "recargar", "reload" },
    [STR_LIST_HINT_EXIT]                   = { "salir", "exit" },
    [STR_LIST_HINT_OPEN]                   = { "abrir", "open" },
    [STR_LIST_HINT_BACK_TO_CATALOG]        = { "volver al catálogo", "back to catalog" },

    // ---- ui_detail.c ----
    [STR_DETAIL_UNKNOWN_SIZE]              = { "tamaño desconocido", "unknown size" },
    [STR_DETAIL_HEADER_TEMPLATE]           = { "por %s   -   v%s   -   %s", "by %s   -   v%s   -   %s" },
    [STR_DETAIL_HEADER_TEMPLATE_YEAR]      = { "por %s   -   %s   -   %s", "by %s   -   %s   -   %s" },
    [STR_DETAIL_QUEUED_BADGE]              = { "En cola", "Queued" },
    [STR_DETAIL_NSZ_APPLET_HINT]           = { "NSZ: instálalo en modo applet (abre FreeShop desde el álbum)",
                                                "NSZ: install it in applet mode (open FreeShop from the Album)" },
    [STR_DETAIL_DLC_SECTION_TEMPLATE]      = { "DLC y actualizaciones (%d)", "DLC and updates (%d)" },
    [STR_DETAIL_HINT_CHOOSE]               = { "elegir", "choose" },
    [STR_DETAIL_HINT_INSTALL_SELECTED]     = { "instalar seleccionado", "install selected" },
    [STR_DETAIL_HINT_BACK_TO_GAME]         = { "volver al juego", "back to game" },
    [STR_DETAIL_HINT_INSTALL]              = { "instalar", "install" },
    [STR_DETAIL_HINT_INSTALL_DBI]          = { "instalar vía DBI", "install via DBI" },
    [STR_DETAIL_HINT_DLC_TEMPLATE]         = { "ver DLC/actualizaciones (%d)", "view DLC/updates (%d)" },
    [STR_DETAIL_QUEUE_REMOVE_HINT]         = { "quitar de la cola", "remove from queue" },
    [STR_DETAIL_QUEUE_ADD_HINT]            = { "agregar a la cola", "add to queue" },
    [STR_DETAIL_FACT_AUTHOR]               = { "Autor", "Author" },
    [STR_DETAIL_FACT_YEAR]                 = { "Año", "Year" },
    [STR_DETAIL_INSTALL_BUTTON]            = { "INSTALAR", "INSTALL" },

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
    [STR_SOURCES_HINT_TOGGLE]              = { "activar/desactivar", "enable/disable" },
    [STR_SOURCES_HINT_ADD]                 = { "agregar", "add" },
    [STR_SOURCES_HINT_REMOVE]              = { "eliminar", "remove" },

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
    [STR_ABOUT_HINT_EFFECTS_TEMPLATE]      = { "efectos (%s)", "effects (%s)" },
    [STR_ABOUT_HINT_SOUND_TEMPLATE]        = { "sonido (%s)", "sound (%s)" },
    [STR_ABOUT_HINT_BACK]                  = { "volver", "back" },
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
    [STR_QUEUE_CANCELED_TEMPLATE]          = { "Cancelado - %d de %d instalados.", "Canceled - %d of %d installed." },
    [STR_QUEUE_DONE_TEMPLATE]              = { "Terminado - %d de %d instalados.", "Done - %d of %d installed." },
    [STR_QUEUE_TITLE_COUNT_TEMPLATE]       = { "Cola de descargas (%d)", "Download queue (%d)" },
    [STR_QUEUE_EMPTY_LINE1]                = { "No hay nada en la cola.", "The queue is empty." },
    [STR_QUEUE_EMPTY_LINE2]                = { "Entra a una app y presiona + para agregarla a la cola.",
                                                "Open an app and press + to add it to the queue." },
    [STR_QUEUE_HINT_NAVIGATE]              = { "navegar", "navigate" },
    [STR_QUEUE_HINT_START]                 = { "empezar", "start" },
    [STR_QUEUE_HINT_REMOVE]                = { "quitar", "remove" },

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

    // ---- ui_saves.c ----
    [STR_SAVES_TITLE]                      = { "- Guardados", "- Saves" },
    [STR_SAVES_SCANNING]                   = { "Buscando guardados...", "Scanning for saves..." },
    [STR_SAVES_NONE_FOUND]                 = { "(no se encontraron guardados en la consola)",
                                                "(no save data found on this console)" },
    [STR_SAVES_COUNT_TEMPLATE]             = { "%d guardados", "%d saves" },
    [STR_SAVES_BACKUPS_TITLE]              = { "- Backups", "- Backups" },
    [STR_SAVES_LIVE_SIZE_TEMPLATE]         = { "Guardado actual: %s", "Current save: %s" },
    [STR_SAVES_BACKUP_COUNT_TEMPLATE]      = { "%d backups", "%d backups" },
    [STR_SAVES_MOST_RECENT]                = { "más reciente", "most recent" },
    [STR_SAVES_NO_BACKUPS]                 = { "(sin backups todavía - presiona Y para crear uno)",
                                                "(no backups yet - press Y to create one)" },
    [STR_SAVES_HINT_NAVIGATE]              = { "navegar", "navigate" },
    [STR_SAVES_HINT_VIEW_BACKUPS]          = { "ver backups", "view backups" },
    [STR_SAVES_HINT_BACKUP_NOW]            = { "backup ahora", "back up now" },
    [STR_SAVES_HINT_RESTORE]               = { "restaurar", "restore" },
    [STR_SAVES_HINT_DELETE]                = { "eliminar", "delete" },
    [STR_SAVES_HINT_BACK]                  = { "volver", "back" },
    [STR_SAVES_HINT_CANCEL]                = { "cancelar", "cancel" },
    [STR_SAVES_BACKING_UP]                 = { "Haciendo backup...", "Backing up..." },
    [STR_SAVES_RESTORING]                  = { "Restaurando...", "Restoring..." },

    // ---- ui_mtp.c ----
    [STR_MTP_TITLE]                        = { "Instalar por USB", "Install over USB" },
    [STR_MTP_WAITING_USB]                  = { "Esperando el cable USB", "Waiting for the USB cable" },
    [STR_MTP_WAITING_HOST]                 = { "Cable conectado - abriendo la conexión con la PC",
                                                "Cable connected - opening the connection to the PC" },
    [STR_MTP_READY]                        = { "Listo para recibir archivos",
                                                "Ready to receive files" },
    [STR_MTP_WAITING_USB_HELP]             = { "Conectá la consola a la PC con el cable USB-C.",
                                                "Connect the console to your PC with the USB-C cable." },
    [STR_MTP_WAITING_HOST_HELP]            = { "Si la PC no la reconoce, probá desconectar y volver a conectar "
                                                "el cable.",
                                                "If the PC doesn't recognise it, try unplugging the cable and "
                                                "plugging it back in." },
    [STR_MTP_HELP]                         = { "En la PC, la consola aparece como un dispositivo portátil "
                                                "(como un celular). Abrila y arrastrá el archivo adentro: se "
                                                "instala solo, sin ocupar espacio extra en la SD.",
                                                "On the PC, the console shows up as a portable device (like a "
                                                "phone). Open it and drag the file in: it installs itself, with "
                                                "no extra space used on the SD card." },
    [STR_MTP_FORMATS]                      = { "Formatos: NSP, NSZ, XCI, XCZ", "Formats: NSP, NSZ, XCI, XCZ" },
    [STR_MTP_RECEIVING]                    = { "Instalando", "Installing" },
    [STR_MTP_INSTALLING_NOW]               = { "Terminando la instalación...", "Finishing the install..." },
    [STR_MTP_SESSION_COUNT_TEMPLATE]       = { "%d instalados en esta sesión", "%d installed this session" },
    [STR_MTP_QUEUE_TITLE]                  = { "Esta sesión", "This session" },
    [STR_MTP_QUEUE_EMPTY]                  = { "Todavía no se recibió nada.", "Nothing received yet." },
    [STR_MTP_HINT_EXIT]                    = { "salir", "exit" },
    [STR_MTP_START_FAILED_TEMPLATE]        = { "No se pudo iniciar MTP:\n%s", "Couldn't start MTP:\n%s" },

    // ---- ui_ftp.c ----
    [STR_FTP_TITLE]                        = { "Servidor FTP", "FTP Server" },
    [STR_FTP_WAITING_NETWORK]              = { "Esperando conexión de red", "Waiting for a network connection" },
    [STR_FTP_WAITING_NETWORK_HELP]         = { "Conectá la consola a Wi-Fi o cable de red desde Ajustes.",
                                                "Connect the console to Wi-Fi or Ethernet from Settings." },
    [STR_FTP_LISTENING_TEMPLATE]           = { "Listo - conectate a ftp://%s:5000 desde tu PC.",
                                                "Ready - connect to ftp://%s:5000 from your PC." },
    [STR_FTP_LISTENING_HELP]               = { "Usá un cliente FTP (FileZilla, WinSCP) o el explorador de "
                                                "archivos de Windows. Un archivo/conexión a la vez.",
                                                "Use an FTP client (FileZilla, WinSCP) or Windows' file explorer. "
                                                "One file/connection at a time." },
    [STR_FTP_CONNECTED_TEMPLATE]           = { "Conectado: %s", "Connected: %s" },
    [STR_FTP_CONNECTED_HELP]               = { "Navegá, subí o bajá archivos desde tu cliente FTP.",
                                                "Browse, upload or download files from your FTP client." },
    [STR_FTP_FORMATS]                      = { "Formatos que se instalan solos: NSP, NSZ, XCI, XCZ",
                                                "Formats that install themselves: NSP, NSZ, XCI, XCZ" },
    [STR_FTP_TRANSFERRING]                 = { "Transfiriendo", "Transferring" },
    [STR_FTP_INSTALLING_NOW]               = { "Terminando la instalación...", "Finishing the install..." },
    [STR_FTP_SESSION_COUNT_TEMPLATE]       = { "%d en esta sesión", "%d this session" },
    [STR_FTP_QUEUE_TITLE]                  = { "Esta sesión", "This session" },
    [STR_FTP_QUEUE_EMPTY]                  = { "Todavía no hubo actividad.", "No activity yet." },
    [STR_FTP_HINT_EXIT]                    = { "salir", "exit" },
    [STR_FTP_START_FAILED_TEMPLATE]        = { "No se pudo iniciar el servidor FTP:\n%s",
                                                "Couldn't start the FTP server:\n%s" },
    [STR_FTP_STATUS_UPLOADED]              = { "Subido", "Uploaded" },
    [STR_FTP_STATUS_DOWNLOADED]            = { "Descargado", "Downloaded" },

    [STR_SAVES_BACKUP_NOW_CONFIRM_TEMPLATE] = { "¿Crear un backup del guardado de \"%s\"?",
                                                 "Create a backup of \"%s\"'s save data?" },
    [STR_SAVES_BACKUP_DONE]                = { "Backup creado correctamente.", "Backup created successfully." },
    [STR_SAVES_BACKUP_FAILED_TEMPLATE]     = { "No se pudo crear el backup:\n%s", "Couldn't create the backup:\n%s" },
    [STR_SAVES_RESTORE_CONFIRM_TEMPLATE]   = { "¿Restaurar el backup \"%s\"?\n\nEsto sobrescribe el guardado "
                                                "actual del juego.",
                                                "Restore backup \"%s\"?\n\nThis overwrites the game's current "
                                                "save data." },
    [STR_SAVES_RESTORE_DONE]               = { "Guardado restaurado correctamente.", "Save data restored successfully." },
    [STR_SAVES_RESTORE_FAILED_TEMPLATE]    = { "No se pudo restaurar el guardado:\n%s",
                                                "Couldn't restore the save data:\n%s" },
    [STR_SAVES_DELETE_CONFIRM_TEMPLATE]    = { "¿Eliminar el backup \"%s\"?\n\nEsta acción no se puede deshacer.",
                                                "Delete backup \"%s\"?\n\nThis action can't be undone." },
};

const char *tr(StrId id) {
    if (id < 0 || id >= STR_COUNT) return "";
    return kStrings[id][g_lang];
}
