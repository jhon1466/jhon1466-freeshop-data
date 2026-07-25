#pragma once
#include "install.h"
#include "../catalog/app_entry.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    INSTALL_ONE_OK,
    INSTALL_ONE_CANCELED,
    INSTALL_ONE_ERROR,
} InstallOneResult;

// Installs `entry` via whichever native installer matches its file_type
// (NSP/XCI/PORT/NRO), driving `cb`/`phase_cb`/`userdata` for progress - the
// per-type dispatch shared between the single-item install flow (main.c) and
// the download queue (ui_queue.c) so neither has to duplicate it. `base_url`
// comes from entry->source_base_url. Does NOT cover the DBI hand-off
// (install_nsp_and_launch_dbi) - that's an explicit single-item manual
// fallback offered from the detail screen, not something to run across a
// whole unattended queue.
InstallOneResult install_one_entry(const AppEntry *entry,
                                   InstallProgressCallback cb, InstallPhaseCallback phase_cb, void *userdata,
                                   char *err_buf, size_t err_buf_size);

// True for the file types whose install-failure message suggests the DBI
// fallback (NSP/XCI) - lets callers tailor their error message the same way.
bool install_suggests_dbi_fallback(AppFileType type);
