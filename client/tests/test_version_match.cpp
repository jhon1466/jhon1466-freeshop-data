#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "app/game_metadata_service.hpp"

namespace {

void writeTextFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

void testPreferVersionMatchPicksUpdateBundle() {
    // B3 (FF IV / FF V / Pokémon, photo_136): three titles, each with a
    // base bundle and an update bundle. The Updates hub must open the
    // bundle carrying the check's found version per title — same-title
    // bundles must never collapse onto one target.
    const std::string root = "/tmp/pipensx-metadata-versionpick-" +
        std::to_string(static_cast<long long>(getpid()));
    const std::string bundled = root + "/bundled.json";
    mkdir(root.c_str(), 0755);
    writeTextFile(
        bundled,
        "[{\"infoHash\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\","
        "\"titleId\":\"0100F41000000000\",\"name\":\"Fantasy IV Base\","
        "\"latestVersion\":\"327680\"},"
        "{\"infoHash\":\"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\","
        "\"titleId\":\"0100F41000000000\",\"name\":\"Fantasy IV Update\","
        "\"latestVersion\":\"458752\"},"
        "{\"infoHash\":\"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC\","
        "\"titleId\":\"0100F51000000000\",\"name\":\"Fantasy V Base\","
        "\"latestVersion\":\"393216\"},"
        "{\"infoHash\":\"DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD\","
        "\"titleId\":\"0100F51000000000\",\"name\":\"Fantasy V Update\","
        "\"latestVersion\":\"524288\"},"
        "{\"infoHash\":\"EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE\","
        "\"titleId\":\"0100C0DE00000000\",\"name\":\"Pocket Beasts Base\","
        "\"latestVersion\":\"131072\"},"
        "{\"infoHash\":\"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\","
        "\"titleId\":\"0100C0DE00000000\",\"name\":\"Pocket Beasts Update\","
        "\"latestVersion\":\"262144\"}]");

    pipensx::GameMetadataService metadata(root, bundled);
    std::string error;
    assert(metadata.load(error));

    struct TitleCase {
        const char* titleId;
        const char* baseHash;
        const char* updateHash;
        const char* updateVersion;
    };
    const TitleCase cases[] = {
        {"0100F41000000000",
         "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
         "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB", "458752"},
        {"0100F51000000000",
         "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC",
         "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD", "524288"},
        {"0100C0DE00000000",
         "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE",
         "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", "262144"},
    };
    for (const TitleCase& tc : cases) {
        // Each title folds to its own target — never a neighbour's.
        std::vector<std::string> versions;
        assert(metadata.collectLatestVersions(tc.titleId, versions));
        assert(versions.size() == 2);
        assert(versions[0] == tc.updateVersion);
        std::vector<const pipensx::GameMetadata*> entries;
        assert(metadata.findByTitleId(tc.titleId, entries));
        assert(entries.size() == 2);
        // The update check's found version selects the update bundle.
        const pipensx::GameMetadata* match =
            pipensx::GameMetadataService::preferVersionMatch(entries,
                                                             tc.updateVersion);
        assert(match);
        assert(match->infoHash == tc.updateHash);
        assert(match->infoHash != tc.baseHash);
        // Anything else keeps the legacy pick (nullptr = caller falls
        // back to newest-published).
        assert(pipensx::GameMetadataService::preferVersionMatch(entries, "") ==
               nullptr);
        assert(pipensx::GameMetadataService::preferVersionMatch(entries, "0") ==
               nullptr);
        assert(pipensx::GameMetadataService::preferVersionMatch(
                   entries, "12abc") == nullptr);
        assert(pipensx::GameMetadataService::preferVersionMatch(
                   entries, "999999999") == nullptr);
        // A neighbour title's target never matches this title's bundles.
        for (const TitleCase& other : cases) {
            if (other.titleId == std::string(tc.titleId))
                continue;
            assert(pipensx::GameMetadataService::preferVersionMatch(
                       entries, other.updateVersion) == nullptr);
        }
    }
    std::vector<const pipensx::GameMetadata*> empty;
    assert(pipensx::GameMetadataService::preferVersionMatch(empty, "458752") ==
           nullptr);

    unlink(bundled.c_str());
    rmdir((root + "/catalog/metadata").c_str());
    rmdir((root + "/catalog/images").c_str());
    rmdir((root + "/catalog").c_str());
    rmdir(root.c_str());
}

} // namespace

int main() {
    testPreferVersionMatchPicksUpdateBundle();
    std::puts("version match tests passed");
    return 0;
}
