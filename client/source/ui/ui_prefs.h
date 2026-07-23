#pragma once
#include <stdbool.h>
#include <stddef.h>

#define UI_PREFS_CATEGORY_MAX 64

// Persists the list/grid screen's own preferences (view mode, sort mode,
// active category filter) across launches - everything ui_show_list()
// otherwise only remembered in-memory (via its own static locals) for as
// long as the app stayed open. Stored as plain ints rather than sharing
// ui_list.c's private ViewMode/SortMode enums, so this header doesn't need
// to know about them (or vice versa) - ui_list.c casts at its own call
// sites, same way it already treats its persisted static locals.
typedef struct {
    int view_mode;
    int sort_mode;
    char category_filter[UI_PREFS_CATEGORY_MAX];
} UiListPrefs;

// Loads sdmc:/switch/freeshop/prefs.json into `out`, defaulting every field
// to 0/"" (view_mode 0 = list, sort_mode 0 = title, no category filter) if
// the file doesn't exist yet or can't be parsed - same tolerant-of-a-
// missing-file approach as sources.c.
void ui_prefs_load(UiListPrefs *out);

// Best-effort - a failed save just means the preference doesn't stick for
// next launch, not a user-facing error worth interrupting them over.
void ui_prefs_save(const UiListPrefs *prefs);
