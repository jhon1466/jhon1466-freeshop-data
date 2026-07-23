#pragma once
#include "../catalog/app_entry.h"

typedef enum {
    UI_DETAIL_INSTALL,
    UI_DETAIL_BACK,
} UiDetailAction;

// Shows the detail screen for `entry`. `dlc_entries`/`dlc_count` are the
// DLC/update entries linked to it (main.c finds these by scanning the full
// catalog for parent_id == entry->id) - if dlc_count > 0, a navigable
// "DLC y actualizaciones" section is also shown (Y focuses it, Up/Down pick
// one, A installs the focused one instead of the base entry, B returns
// focus to the base entry instead of leaving the screen).
//
// Blocks until the user backs out from the base-entry focus (B) or installs
// something (A, from either focus). On UI_DETAIL_INSTALL, *out_target is
// set to whichever entry was actually chosen - `entry` itself, or one of
// `dlc_entries` - the caller must install *out_target, not necessarily
// `entry`.
UiDetailAction ui_show_detail(const AppEntry *entry, const AppEntry *dlc_entries, int dlc_count,
                              const AppEntry **out_target);
