#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pipensx {

struct ExplorerEntry {
    std::string name;
    std::string path;
    bool directory = false;
    uint64_t size = 0;
};

// Directories first, then files, both alphabetical. No "." / ".." entries -
// callers that want an up-navigation row add it themselves (see
// explorerParentEntry in explorer_view.hpp), since only a UI knows whether
// it wants one.
bool listDirectory(const std::string& path, std::vector<ExplorerEntry>& entries,
                   std::string& error);

// Deletes a file, or recursively deletes a directory and everything in it.
bool deleteEntry(const std::string& path, std::string& error);

// Copies a file, or recursively copies a directory tree, so that
// <dstDir>/basename(srcPath) exists afterward. Fails without touching
// anything if that destination already exists.
bool copyEntry(const std::string& srcPath, const std::string& dstDir,
               std::string& error);

// Moves srcPath into dstDir. Tries rename() first (instant, same
// filesystem); falls back to copy-then-delete when that fails (e.g. the
// source and destination are visited through different mount points).
bool moveEntry(const std::string& srcPath, const std::string& dstDir,
               std::string& error);

// Renames a file or directory in place (same parent directory).
bool renameEntry(const std::string& path, const std::string& newName,
                 std::string& error);

bool createDirectory(const std::string& parentPath, const std::string& name,
                     std::string& error);

// "sdmc:/a/b/c" -> "sdmc:/a/b/". Stops at "sdmc:/" rather than going above
// the SD card root, the only volume this screen browses.
std::string explorerParentPath(const std::string& path);

std::string explorerJoinPath(const std::string& dir, const std::string& name);

// Last path component, without a trailing slash.
std::string explorerBaseName(const std::string& path);

}  // namespace pipensx
