#pragma once
#include <switch.h>
#include <stdbool.h>
#include <stddef.h>

// Generous headroom for even a well-stocked console - same reasoning as
// ncm_cleanup.c's MAX_META_KEYS.
#define SAVES_MAX 256
#define SAVE_ENTRY_NAME_MAX 0x200
#define SAVE_ENTRY_NICKNAME_MAX 0x20

typedef struct {
    u64 application_id;
    AccountUid uid;      // all-zero for a save with no owning profile
    u64 save_data_id;
    s64 size;
    // Resolved game title (nsGetApplicationControlData + nacpGetLanguageEntry).
    // Falls back to the application id in hex when the title's control data
    // can't be read anymore (e.g. the game was uninstalled but its save
    // wasn't) - still shown rather than dropped, same as JKSV does.
    char name[SAVE_ENTRY_NAME_MAX];
    char nickname[SAVE_ENTRY_NICKNAME_MAX]; // owning profile's nickname, "" if unresolved
} SaveEntry;

// Scans every Account-type save data on the console (fsOpenSaveDataInfoReader
// over FsSaveDataSpaceId_User, filtered to FsSaveDataType_Account - the
// per-profile game saves; system/bcat/device/cache/temporary saves are
// skipped), resolving each one's game title and owning profile's nickname.
// Writes up to `max` entries into `out`, sorted alphabetically by name, and
// returns the count written. Best-effort per-entry - a save whose title or
// profile can't be resolved still gets an entry (see SaveEntry.name). Only
// fails outright (returns 0 with a reason in err_buf) if the save data
// listing itself couldn't be opened.
int saves_scan(SaveEntry *out, int max, char *err_buf, size_t err_buf_size);

// Fetches the JPEG icon bytes for `application_id` (same source as
// saves_scan's title resolution) into `out` (caller-provided buffer, at
// least 0x20000 bytes - NsApplicationControlData's fixed icon size).
// Writes the actual JPEG length to `out_len`. Returns false if the
// application's control data can't be read (e.g. an orphaned save with no
// installed title behind it).
bool saves_fetch_icon_jpeg(u64 application_id, unsigned char *out, size_t out_size, size_t *out_len);
