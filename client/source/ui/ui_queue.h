#pragma once
#include "../catalog/app_entry.h"
#include <stdbool.h>

// Cap on how many entries can be queued at once - generous for a homebrew
// session, avoids a heap allocation for what's normally a handful of titles.
#define UI_QUEUE_MAX 64

// The download queue is a plain set of catalog ids (stable across reloads/
// re-sorts, unlike array indices). Add/remove is driven from the detail
// screen (+), the badge in the main list/grid reads ui_queue_contains(), and
// the whole batch is browsed/started from ui_show_queue().
void ui_queue_add(const char *id);
void ui_queue_remove(const char *id);
bool ui_queue_contains(const char *id);
int ui_queue_count(void);

// Full-screen download-queue manager. Lists the queued items (icon, title,
// type/size) with a per-item status, lets the user remove items (X) and
// start the whole batch (A). While a batch runs, this same screen renders
// live progress - which item is active, its progress bar, speed and ETA -
// so the user watches the queue drain in place instead of a generic
// full-screen download view. Installs run one at a time, sequentially (no
// background threading); the user stays on this screen until it finishes or
// they cancel (B held during an item stops the batch). Blocks until the
// user backs out (B from the browse view). `entries`/`count` is the FULL
// catalog (including DLC/updates, whose ids can also be queued) so any
// queued id resolves to a title/size/icon.
void ui_show_queue(AppEntry *entries, int count);
