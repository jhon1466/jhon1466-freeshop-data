#pragma once
#include "../catalog/app_entry.h"

// Renders a full-screen scrollable list of `entries` and blocks until the
// user selects one (A) or asks to exit (B or +). Returns the selected
// index, or -1 on exit.
int ui_show_list(const AppEntry *entries, int count);
