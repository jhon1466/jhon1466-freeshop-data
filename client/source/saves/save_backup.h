#pragma once
#include "save_scan.h"
#include <stdbool.h>
#include <stddef.h>

// One backup = one timestamped .zip file (Deflate-compressed, via
// install/zip_create.h) mirroring the save's file tree, under
// sdmc:/switch/freeshop/saves/<sanitized game name> [<application id hex>]/<uid hex, or "common">/<timestamp>.zip -
// the name makes the folder findable in a file explorer without having to
// cross-reference the application id against anything else first; see
// backup_dir_for_entry in save_backup.c for the exact scheme, including a
// note on why pre-1.6.0 (unzipped, id-only) backups aren't found by this
// anymore.
#define SAVE_BACKUP_MAX 64
#define SAVE_BACKUP_NAME_MAX 32

typedef struct {
    char folder_name[SAVE_BACKUP_NAME_MAX]; // e.g. "20260803-143210" (the ".zip" is implicit) - lexically sortable
    long total_size;                        // the compressed .zip file's own size, bytes
} SaveBackupEntry;

// Same shape as install.h's InstallProgressCallback, for the same reason:
// called periodically with bytes-processed-so-far/total (uncompressed) so
// the caller can drive a progress bar instead of the screen appearing to
// hang on a large save. Return false to cancel - the zip being written/
// extracted is left as far as it got (an unfinished archive is deleted by
// save_backup_create itself rather than left corrupt; a canceled restore's
// partially-overwritten save data is left as-is, same as any other
// partial-copy failure). May be NULL.
typedef bool (*SaveBackupProgressCallback)(long total, long now, void *userdata);

// Mounts `entry`'s live save data read-only and compresses it into a new
// timestamped .zip on the SD card. Returns false with a reason in err_buf
// on failure - an incomplete archive (a failure mid-way, or the callback
// canceled) is deleted rather than left behind corrupt.
bool save_backup_create(const SaveEntry *entry, SaveBackupProgressCallback cb, void *userdata,
                         char *err_buf, size_t err_buf_size);

// Lists existing backups for `entry`, newest first. Returns the count
// written into `out` (capped at max, 0 if there are none yet).
int save_backup_list(const SaveEntry *entry, SaveBackupEntry *out, int max);

// Extracts `folder_name`'s backup over entry's live save data (mounted
// writable) and commits the journal (fsdevCommitDevice) - required for a
// save-data write to actually persist. Files present in the live save but
// not in the backup are left untouched, not deleted (same restore
// semantics as JKSV - a restore only overwrites, never prunes). Returns
// false with a reason in err_buf on failure.
bool save_backup_restore(const SaveEntry *entry, const char *folder_name, SaveBackupProgressCallback cb,
                          void *userdata, char *err_buf, size_t err_buf_size);

// Permanently deletes one backup .zip from the SD card.
bool save_backup_delete(const SaveEntry *entry, const char *folder_name);
