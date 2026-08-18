#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pipensx {

struct SaveBackupInfo {
    std::string path;
    // Directory name the backup lives under, also its display label -
    // "2026-08-06_21-30-05" or "2026-08-06_21-31-40_before_restore".
    std::string label;
    uint64_t totalBytes = 0;
};

// 128-bit account user id, matches libnx AccountUid (two u64s). A zeroed
// AccountUserId means "use the currently active user" for backup/restore.
struct AccountUserId {
    uint64_t high = 0;
    uint64_t low = 0;

    bool operator==(const AccountUserId& o) const {
        return high == o.high && low == o.low;
    }
    bool operator!=(const AccountUserId& o) const { return !(*this == o); }
    bool isZero() const { return high == 0 && low == 0; }
};

struct UserProfile {
    AccountUserId uid;
    std::string name;
    std::string imagePath;
};

// sdmc:/switch/freeshop-client/saves - every backup this feature makes lives
// under here, one subfolder per title ID.
std::string saveBackupsRoot();

// False when the console has no account profile this process can read (acc
// service unavailable, or no user selected). Every other function in this
// header fails the same way in that case; callers can use this to grey the
// feature out up front instead of surfacing the failure per action.
bool saveDataAccountAvailable();

// List all user profiles available on the console. Returns true on success,
// fills `users` with available profiles. Fills `error` on failure.
bool listUserProfiles(std::vector<UserProfile>& users, std::string& error);

// Read-only mounts applicationId's save data and copies it into a fresh
// timestamped folder under saveBackupsRoot()/titleId/. Never touches the
// live save data - fsdevMountSaveDataReadOnly makes that a guarantee, not
// just an intention. gameName is folded (sanitized) into the backup folder's
// name so it reads as more than a bare timestamp; pass "" to skip that.
// If `userUid` is zeroed, uses the currently active user (same as before).
bool backupSaveData(uint64_t applicationId, const std::string& titleId,
                    const std::string& gameName, std::string& outPath,
                    std::string& error,
                    const AccountUserId& userUid = {});

// Newest first. gameName makes the per-title folder readable
// ("<name> [<titleId>]"); pass "" to look up the bare titleId folder.
bool listSaveBackups(const std::string& titleId, const std::string& gameName,
                     std::vector<SaveBackupInfo>& backups, std::string& error);

bool deleteSaveBackup(const std::string& backupPath, std::string& error);

// Overwrites applicationId's live save data with backupPath's contents.
// Makes its own backupSaveData() of the CURRENT state first and aborts
// without touching anything live if that fails, so a restore can never
// leave the console with neither the old nor the new save.
// If `userUid` is zeroed, uses the currently active user (same as before).
bool restoreSaveData(uint64_t applicationId, const std::string& titleId,
                     const std::string& gameName,
                     const std::string& backupPath, std::string& error,
                     const AccountUserId& userUid = {});

}  // namespace pipensx
