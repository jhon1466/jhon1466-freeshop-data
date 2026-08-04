#include "save_backup.h"
#include "../install/install_common.h"
#include "../install/zip_create.h"
#include "../install/zip_extract.h"

#include <switch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SAVE_MOUNT_NAME "save"
#define SAVE_BACKUP_ROOT "sdmc:/switch/freeshop/saves"

// Creates every missing ancestor directory of `path`, one level at a time -
// install_common_mkdir_ignore_exists() only creates a single level, and
// SAVE_BACKUP_ROOT is several levels below sdmc:/switch/freeshop which may
// not exist yet if the saves screen is used before Fuentes/sources.c has
// ever run.
static void ensure_dir_tree(const char *path) {
    char buf[FS_MAX_PATH];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            install_common_mkdir_ignore_exists(buf);
            *p = '/';
        }
    }
    install_common_mkdir_ignore_exists(buf);
}

// Swaps characters FAT32/exFAT (what an SD card almost always is) forbid in
// a path component for '_', and trims trailing dots/spaces (which those
// filesystems silently strip anyway - leaving them in would mean the name
// written and the name later found on disk don't match). Used to fold the
// game's own title into the backup folder name below.
static void sanitize_filename(const char *in, char *out, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < out_size; i++) {
        char c = in[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            c = '_';
        }
        out[j++] = c;
    }
    out[j] = '\0';
    while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '.')) out[--j] = '\0';
    if (out[0] == '\0') snprintf(out, out_size, "juego");
}

// Backups live under a folder named after the game, not just its
// application id - the id alone (all this used before) meant every save's
// folder was an opaque 16-hex-digit string, unfindable in a file explorer
// without cross-referencing it against something else first. The id stays
// in brackets alongside the name for uniqueness (two games could share a
// sanitized display name, or a title could be renamed between console
// updates), with a per-profile subfolder below that for consoles with more
// than one account. A save with no owning profile (common/system-adjacent
// account saves, uid all-zero) gets "common" instead of a hex uid.
//
// Note: this changes the path scheme from pre-1.6.0 installs (which used
// `<app id hex>/<uid>/` with no name and stored each backup as a folder
// rather than a .zip - see save_backup_create/restore below) - any backups
// made before that aren't found by save_backup_list anymore. They're still
// physically on the SD card under the old path, just not surfaced by this
// screen; migrating them wasn't worth the complexity for a feature this new.
static void backup_dir_for_entry(const SaveEntry *entry, char *out, size_t out_size) {
    char safe_name[SAVE_ENTRY_NAME_MAX];
    sanitize_filename(entry->name, safe_name, sizeof(safe_name));

    char profile_part[40];
    if (entry->uid.uid[0] == 0 && entry->uid.uid[1] == 0) {
        snprintf(profile_part, sizeof(profile_part), "common");
    } else {
        snprintf(profile_part, sizeof(profile_part), "%016llx%016llx",
                 (unsigned long long)entry->uid.uid[0], (unsigned long long)entry->uid.uid[1]);
    }

    snprintf(out, out_size, "%s/%s [%016llx]/%s", SAVE_BACKUP_ROOT, safe_name,
             (unsigned long long)entry->application_id, profile_part);
}

bool save_backup_create(const SaveEntry *entry, SaveBackupProgressCallback cb, void *userdata,
                         char *err_buf, size_t err_buf_size) {
    if (err_buf && err_buf_size > 0) err_buf[0] = '\0';

    Result rc = fsdevMountSaveData(SAVE_MOUNT_NAME, entry->application_id, entry->uid);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo abrir el guardado (0x%x)", rc);
        return false;
    }

    char base_dir[FS_MAX_PATH];
    backup_dir_for_entry(entry, base_dir, sizeof(base_dir));
    ensure_dir_tree(base_dir);

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char stamp[SAVE_BACKUP_NAME_MAX];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_now);

    char dest_zip[FS_MAX_PATH];
    snprintf(dest_zip, sizeof(dest_zip), "%s/%s.zip", base_dir, stamp);

    bool ok = zip_create_from_dir(SAVE_MOUNT_NAME ":/", dest_zip, cb, userdata, err_buf, err_buf_size);

    fsdevUnmountDevice(SAVE_MOUNT_NAME);
    return ok;
}

static int compare_backup_desc(const void *a, const void *b) {
    const SaveBackupEntry *ea = (const SaveBackupEntry *)a;
    const SaveBackupEntry *eb = (const SaveBackupEntry *)b;
    // Timestamp-named files sort lexically the same as chronologically -
    // reversed for newest-first.
    return strcmp(eb->folder_name, ea->folder_name);
}

int save_backup_list(const SaveEntry *entry, SaveBackupEntry *out, int max) {
    char base_dir[FS_MAX_PATH];
    backup_dir_for_entry(entry, base_dir, sizeof(base_dir));

    DIR *dir = opendir(base_dir);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while (count < max && (ent = readdir(dir)) != NULL) {
        size_t name_len = strlen(ent->d_name);
        // ".zip", case-sensitive - every backup this app writes always is
        // one, so anything else here is foreign and skipped rather than
        // guessed at.
        if (name_len < 5 || strcmp(ent->d_name + name_len - 4, ".zip") != 0) continue;

        char full_path[FS_MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, ent->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        // Strip the ".zip" - callers (ui_saves.c's format_backup_label)
        // still expect a bare "YYYYMMDD-HHMMSS" timestamp, and it's rebuilt
        // into a real path again here on every use rather than carried
        // around as one.
        snprintf(out[count].folder_name, sizeof(out[count].folder_name), "%.*s", (int)(name_len - 4), ent->d_name);
        out[count].total_size = (long)st.st_size; // the compressed archive's own size - no separate walk needed
        count++;
    }
    closedir(dir);

    qsort(out, count, sizeof(SaveBackupEntry), compare_backup_desc);
    return count;
}

bool save_backup_restore(const SaveEntry *entry, const char *folder_name, SaveBackupProgressCallback cb,
                          void *userdata, char *err_buf, size_t err_buf_size) {
    if (err_buf && err_buf_size > 0) err_buf[0] = '\0';

    char base_dir[FS_MAX_PATH];
    backup_dir_for_entry(entry, base_dir, sizeof(base_dir));
    char src_zip[FS_MAX_PATH];
    snprintf(src_zip, sizeof(src_zip), "%s/%s.zip", base_dir, folder_name);

    struct stat st;
    if (stat(src_zip, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "el backup ya no existe");
        return false;
    }

    Result rc = fsdevMountSaveData(SAVE_MOUNT_NAME, entry->application_id, entry->uid);
    if (R_FAILED(rc)) {
        if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo abrir el guardado (0x%x)", rc);
        return false;
    }

    bool ok = zip_extract_to_dir(src_zip, SAVE_MOUNT_NAME ":/", cb, userdata, err_buf, err_buf_size);

    if (ok) {
        rc = fsdevCommitDevice(SAVE_MOUNT_NAME);
        if (R_FAILED(rc)) {
            ok = false;
            if (err_buf) snprintf(err_buf, err_buf_size, "no se pudo confirmar el guardado (0x%x)", rc);
        }
    }

    fsdevUnmountDevice(SAVE_MOUNT_NAME);
    return ok;
}

bool save_backup_delete(const SaveEntry *entry, const char *folder_name) {
    char base_dir[FS_MAX_PATH];
    backup_dir_for_entry(entry, base_dir, sizeof(base_dir));
    char full_path[FS_MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s.zip", base_dir, folder_name);
    return remove(full_path) == 0;
}
