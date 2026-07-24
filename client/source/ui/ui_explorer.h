#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    UI_EXPLORER_INSTALL, // user picked an .nsp/.xci - out_path/out_is_xci are filled
    UI_EXPLORER_EXIT,    // user backed out without picking anything
} UiExplorerAction;

// Browses the SD card starting at sdmc:/, letting the user navigate
// directories and pick an .nsp or .xci file to install directly - the
// local-file counterpart of browsing the online catalog (see
// install/install_local.h). `out_path` (must be at least 512 bytes) is
// filled with the picked file's full sdmc: path when returning
// UI_EXPLORER_INSTALL; *out_is_xci says which of
// install_nsp_from_local_file/install_xci_from_local_file the caller should
// use on it.
UiExplorerAction ui_show_explorer(char *out_path, size_t out_path_size, bool *out_is_xci);
