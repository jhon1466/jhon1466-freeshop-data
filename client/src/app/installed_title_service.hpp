#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pipensx {

struct InstalledTitle {
    uint64_t applicationId = 0;
    std::string titleId;
    std::string name;
    std::string publisher;
    // Installed title version as a decimal string: the title version of the
    // installed Patch content meta read from ncm (0 when no patch is
    // installed). Compared against the metadata index's latestVersion by the
    // game-update check; empty when the ncm read is unavailable.
    std::string version;
    std::string iconPath;
};

class InstalledTitleService {
public:
    explicit InstalledTitleService(std::string rootPath);

    bool refresh(std::string& error);
    bool contains(const std::string& titleId) const;
    // Uninstalls the application (nsDeleteApplicationCompletely) and
    // refreshes titles() afterward. `refreshError` carries a soft failure
    // from that follow-up refresh (uninstall itself still succeeded); the
    // 2-arg overload folds it into `error` for simpler call sites.
    bool uninstall(const std::string& titleId, std::string& error,
                   std::string& refreshError);
    bool uninstall(const std::string& titleId, std::string& error);

    std::vector<InstalledTitle> titles() const;
    std::vector<std::string> dlcTitleIds() const;
    size_t dlcCountForBase(const std::string& titleId) const;
    uint64_t generation() const;
    const std::string& rootPath() const { return rootPath_; }
    // Golden-runner seam: the PC shim reports an empty library, but the
    // installed-populated screen needs rows to pin the update chips. Replaces
    // the enumerated set like a refresh would (generation bumps).
    void injectTitles(std::vector<InstalledTitle> titles);
    void injectDlcTitleIds(std::vector<std::string> dlcTitleIds);

    static std::string formatTitleId(uint64_t applicationId);
    // Updates set bit 11 (…800); DLC lives in the low 12 bits from …1000.
    // Masking those bits maps every variant onto the base application id.
    static uint64_t nxBaseApplicationId(uint64_t applicationId) {
        return applicationId & ~0x1FFFULL;
    }

private:
    std::string rootPath_;
    std::string iconRoot_;
    std::mutex refreshMutex_;
    mutable std::mutex mutex_;
    std::vector<InstalledTitle> titles_;
    std::unordered_set<std::string> titleIds_;
    std::unordered_set<std::string> dlcTitleIds_;
    uint64_t generation_ = 0;
};

// Maps an application id onto the Patch content-meta version string used by
// game-update checks. Empty when the ncm scan was incomplete (CheckError);
// "0" when the scan succeeded and no patch is installed.
inline std::string installedPatchVersionString(
    uint64_t applicationId,
    const std::unordered_map<uint64_t, uint32_t>& patchVersions,
    bool patchMetaComplete) {
    if (!patchMetaComplete)
        return {};
    const auto patch = patchVersions.find(applicationId | 0x800ULL);
    return patch == patchVersions.end() ? "0"
                                        : std::to_string(patch->second);
}

} // namespace pipensx
