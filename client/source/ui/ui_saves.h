#pragma once

// Shows the save-data manager: lists every installed title with an
// Account-type save (regardless of how it was installed - not limited to
// the FreeShop catalog), A drills into that title's backups (restore/
// delete from there, or create a new one), Y backs it up immediately
// without drilling in, B/+ goes back. See ../saves/save_scan.h and
// ../saves/save_backup.h for the underlying scan/backup/restore engine.
void ui_show_saves(void);
