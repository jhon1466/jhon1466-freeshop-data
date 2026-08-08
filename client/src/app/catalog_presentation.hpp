#pragma once

#include "catalog_service.hpp"
#include "game_metadata_service.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace pipensx {

struct CatalogPresentation {
    std::string title;
    std::string titleId;
    std::string iconUrl;
    bool iconPreserveAspect = false;
    std::string coverUrl;
    std::string description;
    std::string developer;
    std::string publisher;
    std::string releaseDate;
    std::string genre;
    std::vector<std::string> screenshots;
};

// One entry of the catalogue's player-mode menu. Any is always offered; the
// rest only when the loaded index has data for them.
enum class PlayerFilter {
    Any,
    Splitscreen,
    LocalCoop,
    Lan,
    Online,
};

// Which source wins for prose the catalogue and the metadata index both carry.
// The metadata index is English; the Langegen catalogue is Russian, so a
// Russian UI reads better from the catalogue. Only `description` differs:
// `releaseDate` is absent from every metadata snapshot we ship or fetch, so
// entry.year already wins unconditionally.
enum class TextPreference {
    Metadata,
    CatalogNative,
};

std::vector<std::string> mergeScreenshotUrls(
    const GameMetadata* metadata, const CatalogEntry& entry,
    size_t limit = 6);

CatalogPresentation resolveCatalogPresentation(
    const CatalogEntry& entry, const GameMetadata* metadata,
    TextPreference preference = TextPreference::Metadata);

bool catalogEntryIsGame(const CatalogEntry& entry,
                        const GameMetadata* metadata);

bool catalogEntryHasMatchedTitle(const GameMetadata* metadata);

// Applies the same metadata-over-catalogue text preference resolveCatalogPresentation
// uses for TextPreference::Metadata to a full entry list, for callers that need a flat
// CatalogEntry vector rather than the UI's per-entry CatalogPresentation — e.g. the web
// companion's /api/catalog endpoint, which otherwise serves the Langegen catalogue's raw
// (Russian) description straight through.
std::vector<CatalogEntry> withPreferredDescriptions(
    const std::vector<CatalogEntry>& entries,
    const GameMetadataService& metadata);

} // namespace pipensx
