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

// Renders a full-screen scrollable list/grid of `entries` (toggle with Y,
// cycle sort with X - both re-sort/redisplay `entries` in place) and blocks
// until the user selects one (A), asks to exit (B or +), asks to manage
// catalog sources (-), or asks to see app info/donations (L). Returns the
// selected index, or one of the sentinels above. `entries` is sorted in
// place by X, so it isn't const here even though this function never
// adds/removes entries.
int ui_show_list(AppEntry *entries, int count);
