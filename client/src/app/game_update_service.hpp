#pragma once

#include "catalog_service.hpp"
#include "download_manager.hpp"
#include "installed_title_service.hpp"
#include "game_metadata_service.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <unordered_map>

namespace pipensx {

class GameMetadataService;
class InstalledTitleService;
class DownloadManager;
class CatalogService;

enum class GameUpdateState {
    NotChecked,
    Checking,
    UpdateAvailable,
    UpToDate,
    CheckError,
    SourceUnknown
};

struct GameUpdateResult {
    std::string titleId;
    std::string latestVersion;
    std::string updateMagnet;
    std::vector<const GameMetadata*> entries;
    std::string error;
    bool ok = false;
    std::string foundVersion;  // versión encontrada en el catálogo
    GameUpdateState state = GameUpdateState::NotChecked;
};

using GameUpdateResults = std::unordered_map<std::string, GameUpdateResult>;

struct GameUpdateService {
    explicit GameUpdateService(GameMetadataService* metadata,
                               const std::string& cachePath);

    bool load(std::string& error);

    // Finds all available updates for installed titles.
    void checkAll(const std::vector<InstalledTitle>& installed,
                  uint64_t installedGen,
                  uint64_t lastMetadataRefreshMs,
                  std::string& error);

    // Update results are stable for the current generation.
    const GameUpdateResults& results() const { return results_; }
    uint64_t generation() const { return generation_; }

    // Re-check a single title id on demand.
    void checkOne(const std::string& titleId,
                  const std::string& installedVersion,
                  std::string& error,
                  GameUpdateResult& result);

    // Whether the update cache is stale for the current generation.
    bool stale(uint64_t installedGen, uint64_t lastMetadataRefreshMs) const;

    // Ignored titles stay out of the Updates section and the update badge;
    // the choice persists in the same state file as the check results.
    bool isIgnored(const std::string& titleId) const;
    void setIgnored(const std::string& titleId, bool ignored,
                    std::string& error);

    // UpdateAvailable titles that are not ignored.
    size_t availableCount(const std::vector<InstalledTitle>& titles) const;

    // Persist the check results and the ignored set. Called by checkAll /
    // setIgnored; exposed for the UI when a re-check happens elsewhere.
    bool save(std::string& error) const;

    // Called by the update-file chooser when it resolves the selected
    // file actions into a DownloadTaskSpec.
    std::vector<uint8_t> buildUpdateActions(const std::string& titleId,
                                            const TorrentPreview& preview,
                                            const std::string& latestVersion,
                                            const std::vector<std::string>& installedDlcIds);

    // Resolve the update torrent's magnet URI for the given info hash.
    std::string resolveUpdateMagnet(const std::string& infoHash) const;

private:
    struct UpdateCheckResult {
        std::string titleId;
        std::string latestVersion;
        std::string updateMagnet;
        std::vector<const GameMetadata*> entries;
        std::string error;
        bool ok = false;
    };

    GameMetadataService* metadata_;
    InstalledTitleService* installed_ = nullptr;
    std::string cachePath_;
    GameUpdateResults results_;
    std::set<std::string> ignored_;
    uint64_t generation_ = 0;
    uint64_t lastMetadataRefreshMs_ = 0;

    bool findUpdate(const std::string& baseTitleId,
                    const std::string& installedVersion,
                    const GameMetadata* meta,
                    GameUpdateResult& result);
};

} // namespace pipensx