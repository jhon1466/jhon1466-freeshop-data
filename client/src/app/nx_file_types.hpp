#pragma once

#include <cctype>
#include <cstdint>
#include <string>

namespace pipensx {

// Case-insensitive test that `name` ends with the 4-character extension
// `ext4` (e.g. ".nsp"). Shared by every Switch file-name classifier so the
// recognized extensions live in exactly one place.
inline bool hasFileExtension(const std::string& name, const char* ext4) {
    if (name.size() < 4)
        return false;
    const size_t base = name.size() - 4;
    for (int i = 0; i < 4; ++i)
        if (static_cast<char>(std::tolower(
                static_cast<unsigned char>(name[base + i]))) != ext4[i])
            return false;
    return true;
}

// Installable NSP/NSZ package.
inline bool isPackageName(const std::string& name) {
    return hasFileExtension(name, ".nsp") || hasFileExtension(name, ".nsz");
}

// XCI/XCZ cartridge dump.
inline bool isCartridgeName(const std::string& name) {
    return hasFileExtension(name, ".xci") || hasFileExtension(name, ".xcz");
}

// Zstd-compressed installable package (.nsz). Used for space estimation,
// where a compressed package has an unknown expanded size.
inline bool isCompressedName(const std::string& name) {
    return hasFileExtension(name, ".nsz");
}

// Port payload archive sitting next to NSP forwarders: switch.7z / switch.zip.
// "switch.7z" is 9 chars — do not gate on length or it is silently dropped.
inline bool isPortArchiveName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const std::string base =
        slash == std::string::npos ? path : path.substr(slash + 1);
    std::string lower = base;
    for (char& ch : lower)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return lower == "switch.7z" || lower == "switch.zip";
}

inline bool hasNroExtension(const std::string& path) {
    return hasFileExtension(path, ".nro");
}

inline bool isSwitchPathComponent(const std::string& value) {
    const char expected[] = "switch";
    if (value.size() != 6)
        return false;
    for (size_t i = 0; i < 6; ++i)
        if (static_cast<char>(std::tolower(
                static_cast<unsigned char>(value[i]))) != expected[i])
            return false;
    return true;
}

inline bool pathContainsSwitchComponent(const std::string& path) {
    size_t start = 0;
    while (start < path.size()) {
        const size_t slash = path.find_first_of("/\\", start);
        const std::string component = path.substr(
            start, slash == std::string::npos ? std::string::npos
                                               : slash - start);
        if (isSwitchPathComponent(component))
            return true;
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return false;
}

inline bool isPortPayloadName(const std::string& path) {
    return isPortArchiveName(path) || pathContainsSwitchComponent(path);
}

inline bool isRarName(const std::string& name) {
    return hasFileExtension(name, ".rar");
}

inline bool isCompressedArchiveName(const std::string& name) {
    return isPortArchiveName(name) || isRarName(name);
}

// Patch NSPs are tagged [vN] with N>0, named update/patch, or carry the
// …800 title id. Lower rank installs first so a dump that lists the patch
// before the [v0] base still commits the application package first.
inline bool pathLooksLikeUpdatePackage(const std::string& path) {
    std::string lower;
    lower.resize(path.size());
    for (size_t i = 0; i < path.size(); ++i)
        lower[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(path[i])));
    if (lower.find("update") != std::string::npos ||
        lower.find("patch") != std::string::npos)
        return true;
    for (size_t i = 0; i + 2 < lower.size(); ++i) {
        if (lower[i] == '[' && lower[i + 1] == 'v' &&
            lower[i + 2] >= '1' && lower[i + 2] <= '9')
            return true;
    }
    for (size_t i = 0; i + 16 <= path.size(); ++i) {
        uint64_t id = 0;
        bool hex = true;
        for (size_t j = 0; j < 16; ++j) {
            const unsigned char ch = static_cast<unsigned char>(path[i + j]);
            unsigned digit = 0;
            if (ch >= '0' && ch <= '9')
                digit = ch - '0';
            else if (ch >= 'a' && ch <= 'f')
                digit = 10 + (ch - 'a');
            else if (ch >= 'A' && ch <= 'F')
                digit = 10 + (ch - 'A');
            else {
                hex = false;
                break;
            }
            id = (id << 4) | digit;
        }
        if (hex && (id & 0xFFFULL) == 0x800ULL)
            return true;
    }
    return false;
}

inline int packageInstallRank(const std::string& path) {
    if (!isPackageName(path))
        return 2;
    return pathLooksLikeUpdatePackage(path) ? 1 : 0;
}

} // namespace pipensx
