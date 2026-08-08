#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pipensx {

struct ReleaseInfo {
    std::string version;
    std::string nroUrl;
    std::string checksumUrl;
    // GitHub release body (changelog). Shown by the post-update "what's new"
    // screen once the new binary confirms itself on the next launch.
    std::string notes;
};

struct UpdateCheckResult {
    bool ok = false;
    bool updateAvailable = false;
    ReleaseInfo release;
    std::string error;
};

// Retrieves published releases from the canonical GitHub repository. A
// verified release is staged beside the active NRO, then a minimal helper
// finalizes the replacement after the original process has exited.
class UpdateService {
public:
    using MetadataFetcher = std::function<bool(
        const std::string&, size_t, std::string&, std::string&)>;
    using AssetFetcher = std::function<bool(
        const std::string&, const std::string&, size_t, std::string&)>;
    using CheckCallback = std::function<void(UpdateCheckResult)>;
    using InstallCallback = std::function<void(bool, std::string)>;
    using ProgressCallback = std::function<void(uint64_t, uint64_t)>;

    explicit UpdateService(
        std::string targetPath = "sdmc:/switch/freeshop-client/freeshop-client.nro",
        MetadataFetcher metadataFetcher = {}, AssetFetcher assetFetcher = {},
        std::string helperSourcePath = "romfs:/freeshop-client-updater.nro");
    ~UpdateService();

    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    UpdateCheckResult check() const;
    bool install(const ReleaseInfo& release, std::string& error) const;
    bool stagedReady() const;
    bool hasPendingConfirmation() const;
    bool confirmInstalled(std::string& error) const;
    void discardStaged() const;
    const std::string& targetPath() const { return targetPath_; }
    std::string stagedPath() const { return targetPath_ + ".update"; }
    std::string backupPath() const { return targetPath_ + ".previous"; }
    // Release notes staged alongside the update, read back by the "what's
    // new" screen once confirmInstalled() lands on the next launch.
    std::string notesPath() const { return targetPath_ + ".update-notes.txt"; }
    const std::string& helperPath() const { return helperPath_; }
    // Re-copies the helper from this (currently running) build's own romfs
    // over whatever sits at helperPath() on the SD card, verifying the copy
    // by checksum. install() already does this for a freshly staged update;
    // this is the same step, callable on its own right before offering to
    // run a helper staged by a PREVIOUS run - which may have shipped with a
    // bug since fixed upstream (see main_switch.cpp's pending-update-confirm
    // prompt). A stale helper left over from an old attempt would otherwise
    // never get refreshed just by replacing the main .nro.
    bool refreshHelper(std::string& error) const;
    void checkAsync(CheckCallback callback);
    bool checkCompleted() const { return checkCompleted_.load(); }
    void installAsync(ReleaseInfo release, InstallCallback callback);
    void cancel();
    void shutdown();
    // Reports (received, total) from the built-in NRO downloader on a worker
    // thread. Set it before installAsync; the callback must marshal to the UI
    // thread itself.
    void onInstallProgress(ProgressCallback callback) {
        progress_ = std::move(callback);
    }

    static bool isNewerVersion(const std::string& candidate,
                               const std::string& current);
    static bool parseRelease(const std::string& json, ReleaseInfo& release,
                             std::string& error);

private:
    bool publishHelper(std::string& error) const;

    std::string targetPath_;
    std::string helperPath_;
    std::string helperSourcePath_;
    MetadataFetcher metadataFetcher_;
    AssetFetcher assetFetcher_;
    ProgressCallback progress_;
    std::vector<std::thread> workers_;
    mutable std::atomic<bool> stopping_{false};
    mutable std::mutex stopMutex_;
    mutable std::condition_variable stopReady_;
    std::atomic<bool> checkCompleted_{false};
};

} // namespace pipensx
