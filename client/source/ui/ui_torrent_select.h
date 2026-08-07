#pragma once
#include "../install/install_torrent.h"

typedef enum {
    // User confirmed a (possibly partial) selection - out_selected_indices/
    // out_selected_count are filled in.
    UI_TORRENT_SELECT_OK,
    // User backed out (B) - nothing should be installed.
    UI_TORRENT_SELECT_CANCELED,
} UiTorrentSelectResult;

// Lets the user pick which of `files` (from install_torrent_preview() - see
// install_torrent.h) to actually install, instead of the whole torrent.
// Every file starts checked (preserves "install everything" as the
// default - the prior, only behavior - so this is purely opt-out).
// `out_selected_indices` must have room for at least `file_count` ints;
// writes each checked entry's `file_index` (not its position in `files`) in
// order, ready to hand straight to install_torrent()/
// install_one_entry_torrent_selected().
UiTorrentSelectResult ui_show_torrent_select(const TorrentFileEntry *files, int file_count,
                                             int *out_selected_indices, int *out_selected_count);
