#include "game_update_install.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace pipensx {
namespace {

// Strict decimal parse ("131072"); rejects signs, whitespace and overflow —
// strtoull would happily turn "1.2.3" into 1 and match a [v1] package.
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

// First "[vN]" numeric tag in a file name
// ("Minecraft [0100D71004694800][v10092544].nsp" -> 10092544). Returns false
// when the name carries no numeric [vN] tag.
bool fileVersionTag(const std::string& path, uint64_t& value) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    for (size_t i = 0; i + 2 < lower.size(); ++i) {
        if (lower[i] == '[' && lower[i + 1] == 'v' &&
            lower[i + 2] >= '0' && lower[i + 2] <= '9') {
            char* end = nullptr;
            const unsigned long long v =
                strtoull(lower.c_str() + i + 2, &end, 10);
            if (end != lower.c_str() + i + 2) {
                value = static_cast<uint64_t>(v);
                return true;
            }
        }
    }
    return false;
}

bool isUpdateFile(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (lower.find("update") != std::string::npos ||
        lower.find("patch") != std::string::npos ||
        lower.find("upd") != std::string::npos)
        return true;
    // "[vN]" with a non-zero N — release bundles tag the bundled update
    // version in the file name, and the base package usually carries "[v0]".
    for (size_t i = 0; i + 2 < lower.size(); ++i) {
        if (lower[i] == '[' && lower[i + 1] == 'v' &&
            lower[i + 2] >= '1' && lower[i + 2] <= '9')
            return true;
    }
    return false;
}

// Release names put the 16-hex title id next to [vN]. Empty titleId skips
// the check so older call sites / tests keep matching on version alone.
bool pathHasTitleId(const std::string& path, const std::string& titleId) {
    if (titleId.empty())
        return true;
    std::string lowerPath = path;
    std::string lowerId = titleId;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (lowerPath.find(lowerId) != std::string::npos)
        return true;
    // Patch packages carry the …800 update id while callers pass the …000
    // base id (combo dumps, installed-view updates). Normalize both sides
    // onto the base application id before comparing.
    auto parseHex16 = [](const std::string& text, size_t at, uint64_t& out) {
        if (at + 16 > text.size())
            return false;
        uint64_t value = 0;
        for (size_t i = 0; i < 16; ++i) {
            const unsigned char c =
                static_cast<unsigned char>(text[at + i]);
            unsigned digit = 0;
            if (c >= '0' && c <= '9')
                digit = c - '0';
            else if (c >= 'a' && c <= 'f')
                digit = 10 + (c - 'a');
            else
                return false;
            value = (value << 4) | digit;
        }
        out = value;
        return true;
    };
    uint64_t wanted = 0;
    if (!parseHex16(lowerId, 0, wanted) || lowerId.size() != 16)
        return false;
    wanted &= ~0x1FFFULL;
    for (size_t i = 0; i + 16 <= lowerPath.size(); ++i) {
        uint64_t candidate = 0;
        if (parseHex16(lowerPath, i, candidate) &&
            (candidate & ~0x1FFFULL) == wanted)
            return true;
    }
    return false;
}

} // namespace

std::vector<size_t> updateVersionMatches(const TorrentPreview& preview,
                                         const std::string& latestVersion,
                                         const std::string& titleId) {
    std::vector<size_t> matches;
    uint64_t wanted = 0;
    if (!parseDecimal(latestVersion, wanted) || wanted == 0)
        return matches;
    for (size_t i = 0; i < preview.files.size(); ++i) {
        uint64_t tag = 0;
        if (preview.files[i].package &&
            pathHasTitleId(preview.files[i].path, titleId) &&
            fileVersionTag(preview.files[i].path, tag) && tag == wanted)
            matches.push_back(i);
    }
    return matches;
}

std::vector<uint8_t> selectFiles(const TorrentPreview& preview,
                                 const std::vector<size_t>& picks) {
    std::vector<uint8_t> actions(
        preview.files.size(), static_cast<uint8_t>(FileAction::Skip));
    for (const size_t i : picks) {
        if (i < actions.size())
            actions[i] = static_cast<uint8_t>(FileAction::Install);
    }
    return actions;
}

std::vector<uint8_t> selectUpdateFiles(const TorrentPreview& preview,
                                       const std::string& latestVersion,
                                       const std::string& titleId) {
    const std::vector<size_t> matches =
        updateVersionMatches(preview, latestVersion, titleId);
    if (!matches.empty())
        return selectFiles(preview, matches);

    std::vector<size_t> marked;
    for (size_t i = 0; i < preview.files.size(); ++i) {
        if (preview.files[i].package &&
            pathHasTitleId(preview.files[i].path, titleId) &&
            isUpdateFile(preview.files[i].path))
            marked.push_back(i);
    }
    if (!marked.empty()) {
        // No exact tag: install only the highest-tagged marked package, so a
        // stray marker cannot drag unrelated packages along.
        uint64_t bestTag = 0;
        bool haveBest = false;
        std::vector<size_t> best;
        for (const size_t i : marked) {
            uint64_t tag = 0;
            const bool hasTag = fileVersionTag(preview.files[i].path, tag);
            if (hasTag) {
                if (!haveBest || tag > bestTag) {
                    bestTag = tag;
                    haveBest = true;
                    best = {i};
                } else if (tag == bestTag) {
                    best.push_back(i);
                }
            } else if (!haveBest) {
                best.push_back(i);
            }
        }
        return selectFiles(preview, best.empty() ? marked : best);
    }

    // Nothing identifiable: leave everything Skip so the chooser opens with
    // no preselection (Continue stays disabled until the user picks).
    return selectFiles(preview, {});
}

std::string updateMagnetFor(const std::string& infoHash,
                            const CatalogEntry* entry) {
    if (entry && !entry->magnetUri.empty())
        return entry->magnetUri;
    // The metadata index is RuTracker-derived, and MagnetResolver only
    // accepts RuTracker trackers, so the fallback carries the canonical
    // mirror (resolveToFile bakes all mirrors into the announce list).
    return "magnet:?xt=urn:btih:" + infoHash +
           "&tr=http://bt.t-ru.org/ann?magnet";
}

UpdatePreflight describeUpdatePreflight(const InstalledTitle& installed,
                                         const std::string& latestVersion,
                                         bool titleInstalled) {
    UpdatePreflight pre;
    pre.titleInstalled = titleInstalled;
    pre.installedXyz = formatTitleVersion(installed.version);
    pre.targetXyz = formatTitleVersion(latestVersion);
    pre.displayVersion = installed.displayVersion;
    pre.hasMods = installed.hasLayeredFsMods;
    return pre;
}

std::string formatRequiredHosVersion(uint32_t requiredSystemVersion) {
    if (requiredSystemVersion == 0)
        return {};
    const unsigned major =
        static_cast<unsigned>((requiredSystemVersion >> 26) & 0x3f);
    const unsigned minor =
        static_cast<unsigned>((requiredSystemVersion >> 20) & 0x3f);
    const unsigned micro =
        static_cast<unsigned>((requiredSystemVersion >> 16) & 0xfu);
    return std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(micro);
}

uint32_t makeRequiredSystemVersion(unsigned major, unsigned minor,
                                   unsigned micro) {
    return ((static_cast<uint32_t>(major) & 0x3fu) << 26) |
           ((static_cast<uint32_t>(minor) & 0x3fu) << 20) |
           ((static_cast<uint32_t>(micro) & 0xfu) << 16);
}

bool updateRequiresNewerHos(uint32_t requiredSystemVersion,
                             unsigned hosMajor, unsigned hosMinor,
                             unsigned hosMicro) {
    if (requiredSystemVersion == 0)
        return false;
    const unsigned needMajor =
        static_cast<unsigned>((requiredSystemVersion >> 26) & 0x3f);
    const unsigned needMinor =
        static_cast<unsigned>((requiredSystemVersion >> 20) & 0x3f);
    const unsigned needMicro =
        static_cast<unsigned>((requiredSystemVersion >> 16) & 0xfu);
    if (needMajor != hosMajor)
        return needMajor > hosMajor;
    if (needMinor != hosMinor)
        return needMinor > hosMinor;
    return needMicro > hosMicro;
}

UpdateFailureHint classifyUpdateFailure(const std::string& installError,
                                        bool hasMods,
                                        uint32_t requiredSystemVersion,
                                        unsigned hosMajor, unsigned hosMinor,
                                        unsigned hosMicro) {
    std::string lower = installError;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    // "0x00000291" (ticket) contains "0291"; "0x291" is the short form.
    if (lower.find("0291") != std::string::npos ||
        lower.find("0x291") != std::string::npos ||
        lower.find("sys-patch") != std::string::npos ||
        lower.find("syspatch") != std::string::npos ||
        lower.find("sigpatch") != std::string::npos ||
        lower.find("sig-patch") != std::string::npos ||
        lower.find("signature patch") != std::string::npos)
        return UpdateFailureHint::SigPatches;
    if (updateRequiresNewerHos(requiredSystemVersion, hosMajor, hosMinor,
                               hosMicro))
        return UpdateFailureHint::NeedsNewHos;
    if (hasMods)
        return UpdateFailureHint::ModConflict;
    return UpdateFailureHint::None;
}

std::string formatUpdateFailureHint(UpdateFailureHint hint,
                                    const std::string& targetXyz,
                                    uint32_t requiredSystemVersion) {
    switch (hint) {
    case UpdateFailureHint::ModConflict: {
        std::string text =
            "If the game closes on launch after this update, remove the "
            "mods in atmosphere/contents/ for this title (or update the "
            "mods to match the new version) and try again.";
        if (!targetXyz.empty())
            text = "Update v" + targetXyz + " may conflict with the "
                     "installed mods. " + text;
        return text;
    }
    case UpdateFailureHint::NeedsNewHos: {
        const std::string need =
            formatRequiredHosVersion(requiredSystemVersion);
        if (!need.empty() && !targetXyz.empty())
            return "Update v" + targetXyz + " requires system " + need +
                   ". Update the firmware before launching it.";
        if (!need.empty())
            return "This update requires system " + need +
                   ". Update the firmware before launching it.";
        return "This update needs newer firmware. Update the firmware "
               "before launching it.";
    }
    case UpdateFailureHint::SigPatches:
        return "Title ticket import failed (0x291). Update sys-patch / "
               "sigpatches for this firmware, then reinstall the update.";
    case UpdateFailureHint::None:
    default:
        return {};
    }
}

} // namespace pipensx
