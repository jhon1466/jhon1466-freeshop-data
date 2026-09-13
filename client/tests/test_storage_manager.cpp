#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "app/storage_manager.hpp"

namespace {

std::string tempRoot(const char* tag) {
    std::string templ = "/tmp/pipensx-storagemgr-";
    templ += tag;
    templ += "-XXXXXX";
    char* created = mkdtemp(templ.data());
    assert(created != nullptr);
    return created;
}

void makeDir(const std::string& path) {
    assert(mkdir(path.c_str(), 0755) == 0);
}

void writeFile(const std::string& path, const std::string& bytes) {
    FILE* out = fopen(path.c_str(), "wb");
    assert(out != nullptr);
    assert(fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size());
    fclose(out);
}

void testClearOrphanDownloads() {
    const std::string root = tempRoot("orphandl");
    const std::string downloads = root + "/downloads";
    makeDir(downloads);
    // Active task directory: must survive with its payload intact.
    makeDir(downloads + "/Game-aaaaaaaa");
    writeFile(downloads + "/Game-aaaaaaaa/payload.bin", "1234567890");
    // Orphaned task directory from a crash/interrupted remove.
    makeDir(downloads + "/Stale-bbbbbbbb");
    writeFile(downloads + "/Stale-bbbbbbbb/chunk.bin", "12345");
    // Stray file directly under downloads/.
    writeFile(downloads + "/stray.bin", "123");
    const std::vector<std::string> active{downloads + "/Game-aaaaaaaa"};
    // Estimate matches the bytes about to be recovered (5 + 3).
    assert(pipensx::orphanDownloadBytes(downloads, active) == 5 + 3);
    uint64_t recovered = 0;
    std::string error;
    assert(pipensx::clearOrphanDownloads(downloads, active, error, recovered));
    assert(error.empty());
    assert(recovered == 5 + 3);
    // Active payload intact; orphans gone.
    uint64_t kept = 0;
    assert(pipensx::directorySize(downloads + "/Game-aaaaaaaa/payload.bin",
                                  kept));
    assert(kept == 10);
    assert(access((downloads + "/Stale-bbbbbbbb").c_str(), F_OK) != 0);
    assert(access((downloads + "/stray.bin").c_str(), F_OK) != 0);
    // Second sweep is a no-op success; basename fallback keeps a task whose
    // app root moved but whose leaf name survived.
    assert(pipensx::orphanDownloadBytes(downloads, active) == 0);
    const std::vector<std::string> movedActive{"/other/root/Game-aaaaaaaa"};
    assert(pipensx::orphanDownloadBytes(downloads, movedActive) == 0);
    // Missing downloads directory: estimate 0, clear succeeds with 0.
    const std::string missing = root + "/no-downloads";
    assert(pipensx::orphanDownloadBytes(missing, active) == 0);
    uint64_t none = 1;
    assert(pipensx::clearOrphanDownloads(missing, active, error, none));
    assert(none == 0);
    unlink((downloads + "/Game-aaaaaaaa/payload.bin").c_str());
    rmdir((downloads + "/Game-aaaaaaaa").c_str());
    rmdir(downloads.c_str());
    rmdir(root.c_str());
}

} // namespace

int main() {
    testClearOrphanDownloads();
    std::puts("storage manager tests passed");
    return 0;
}
