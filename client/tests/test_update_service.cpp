#include "app/update_service.hpp"

#include <cassert>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

constexpr const char* Target = "/tmp/pipensx-update-test.nro";
constexpr const char* HelperSource = "/tmp/freeshop-client-updater-fixture.nro";

bool emulateSwitchRename = false;

extern "C" int __real_rename(const char* oldPath, const char* newPath);

extern "C" int __wrap_rename(const char* oldPath, const char* newPath) {
    if (emulateSwitchRename) {
        if (std::strcmp(oldPath, Target) == 0) {
            errno = EACCES;
            return -1;
        }
        if (std::strcmp(newPath, Target) == 0 && access(Target, F_OK) == 0) {
            errno = EEXIST;
            return -1;
        }
    }
    return __real_rename(oldPath, newPath);
}

std::string releaseJson(const std::string& version, bool checksum = true) {
    return "{\"draft\":false,\"prerelease\":false,\"tag_name\":\"" +
           version + "\",\"assets\":[{\"name\":\"freeshop-client.nro\","
           "\"browser_download_url\":\"https://freeshop-proxy.freeshopnx."
           "workers.dev/releases/assets/1001\"}" +
           (checksum ? ",{\"name\":\"freeshop-client.nro.sha256\","
            "\"browser_download_url\":\"https://freeshop-proxy.freeshopnx."
            "workers.dev/releases/assets/1002\"}" : "") +
           "]}";
}

void write(const std::string& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
}

void testVersionsAndReleaseValidation() {
    assert(pipensx::UpdateService::isNewerVersion("v1.2.3", "1.2.2"));
    assert(!pipensx::UpdateService::isNewerVersion("1.2.3", "1.2.3"));
    assert(!pipensx::UpdateService::isNewerVersion("next", "1.2.3"));
    pipensx::ReleaseInfo release;
    std::string error;
    assert(pipensx::UpdateService::parseRelease(releaseJson("v1.2.3"),
                                                release, error));
    assert(release.version == "v1.2.3");
    assert(!pipensx::UpdateService::parseRelease(releaseJson("v1.2.3", false),
                                                 release, error));
}

void testStagedReadyRecoversInterruptedRestart() {
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
    unlink("/tmp/pipensx-update-test.nro.update.sha256");
    unlink("/tmp/freeshop-client-updater.nro");
    write(Target, "old");
    write(HelperSource, "minimal updater helper");
    const std::string payload = "new verified pipensx nro";
    const std::string checksum =
        "9dc1034a694baa2d68b032ed446fbc9b170975306c571202e99ff741821365b4";
    pipensx::UpdateService service(Target,
        [checksum](const std::string&, size_t, std::string& body,
                   std::string&) { body = checksum + "  freeshop-client.nro\n"; return true; },
        [payload](const std::string&, const std::string& path, size_t,
                  std::string&) { write(path, payload); return true; },
        HelperSource);
    assert(!service.stagedReady());
    pipensx::ReleaseInfo release{"v1.2.3",
        "https://freeshop-proxy.freeshopnx.workers.dev/releases/assets/1001",
        "https://freeshop-proxy.freeshopnx.workers.dev/releases/assets/1002"};
    std::string error;
    assert(service.install(release, error));
    // The app quit before chain-loading the helper: the staged download must
    // still be recognized as complete so the next launch can finalize it.
    assert(service.stagedReady());
    write("/tmp/pipensx-update-test.nro.update", "corrupted");
    assert(!service.stagedReady());
    write("/tmp/pipensx-update-test.nro.update", payload);
    assert(service.stagedReady());
    service.discardStaged();
    unlink(Target);
}

void testInstallVerifiesBeforeStaging() {
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
    unlink("/tmp/pipensx-update-test.nro.update.sha256");
    unlink("/tmp/freeshop-client-updater.nro");
    write(Target, "old");
    write(HelperSource, "minimal updater helper");
    const std::string payload = "new verified pipensx nro";
    const std::string checksum =
        "9dc1034a694baa2d68b032ed446fbc9b170975306c571202e99ff741821365b4";
    pipensx::UpdateService service(Target,
        [checksum](const std::string&, size_t, std::string& body,
                   std::string&) { body = checksum + "  freeshop-client.nro\n"; return true; },
        [payload](const std::string&, const std::string& path, size_t,
                  std::string&) { write(path, payload); return true; },
        HelperSource);
    pipensx::ReleaseInfo release{"v1.2.3",
        "https://freeshop-proxy.freeshopnx.workers.dev/releases/assets/1001",
        "https://freeshop-proxy.freeshopnx.workers.dev/releases/assets/1002"};
    std::string error;
    assert(service.install(release, error));
    std::ifstream installed("/tmp/pipensx-update-test.nro.update",
                            std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(installed)), {});
    assert(bytes == payload);
    std::ifstream current(Target, std::ios::binary);
    bytes.assign((std::istreambuf_iterator<char>(current)), {});
    assert(bytes == "old");
    std::ifstream marker("/tmp/pipensx-update-test.nro.update.sha256");
    bytes.assign((std::istreambuf_iterator<char>(marker)), {});
    assert(bytes == checksum + "\n");
    std::ifstream helper(service.helperPath(), std::ios::binary);
    bytes.assign((std::istreambuf_iterator<char>(helper)), {});
    assert(service.helperPath() == "/tmp/freeshop-client-updater.nro");
    assert(bytes == "minimal updater helper");
    installed.close();
    current.close();
    marker.close();
    helper.close();
    service.discardStaged();
    assert(access("/tmp/pipensx-update-test.nro.update", F_OK) != 0);
    assert(access(service.helperPath().c_str(), F_OK) != 0);

    write(Target, "old");
    pipensx::UpdateService wrongChecksum(Target,
        [](const std::string&, size_t, std::string& body,
           std::string&) { body = "0000000000000000000000000000000000000000000000000000000000000000\n"; return true; },
        [payload](const std::string&, const std::string& path, size_t,
                  std::string&) { write(path, payload); return true; },
        HelperSource);
    assert(!wrongChecksum.install(release, error));
    std::ifstream preserved(Target, std::ios::binary);
    bytes.assign((std::istreambuf_iterator<char>(preserved)), {});
    assert(bytes == "old");
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
    unlink("/tmp/pipensx-update-test.nro.update.sha256");
    unlink("/tmp/freeshop-client-updater.nro");
    unlink("/tmp/pipensx-update-test.nro.update.sha256");
    unlink(HelperSource);
}

void testTransientNetworkFailuresAreRetried() {
    int checkAttempts = 0;
    pipensx::UpdateService checkService(Target,
        [&checkAttempts](const std::string&, size_t, std::string& body,
                         std::string& error) {
            ++checkAttempts;
            if (checkAttempts < 3) {
                error = "Error de red de actualización: Timeout was reached";
                return false;
            }
            body = releaseJson("v9.9.9");
            return true;
        });
    const auto result = checkService.check();
    assert(result.ok);
    assert(checkAttempts == 3);

    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
    write(Target, "old");
    write(HelperSource, "minimal updater helper");
    const std::string payload = "new verified pipensx nro";
    const std::string checksum =
        "9dc1034a694baa2d68b032ed446fbc9b170975306c571202e99ff741821365b4";
    int checksumAttempts = 0;
    int downloadAttempts = 0;
    pipensx::UpdateService installService(Target,
        [&checksumAttempts, checksum](const std::string&, size_t,
                                      std::string& body,
                                      std::string& error) {
            ++checksumAttempts;
            if (checksumAttempts < 3) {
                error = "Error de red de actualización: Timeout was reached";
                return false;
            }
            body = checksum + "  freeshop-client.nro\n";
            return true;
        },
        [&downloadAttempts, payload](const std::string&,
                                     const std::string& path, size_t,
                                     std::string& error) {
            ++downloadAttempts;
            if (downloadAttempts < 3) {
                error = "Falló la descarga de actualización: Timeout was reached";
                return false;
            }
            write(path, payload);
            return true;
        }, HelperSource);
    pipensx::ReleaseInfo release{"v9.9.9",
        "https://freeshop-proxy.freeshopnx.workers.dev/releases/assets/2001",
        "https://freeshop-proxy.freeshopnx.workers.dev/releases/assets/2002"};
    std::string error;
    assert(installService.install(release, error));
    assert(checksumAttempts == 3);
    assert(downloadAttempts == 3);
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
}

void testInstallNeverTouchesRunningNro() {
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
    unlink("/tmp/pipensx-update-test.nro.update.sha256");
    unlink("/tmp/freeshop-client-updater.nro");
    unlink("/tmp/pipensx-update-test.nro.previous");
    write(Target, "old");
    write(HelperSource, "minimal updater helper");
    const std::string payload = "new verified pipensx nro";
    const std::string checksum =
        "9dc1034a694baa2d68b032ed446fbc9b170975306c571202e99ff741821365b4";
    pipensx::UpdateService service(Target,
        [checksum](const std::string&, size_t, std::string& body,
                   std::string&) { body = checksum + "  freeshop-client.nro\n"; return true; },
        [payload](const std::string&, const std::string& path, size_t,
                  std::string&) { write(path, payload); return true; },
        HelperSource);
    pipensx::ReleaseInfo release{"v1.2.3",
        "https://freeshop-proxy.freeshopnx.workers.dev/releases/assets/1001",
        "https://freeshop-proxy.freeshopnx.workers.dev/releases/assets/1002"};
    std::string error;
    emulateSwitchRename = true;
    const bool installed = service.install(release, error);
    emulateSwitchRename = false;
    assert(installed);
    std::ifstream result(Target, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(result)), {});
    assert(bytes == "old");
    std::ifstream staged("/tmp/pipensx-update-test.nro.update",
                         std::ios::binary);
    bytes.assign((std::istreambuf_iterator<char>(staged)), {});
    assert(bytes == payload);
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
    unlink("/tmp/pipensx-update-test.nro.update.sha256");
    unlink("/tmp/freeshop-client-updater.nro");
    unlink("/tmp/pipensx-update-test.nro.previous");
}

void testShutdownInterruptsRetryWait() {
    std::atomic<int> attempts{0};
    pipensx::UpdateService service(Target,
        [&attempts](const std::string&, size_t, std::string&,
                    std::string& error) {
            ++attempts;
            error = "Error de red de actualización: Timeout was reached";
            return false;
        });
    service.checkAsync([](pipensx::UpdateCheckResult) {});
    while (attempts.load() == 0)
        std::this_thread::yield();
    const auto started = std::chrono::steady_clock::now();
    service.shutdown();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    assert(elapsed.count() < 250);
    assert(attempts.load() == 1);
}

void testHelperPublishFailurePreservesPreviousHelper() {
    unlink(Target);
    unlink("/tmp/pipensx-update-test.nro.update");
    unlink("/tmp/pipensx-update-test.nro.update.sha256");
    unlink("/tmp/freeshop-client-updater.nro.tmp");
    unlink(HelperSource);
    write(Target, "old");
    // HelperSource deliberately left missing: publishHelper()'s very first
    // step (copy from helperSourcePath_) fails with no fallback, unlike a
    // rename() EIO, which is recoverable (see the copy-fallback in
    // publishHelper() for the sdmc: quirk where rename() can spuriously
    // refuse to replace an existing destination).
    write("/tmp/freeshop-client-updater.nro", "previous helper");
    const std::string payload = "new verified pipensx nro";
    const std::string checksum =
        "9dc1034a694baa2d68b032ed446fbc9b170975306c571202e99ff741821365b4";
    pipensx::UpdateService service(Target,
        [checksum](const std::string&, size_t, std::string& body,
                   std::string&) { body = checksum + "  freeshop-client.nro\n"; return true; },
        [payload](const std::string&, const std::string& path, size_t,
                  std::string&) { write(path, payload); return true; },
        HelperSource);
    pipensx::ReleaseInfo release{"v1.2.3",
        "https://freeshop-proxy.freeshopnx.workers.dev/releases/assets/1001",
        "https://freeshop-proxy.freeshopnx.workers.dev/releases/assets/1002"};
    std::string error;
    const bool installed = service.install(release, error);
    assert(!installed);
    assert(error.find("No se pudo publicar el ayudante de actualización") !=
          std::string::npos);
    std::ifstream helper(service.helperPath(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(helper)), {});
    assert(bytes == "previous helper");
    assert(access("/tmp/freeshop-client-updater.nro.tmp", F_OK) != 0);
    assert(access("/tmp/pipensx-update-test.nro.update", F_OK) != 0);
    assert(access("/tmp/pipensx-update-test.nro.update.sha256", F_OK) != 0);
    unlink(Target);
    unlink(service.helperPath().c_str());
    unlink(HelperSource);
}

} // namespace

int main() {
    testVersionsAndReleaseValidation();
    testStagedReadyRecoversInterruptedRestart();
    testInstallVerifiesBeforeStaging();
    testTransientNetworkFailuresAreRetried();
    testInstallNeverTouchesRunningNro();
    testShutdownInterruptsRetryWait();
    testHelperPublishFailurePreservesPreviousHelper();
    std::puts("update service tests passed");
    return 0;
}
