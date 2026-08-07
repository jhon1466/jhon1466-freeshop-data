#include "app/save_data_service.hpp"
#include "app/file_explorer_service.hpp"

extern "C" {
#include "../core/util.h"
}

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
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
    log_msg("[saves] ensureDirectory: mkdir %s\n", path.c_str());
    const int mkdirResult = mkdir(path.c_str(), 0755);
    const int mkdirErrno = errno;
    log_msg("[saves] ensureDirectory: mkdir(%s) = %d errno=%d\n",
            path.c_str(), mkdirResult, mkdirErrno);
    if (mkdirResult == 0)
        return true;
    // EEXIST is the expected case for every parent segment, and it already
    // proves something is there — no stat() needed. If that something is a
    // file rather than a directory the following mkdir/open fails anyway,
    // with a clearer error than a stat probe would have produced.
    return mkdirErrno == EEXIST;
}

bool ensurePath(const std::string& path) {
    log_msg("[saves] ensurePath: %s\n", path.c_str());
    if (!ensureDirectory("sdmc:/switch"))
        return false;
    if (!ensureDirectory("sdmc:/switch/freeshop-client"))
        return false;
    std::string built = "sdmc:/switch/freeshop-client";
    // path is always "sdmc:/switch/freeshop-client/..." from
    // saveBackupsRoot()-derived callers - walk the remaining segments.
    std::string rest = path.substr(built.size());
    log_msg("[saves] ensurePath: rest=%s\n", rest.c_str());
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
        log_msg("[saves] ensurePath: segment built=%s\n", built.c_str());
        if (!ensureDirectory(built))
            return false;
        start = slash;
    }
    log_msg("[saves] ensurePath: done\n");
    return true;
}

bool activeUser(AccountUid& uid, std::string& error) {
    Result rc = accountGetPreselectedUser(&uid);
    if (R_SUCCEEDED(rc) && accountUidIsValid(&uid))
        return true;
    rc = accountGetLastOpenedUser(&uid);
    if (R_SUCCEEDED(rc) && accountUidIsValid(&uid))
        return true;
    error = "No hay ningún perfil de cuenta disponible.";
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

// Strips characters FAT32/exFAT (the SD card's filesystem) can't hold in a
// path component, collapses whitespace runs, and caps the length - a title's
// display name is free-form text (may carry ": ", "/", trademark glyphs,
// trailing dots) with no such guarantee.
std::string sanitizeForPathSegment(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    bool lastWasSpace = false;
    for (unsigned char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c < 0x20) {
            c = ' ';
        }
        if (c == ' ') {
            if (lastWasSpace)
                continue;
            lastWasSpace = true;
        } else {
            lastWasSpace = false;
        }
        out.push_back(static_cast<char>(c));
        if (out.size() >= 48)
            break;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    return out;
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
                    const std::string& gameName, std::string& outPath,
                    std::string& error) {
    // Per-line flushing for the duration of the backup only: this function has
    // several early returns, so scope it to an RAII guard rather than trying to
    // clear the flag on each one.
    struct VerboseLogScope {
        VerboseLogScope() { log_set_verbose(1); }
        ~VerboseLogScope() { log_set_verbose(0); }
    } verboseLogScope;
    log_msg("[saves] backup begin title=%s app=%016llx\n", titleId.c_str(),
            (unsigned long long)applicationId);
    AccountUid uid;
    if (!activeUser(uid, error)) {
        log_msg("[saves] backup: no active account: %s\n", error.c_str());
        return false;
    }

    const std::string titleRoot =
        explorerJoinPath(saveBackupsRoot(), titleId);
    const std::string sanitizedName = sanitizeForPathSegment(gameName);
    const std::string backupLabel = sanitizedName.empty()
        ? timestampLabel()
        : timestampLabel() + " - " + sanitizedName;
    const std::string destDir = explorerJoinPath(titleRoot, backupLabel);
    log_msg("[saves] backup destDir=%s\n", destDir.c_str());
    if (!ensurePath(destDir)) {
        error = "No se pudo crear la carpeta de respaldo.";
        log_msg("[saves] backup: ensurePath failed\n");
        return false;
    }

    const std::string mountName =
        "savebk" + std::to_string(gMountSerial.fetch_add(1));
    log_msg("[saves] backup mounting %s (read-only)\n", mountName.c_str());
    const Result rc =
        fsdevMountSaveDataReadOnly(mountName.c_str(), applicationId, uid);
    if (R_FAILED(rc)) {
        error = "No se pudieron abrir los datos de guardado de este título.";
        log_msg("[saves] backup: mount failed rc=0x%08x\n", rc);
        return false;
    }
    log_msg("[saves] backup mounted, copying tree\n");

    const bool ok =
        copyTreeContents(mountName + ":/", destDir, error);
    log_msg("[saves] backup copyTreeContents ok=%d error=%s\n", ok ? 1 : 0,
            error.c_str());
    fsdevUnmountDevice(mountName.c_str());
    log_msg("[saves] backup unmounted %s\n", mountName.c_str());
    if (!ok) {
        std::string cleanupError;
        deleteEntry(destDir, cleanupError);
        return false;
    }
    outPath = destDir;
    log_msg("[saves] backup complete outPath=%s\n", outPath.c_str());
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
                     const std::string& gameName,
                     const std::string& backupPath, std::string& error) {
    // A restore that cannot even protect the save it is about to overwrite
    // does not proceed - the live save is never touched in that case.
    std::string safetyPath;
    std::string safetyError;
    if (!backupSaveData(applicationId, titleId, gameName, safetyPath,
                        safetyError)) {
        error = "No se pudo hacer una copia de seguridad antes de restaurar, así que no "
                "se cambió nada: " + safetyError;
        return false;
    }

    AccountUid uid;
    if (!activeUser(uid, error))
        return false;

    const std::string mountName =
        "saverw" + std::to_string(gMountSerial.fetch_add(1));
    const Result rc = fsdevMountSaveData(mountName.c_str(), applicationId, uid);
    if (R_FAILED(rc)) {
        error = "No se pudieron abrir los datos de guardado de este título.";
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
            error = "La restauración no se confirmó; el servicio de datos de guardado "
                    "la rechazó.";
        }
    }
    fsdevUnmountDevice(mountName.c_str());
    return ok;
}

#else  // !__SWITCH__

bool saveDataAccountAvailable() { return false; }

bool backupSaveData(uint64_t, const std::string&, std::string&,
                    std::string& error) {
    error = "El acceso a los datos de guardado solo está disponible en la consola.";
    return false;
}

bool listSaveBackups(const std::string&, std::vector<SaveBackupInfo>&,
                     std::string&) {
    return true;
}

bool deleteSaveBackup(const std::string&, std::string& error) {
    error = "El acceso a los datos de guardado solo está disponible en la consola.";
    return false;
}

bool restoreSaveData(uint64_t, const std::string&, const std::string&,
                     std::string& error) {
    error = "El acceso a los datos de guardado solo está disponible en la consola.";
    return false;
}

#endif

}  // namespace pipensx
