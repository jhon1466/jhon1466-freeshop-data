#include "game_update_service.hpp"
#include "game_update_install.hpp"
#include "installed_title_service.hpp"
#include "catalog_service.hpp"
#include "game_metadata_service.hpp"
#include "magnet_resolver.hpp"
#include "download_manager.hpp"
#include "catalog_presentation.hpp"

#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <unistd.h>

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

constexpr int kStateVersion = 1;

bool parseState(const std::string& name, GameUpdateState& out) {
    if (name == "update_available") {
        out = GameUpdateState::UpdateAvailable;
        return true;
    }
    if (name == "up_to_date") {
        out = GameUpdateState::UpToDate;
        return true;
    }
    if (name == "source_unknown") {
        out = GameUpdateState::SourceUnknown;
        return true;
    }
    if (name == "check_error") {
        out = GameUpdateState::CheckError;
        return true;
    }
    if (name == "not_checked") {
        out = GameUpdateState::NotChecked;
        return true;
    }
    return false;
}

const char* stateName(GameUpdateState state) {
    switch (state) {
        case GameUpdateState::NotChecked: return "not_checked";
        case GameUpdateState::Checking: return "checking";
        case GameUpdateState::UpToDate: return "up_to_date";
        case GameUpdateState::UpdateAvailable: return "update_available";
        case GameUpdateState::SourceUnknown: return "source_unknown";
        case GameUpdateState::CheckError: return "check_error";
    }
    return "not_checked";
}
} // namespace

GameUpdateService::GameUpdateService(GameMetadataService* metadata,
                                     const std::string& cachePath)
    : metadata_(metadata), cachePath_(cachePath) {}

bool GameUpdateService::load(std::string& error) {
    error.clear();
    results_.clear();
    ignored_.clear();
    generation_ = 0;
    lastMetadataRefreshMs_ = 0;

    std::ifstream input(cachePath_ + "/game_updates.json", std::ios::binary);
    if (!input) {
        // First run: no state file yet.
        return true;
    }

    std::string data((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    nlohmann::json root = nlohmann::json::parse(data, nullptr, false);
    if (root.is_discarded() || !root.is_object() ||
        root.value("version", 0) != kStateVersion) {
        error = "Game-update state file is not valid.";
        return false;
    }
    if (root.contains("results") && root["results"].is_array()) {
        for (const auto& item : root["results"]) {
            if (!item.is_object())
                continue;
            const std::string titleId = item.value("title_id", "");
            if (titleId.empty())
                continue;
            GameUpdateResult result;
            if (!parseState(item.value("state", ""), result.state))
                continue;
            result.titleId = titleId;
            result.latestVersion = item.value("latest_version", "");
            result.foundVersion = item.value("found_version", "");
            result.error = item.value("error", "");
            result.ok = result.state == GameUpdateState::UpdateAvailable;
            results_[titleId] = std::move(result);
        }
    }
    if (root.contains("ignored_title_ids") &&
        root["ignored_title_ids"].is_array()) {
        for (const auto& item : root["ignored_title_ids"]) {
            if (!item.is_string())
                continue;
            const std::string titleId = item.get<std::string>();
            if (!titleId.empty())
                ignored_.insert(titleId);
        }
    }
    return true;
}

bool GameUpdateService::save(std::string& error) const {
    error.clear();
    nlohmann::json root = nlohmann::json::object();
    root["version"] = kStateVersion;
    nlohmann::json list = nlohmann::json::array();
    for (const auto& entry : results_) {
        nlohmann::json item = nlohmann::json::object();
        item["title_id"] = entry.first;
        item["state"] = stateName(entry.second.state);
        item["latest_version"] = entry.second.latestVersion;
        item["found_version"] = entry.second.foundVersion;
        item["error"] = entry.second.error;
        list.push_back(std::move(item));
    }
    root["results"] = std::move(list);
    nlohmann::json ignored = nlohmann::json::array();
    for (const std::string& titleId : ignored_)
        ignored.push_back(titleId);
    root["ignored_title_ids"] = std::move(ignored);

    const std::string path = cachePath_ + "/game_updates.json";
    const std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "Unable to create game-update state file.";
            return false;
        }
        output << root.dump(2) << "\n";
        output.flush();
        if (!output.good()) {
            unlink(temporary.c_str());
            error = "Unable to write game-update state file.";
            return false;
        }
    }
    if (rename(temporary.c_str(), path.c_str()) == 0)
        return true;

    // Same fallback as AppSettings::write: some FAT drivers refuse to rename
    // over an existing file.
    int renameError = errno;
    if ((unlink(path.c_str()) == 0 || errno == ENOENT) &&
        rename(temporary.c_str(), path.c_str()) == 0) {
        return true;
    }
    int finalError = errno;
    unlink(temporary.c_str());
    error = std::string("Unable to replace game-update state file: ") +
            std::strerror(finalError ? finalError : renameError);
    return false;
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
    error.clear();
    std::string saveError;
    if (!save(saveError))
        error = std::move(saveError);
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

bool GameUpdateService::isIgnored(const std::string& titleId) const {
    return ignored_.count(titleId) != 0;
}

void GameUpdateService::setIgnored(const std::string& titleId, bool ignored,
                                   std::string& error) {
    error.clear();
    if (ignored)
        ignored_.insert(titleId);
    else
        ignored_.erase(titleId);
    // The cached verdict stays in place so un-ignore flips the row straight
    // back into the Updates section; every query filters by isIgnored().
    std::string saveError;
    if (!save(saveError))
        error = std::move(saveError);
}

size_t GameUpdateService::availableCount(
    const std::vector<InstalledTitle>& titles) const {
    size_t count = 0;
    for (const InstalledTitle& title : titles) {
        if (ignored_.count(title.titleId))
            continue;
        auto it = results_.find(title.titleId);
        if (it != results_.end() &&
            it->second.state == GameUpdateState::UpdateAvailable)
            ++count;
    }
    return count;
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