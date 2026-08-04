#pragma once
#include "../catalog/app_entry.h"

// Returned by ui_show_list() when the user asks to exit (B or +).
#define UI_LIST_EXIT (-1)
// Returned by ui_show_list() when the user asks to open the sources screen
// (the "-" button) instead of selecting an entry.
#define UI_LIST_OPEN_SOURCES (-2)
// Returned by ui_show_list() when the user asks to open the "Acerca de"
// screen (the "L" button) instead of selecting an entry.
#define UI_LIST_OPEN_ABOUT (-3)
// Returned by ui_show_list() when the user asks to open the SD card file
// explorer (right stick click) instead of selecting an entry.
#define UI_LIST_OPEN_EXPLORER (-4)
// Returned by ui_show_list() when the user asks to reload the catalog from
// its sources (left stick click) - e.g. new apps were added since this
// session started, without wanting to fully close and reopen the client.
#define UI_LIST_RELOAD_CATALOG (-5)
// Returned by ui_show_list() when the user asks to open the download queue
// screen from the sidebar - see ui_queue.h. B is the sole exit now.
#define UI_LIST_OPEN_QUEUE (-6)
// Returned by ui_show_list() when the user asks to open the save-data
// manager from the sidebar - see ui_saves.h.
#define UI_LIST_OPEN_SAVES (-7)
// Returned by ui_show_list() when the user asks to open the native MTP
// responder from the sidebar - see ui_mtp.h.
#define UI_LIST_OPEN_MTP (-8)
// Returned by ui_show_list() when the user asks to open the FTP server
// from the sidebar - see ui_ftp.h.
#define UI_LIST_OPEN_FTP (-9)

// Renders the catalog (grid/list, toggle with Y, cycle sort with X - both
// re-sort/redisplay `entries` in place) alongside a persistent left sidebar
// (Catálogo/Explorador/Cola/Guardados/MTP/FTP/Fuentes/Acerca de) - L moves input
// focus into the sidebar (Up/Down to pick a section, A to open it, L again
// to come back), otherwise every button acts on the catalog content as
// usual. Blocks until the user selects an app (A, content-focused), asks to
// exit (B), or picks a sidebar section other than Catálogo (which returns
// one of the sentinels above instead of an index). `entries` is sorted in
// place by X, so it isn't const here even though this function never adds/
// removes entries. Entries already in the download queue (ui_queue.h) get a
// badge - queuing itself is done from the detail screen, not here.
int ui_show_list(AppEntry *entries, int count);
