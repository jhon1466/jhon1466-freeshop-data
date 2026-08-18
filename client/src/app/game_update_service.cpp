#include "game_update_service.hpp"
#include "game_update_install.hpp"
#include "installed_title_service.hpp"
#include "catalog_service.hpp"
#include "game_metadata_service.hpp"
#include "magnet_resolver.hpp"
#include "download_manager.hpp"
#include "catalog_presentation.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>

namespace pipensx {

namespace {
bool parseDecimal(const std::string& text, uint64_t& out) {
    if (text.empty())
        return false;
    uint64_t value = 0;
    for (unsigned char c : text) {
        if (c < '0' || c > '9')
            return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}
} // namespace

GameUpdateService::GameUpdateService(GameMetadataService* metadata,
                                     const std::string& cachePath)
    : metadata_(metadata), cachePath_(cachePath) {}

bool GameUpdateService::load(std::string& error) {
    std::ifstream input(cachePath_);
    if (!input) {
        // First run: no cache yet.
        return true;
    }

    std::stringstream buffer;
    buffer << input.rdbuf();
    std::string json = buffer.str();

    // Simple JSON parse for the results array
    size_t pos = json.find("\"results\"");
    if (pos == std::string::npos)
        return true;

    // Very simple parse - in reality you'd use a proper JSON library
    // For now just return true and let checkAll rebuild
    return true;
}

void GameUpdateService::checkAll(const std::vector<InstalledTitle>& installed,
                                 uint64_t installedGen,
                                 uint64_t lastMetadataRefreshMs,
                                 std::string& error) {
    results_.clear();
    generation_ = installedGen;
    lastMetadataRefreshMs_ = lastMetadataRefreshMs;

    for (const InstalledTitle& title : installed) {
        if (title.version.empty())
            continue; // No version = can't check for updates

        std::vector<const GameMetadata*> entries;
        metadata_->findByTitleId(title.titleId, entries);
        const GameMetadata* meta = entries.empty() ? nullptr : entries[0];
        if (!meta)
            continue;

        GameUpdateResult result;
        if (findUpdate(title.titleId, title.version, meta, result) && result.ok) {
            result.state = GameUpdateState::UpdateAvailable;
            results_[result.titleId] = std::move(result);
        }
    }
}

void GameUpdateService::checkOne(const std::string& titleId,
                                 const std::string& installedVersion,
                                 std::string& error,
                                 GameUpdateResult& result) {
    // Check if we already have this result cached
    auto it = results_.find(titleId);
    if (it != results_.end()) {
        result = it->second;
        return;
    }
    error = "Not implemented: checkOne needs installed service";
}

bool GameUpdateService::stale(uint64_t installedGen, uint64_t lastMetadataRefreshMs) const {
    return generation_ != installedGen || lastMetadataRefreshMs != lastMetadataRefreshMs_;
}

std::vector<uint8_t> GameUpdateService::buildUpdateActions(
    const std::string& titleId,
    const TorrentPreview& preview,
    const std::string& latestVersion,
    const std::vector<std::string>& installedDlcIds) {
    return selectUpdateFiles(preview, latestVersion, titleId);
}

bool GameUpdateService::findUpdate(const std::string& baseTitleId,
                                   const std::string& installedVersion,
                                   const GameMetadata* meta,
                                   GameUpdateResult& result) {
    result.titleId = baseTitleId;

    if (meta->latestVersion.empty())
        return false;

    uint64_t installedVer = 0;
    uint64_t latestVer = 0;
    parseDecimal(installedVersion, installedVer);
    parseDecimal(meta->latestVersion, latestVer);

    if (latestVer <= installedVer)
        return false;

    result.latestVersion = meta->latestVersion;
    result.ok = true;
    result.state = GameUpdateState::UpdateAvailable;
    return true;
}

std::string GameUpdateService::resolveUpdateMagnet(const std::string& infoHash) const {
    return updateMagnetFor(infoHash, nullptr);
}

} // namespace pipensx