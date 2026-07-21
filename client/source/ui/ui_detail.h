#pragma once
#include "../catalog/app_entry.h"

typedef enum {
    UI_DETAIL_INSTALL,
    UI_DETAIL_BACK,
} UiDetailAction;

// Shows the detail screen for a single entry, blocking until the user
// presses A (install) or B (back).
UiDetailAction ui_show_detail(const AppEntry *entry);
