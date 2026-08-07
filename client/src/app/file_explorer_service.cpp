#include "app/file_explorer_service.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace pipensx {

std::string explorerParentPath(const std::string& path) {
    if (path == "sdmc:/" || path.empty())
        return "sdmc:/";
    std::string trimmed = path;
    while (trimmed.size() > std::strlen("sdmc:/") && trimmed.back() == '/')
        trimmed.pop_back();
    const size_t slash = trimmed.find_last_of('/');
    if (slash == std::string::npos || slash < std::strlen("sdmc:/") - 1)
        return "sdmc:/";
    return trimmed.substr(0, slash + 1);
}

std::string explorerJoinPath(const std::string& dir, const std::string& name) {
    std::string joined = dir;
    if (!joined.empty() && joined.back() != '/')
        joined += '/';
    joined += name;
    return joined;
}

std::string explorerBaseName(const std::string& path) {
    std::string trimmed = path;
    while (trimmed.size() > std::strlen("sdmc:/") && trimmed.back() == '/')
        trimmed.pop_back();
    const size_t slash = trimmed.find_last_of('/');
    return slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
}

bool listDirectory(const std::string& path, std::vector<ExplorerEntry>& entries,
                   std::string& error) {
    entries.clear();
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        error = std::strerror(errno);
        return false;
    }
    std::vector<ExplorerEntry> directories;
    std::vector<ExplorerEntry> files;
    while (dirent* item = readdir(dir)) {
        if (std::strcmp(item->d_name, ".") == 0 ||
            std::strcmp(item->d_name, "..") == 0)
            continue;
        ExplorerEntry entry;
        entry.name = item->d_name;
        entry.path = explorerJoinPath(path, entry.name);
        struct stat st {};
        if (stat(entry.path.c_str(), &st) != 0)
            continue;
        entry.directory = S_ISDIR(st.st_mode);
        entry.size = entry.directory ? 0 : static_cast<uint64_t>(st.st_size);
        (entry.directory ? directories : files).push_back(std::move(entry));
    }
    closedir(dir);
    auto byName = [](const ExplorerEntry& a, const ExplorerEntry& b) {
        return a.name < b.name;
    };
    std::sort(directories.begin(), directories.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    entries = std::move(directories);
    entries.insert(entries.end(), std::make_move_iterator(files.begin()),
                   std::make_move_iterator(files.end()));
    return true;
}

bool deleteEntry(const std::string& path, std::string& error) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        error = std::strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path.c_str()) != 0) {
            error = std::strerror(errno);
            return false;
        }
        return true;
    }
    std::vector<ExplorerEntry> children;
    if (!listDirectory(path, children, error))
        return false;
    for (const ExplorerEntry& child : children)
        if (!deleteEntry(child.path, error))
            return false;
    if (rmdir(path.c_str()) != 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

namespace {

bool copyFileContents(const std::string& srcPath, const std::string& dstPath,
                      std::string& error) {
    std::ifstream input(srcPath, std::ios::binary);
    if (!input) {
        error = std::strerror(errno);
        return false;
    }
    std::ofstream output(dstPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = std::strerror(errno);
        return false;
    }
    std::array<char, 128 * 1024> buffer;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0)
            output.write(buffer.data(), count);
        if (!output) {
            error = "write failed";
            unlink(dstPath.c_str());
            return false;
        }
    }
    output.flush();
    if (input.bad() || !output) {
        error = "copy failed";
        unlink(dstPath.c_str());
        return false;
    }
    return true;
}

} // namespace

bool copyEntry(const std::string& srcPath, const std::string& dstDir,
               std::string& error) {
    const std::string dstPath =
        explorerJoinPath(dstDir, explorerBaseName(srcPath));
    struct stat existing {};
    if (stat(dstPath.c_str(), &existing) == 0) {
        error = "already exists at the destination";
        return false;
    }
    struct stat st {};
    if (stat(srcPath.c_str(), &st) != 0) {
        error = std::strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode))
        return copyFileContents(srcPath, dstPath, error);

    if (mkdir(dstPath.c_str(), 0755) != 0) {
        error = std::strerror(errno);
        return false;
    }
    std::vector<ExplorerEntry> children;
    if (!listDirectory(srcPath, children, error))
        return false;
    for (const ExplorerEntry& child : children)
        if (!copyEntry(child.path, dstPath, error))
            return false;
    return true;
}

bool moveEntry(const std::string& srcPath, const std::string& dstDir,
              std::string& error) {
    const std::string dstPath =
        explorerJoinPath(dstDir, explorerBaseName(srcPath));
    struct stat existing {};
    if (stat(dstPath.c_str(), &existing) == 0) {
        error = "already exists at the destination";
        return false;
    }
    if (rename(srcPath.c_str(), dstPath.c_str()) == 0)
        return true;
    // Cross-mount-point moves (e.g. a "romfs:"-backed shortcut, or a
    // devoptab boundary rename() refuses) fall back to a copy the caller
    // cannot tell apart from a native move.
    if (!copyEntry(srcPath, dstDir, error))
        return false;
    std::string deleteError;
    if (!deleteEntry(srcPath, deleteError)) {
        // The copy is in place and verified reachable; leaving the source
        // behind loses no data, just leaves a duplicate for the user to
        // clean up by hand.
        error = deleteError;
        return false;
    }
    return true;
}

bool renameEntry(const std::string& path, const std::string& newName,
                 std::string& error) {
    const std::string dstPath =
        explorerJoinPath(explorerParentPath(path), newName);
    struct stat existing {};
    if (stat(dstPath.c_str(), &existing) == 0) {
        error = "already exists";
        return false;
    }
    if (rename(path.c_str(), dstPath.c_str()) != 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

bool createDirectory(const std::string& parentPath, const std::string& name,
                     std::string& error) {
    const std::string path = explorerJoinPath(parentPath, name);
    struct stat existing {};
    if (stat(path.c_str(), &existing) == 0) {
        error = "already exists";
        return false;
    }
    if (mkdir(path.c_str(), 0755) != 0) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

}  // namespace pipensx
