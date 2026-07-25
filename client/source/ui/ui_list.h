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
// screen (+) - see ui_queue.h. B is the sole exit now.
#define UI_LIST_OPEN_QUEUE (-6)

// Renders a full-screen scrollable list/grid of `entries` (toggle with Y,
// cycle sort with X - both re-sort/redisplay `entries` in place) and blocks
// until the user selects one (A), asks to exit (B), asks to manage catalog
// sources (-), asks to see app info/donations (L), or asks to open the
// download queue (+). Returns the selected index, or one of the sentinels
// above. `entries` is sorted in place by X, so it isn't const here even
// though this function never adds/removes entries. Entries already in the
// download queue (ui_queue.h) get a badge - queuing itself is done from the
// detail screen, not here.
int ui_show_list(AppEntry *entries, int count);
