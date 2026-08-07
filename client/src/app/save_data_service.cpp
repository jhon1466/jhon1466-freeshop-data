#include "app/save_data_service.hpp"
#include "app/file_explorer_service.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace pipensx {

std::string saveBackupsRoot() {
    return "sdmc:/switch/freeshop-client/saves";
}

#ifdef __SWITCH__

namespace {

std::atomic<int> gMountSerial{0};

// mkdir() only makes one level; every caller here builds a path under
// saveBackupsRoot() a segment at a time, so an existing parent (EEXIST) is
// the expected case, not an error.
bool ensureDirectory(const std::string& path) {
    if (mkdir(path.c_str(), 0755) == 0)
        return true;
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool ensurePath(const std::string& path) {
    if (!ensureDirectory("sdmc:/switch"))
        return false;
    if (!ensureDirectory("sdmc:/switch/freeshop-client"))
        return false;
    std::string built = "sdmc:/switch/freeshop-client";
    // path is always "sdmc:/switch/freeshop-client/..." from
    // saveBackupsRoot()-derived callers - walk the remaining segments.
    std::string rest = path.substr(built.size());
    size_t start = 0;
    while (start < rest.size()) {
        if (rest[start] == '/') {
            ++start;
            continue;
        }
        size_t slash = rest.find('/', start);
        if (slash == std::string::npos)
            slash = rest.size();
        built += "/" + rest.substr(start, slash - start);
        if (!ensureDirectory(built))
            return false;
        start = slash;
    }
    return true;
}

bool activeUser(AccountUid& uid, std::string& error) {
    Result rc = accountGetPreselectedUser(&uid);
    if (R_SUCCEEDED(rc) && accountUidIsValid(&uid))
        return true;
    rc = accountGetLastOpenedUser(&uid);
    if (R_SUCCEEDED(rc) && accountUidIsValid(&uid))
        return true;
    error = "No account profile is available.";
    return false;
}

std::string timestampLabel() {
    const time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S", &local);
    return buffer;
}

uint64_t directorySize(const std::string& path) {
    std::vector<ExplorerEntry> children;
    std::string error;
    if (!listDirectory(path, children, error))
        return 0;
    uint64_t total = 0;
    for (const ExplorerEntry& child : children)
        total += child.directory ? directorySize(child.path) : child.size;
    return total;
}

// Copies every entry at the root of a mounted save data filesystem (or a
// prior backup folder) into destDir. Both source shapes are a flat set of
// top-level entries, so one function covers "back up a live save" and
// "restore a save onto a live mount" alike.
bool copyTreeContents(const std::string& srcRoot, const std::string& destDir,
                      std::string& error) {
    std::vector<ExplorerEntry> children;
    if (!listDirectory(srcRoot, children, error))
        return false;
    for (const ExplorerEntry& child : children)
        if (!copyEntry(child.path, destDir, error))
            return false;
    return true;
}

} // namespace

bool saveDataAccountAvailable() {
    AccountUid uid;
    std::string error;
    return activeUser(uid, error);
}

bool backupSaveData(uint64_t applicationId, const std::string& titleId,
                    std::string& outPath, std::string& error) {
    AccountUid uid;
    if (!activeUser(uid, error))
        return false;

    const std::string titleRoot =
        explorerJoinPath(saveBackupsRoot(), titleId);
    const std::string destDir =
        explorerJoinPath(titleRoot, timestampLabel());
    if (!ensurePath(destDir)) {
        error = "Could not create the backup folder.";
        return false;
    }

    const std::string mountName =
        "savebk" + std::to_string(gMountSerial.fetch_add(1));
    const Result rc =
        fsdevMountSaveDataReadOnly(mountName.c_str(), applicationId, uid);
    if (R_FAILED(rc)) {
        error = "Could not open the save data for this title.";
        return false;
    }

    const bool ok =
        copyTreeContents(mountName + ":/", destDir, error);
    fsdevUnmountDevice(mountName.c_str());
    if (!ok) {
        std::string cleanupError;
        deleteEntry(destDir, cleanupError);
        return false;
    }
    outPath = destDir;
    return true;
}

bool listSaveBackups(const std::string& titleId,
                     std::vector<SaveBackupInfo>& backups, std::string& error) {
    backups.clear();
    const std::string titleRoot =
        explorerJoinPath(saveBackupsRoot(), titleId);
    std::vector<ExplorerEntry> entries;
    if (!listDirectory(titleRoot, entries, error)) {
        // No backups yet is not a failure - the folder simply does not
        // exist until the first one is made.
        error.clear();
        return true;
    }
    for (const ExplorerEntry& entry : entries) {
        if (!entry.directory)
            continue;
        SaveBackupInfo info;
        info.path = entry.path;
        info.label = entry.name;
        info.totalBytes = directorySize(entry.path);
        backups.push_back(std::move(info));
    }
    std::sort(backups.begin(), backups.end(),
             [](const SaveBackupInfo& a, const SaveBackupInfo& b) {
                 return a.label > b.label;
             });
    return true;
}

bool deleteSaveBackup(const std::string& backupPath, std::string& error) {
    return deleteEntry(backupPath, error);
}

bool restoreSaveData(uint64_t applicationId, const std::string& titleId,
                     const std::string& backupPath, std::string& error) {
    // A restore that cannot even protect the save it is about to overwrite
    // does not proceed - the live save is never touched in that case.
    std::string safetyPath;
    std::string safetyError;
    if (!backupSaveData(applicationId, titleId, safetyPath, safetyError)) {
        error = "Could not make a safety copy before restoring, so nothing "
                "was changed: " + safetyError;
        return false;
    }

    AccountUid uid;
    if (!activeUser(uid, error))
        return false;

    const std::string mountName =
        "saverw" + std::to_string(gMountSerial.fetch_add(1));
    const Result rc = fsdevMountSaveData(mountName.c_str(), applicationId, uid);
    if (R_FAILED(rc)) {
        error = "Could not open the save data for this title.";
        return false;
    }

    const std::string mountRoot = mountName + ":/";
    std::vector<ExplorerEntry> backupChildren;
    bool ok = listDirectory(backupPath, backupChildren, error);
    if (ok) {
        for (const ExplorerEntry& child : backupChildren) {
            const std::string destPath =
                explorerJoinPath(mountRoot, child.name);
            struct stat st {};
            if (stat(destPath.c_str(), &st) == 0) {
                std::string deleteError;
                if (!deleteEntry(destPath, deleteError)) {
                    error = deleteError;
                    ok = false;
                    break;
                }
            }
            if (!copyEntry(child.path, mountRoot, error)) {
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        const Result commit = fsdevCommitDevice(mountName.c_str());
        if (R_FAILED(commit)) {
            ok = false;
            error = "The restore did not commit; the save data service "
                    "rejected it.";
        }
    }
    fsdevUnmountDevice(mountName.c_str());
    return ok;
}

#else  // !__SWITCH__

bool saveDataAccountAvailable() { return false; }

bool backupSaveData(uint64_t, const std::string&, std::string&,
                    std::string& error) {
    error = "Save data access is only available on console.";
    return false;
}

bool listSaveBackups(const std::string&, std::vector<SaveBackupInfo>&,
                     std::string&) {
    return true;
}

bool deleteSaveBackup(const std::string&, std::string& error) {
    error = "Save data access is only available on console.";
    return false;
}

bool restoreSaveData(uint64_t, const std::string&, const std::string&,
                     std::string& error) {
    error = "Save data access is only available on console.";
    return false;
}

#endif

}  // namespace pipensx
