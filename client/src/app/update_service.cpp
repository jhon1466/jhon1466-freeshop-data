#include "update_service.hpp"
#include "curl_https.hpp"
#include "update_transaction.h"

#include <curl/curl.h>
#include <borealis/extern/nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <vector>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

extern "C" {
#include "core/sha256.h"
#include "core/util.h"
}

namespace pipensx {
namespace {

#ifndef PIPENSX_VERSION
#define PIPENSX_VERSION "0.0.0"
#endif

// Proxied through our own Worker (not straight at api.github.com/github.com)
// so update checks keep working once the data repo is set to private, and
// so the repo owner/name doesn't appear in the compiled .nro's strings. See
// worker/src/index.ts's handleReleaseLatest/handleReleaseAsset.
constexpr const char* kLatestReleaseUrl =
    "https://freeshop-proxy.freeshopnx.workers.dev/releases/latest";
constexpr const char* kReleaseAssetPrefix =
    "https://freeshop-proxy.freeshopnx.workers.dev/releases/assets/";
constexpr size_t kMetadataLimit = 512 * 1024;
constexpr size_t kChecksumLimit = 1024;
constexpr size_t kNroLimit = 64 * 1024 * 1024;
constexpr int kFetchAttempts = 3;

enum class TransferKind {
    Metadata,
    Download,
};

struct HttpBuffer {
    std::string data;
    size_t limit = 0;
    bool overflow = false;
};

size_t writeString(char* bytes, size_t size, size_t count, void* opaque) {
    auto* buffer = static_cast<HttpBuffer*>(opaque);
    const size_t received = size * count;
    if (received > buffer->limit - std::min(buffer->limit, buffer->data.size())) {
        buffer->overflow = true;
        return 0;
    }
    buffer->data.append(bytes, received);
    return received;
}

struct FileWriter {
    std::ofstream output;
    size_t written = 0;
    size_t limit = 0;
    bool overflow = false;
};

size_t writeFile(char* bytes, size_t size, size_t count, void* opaque) {
    auto* writer = static_cast<FileWriter*>(opaque);
    const size_t received = size * count;
    if (received > writer->limit - std::min(writer->limit, writer->written)) {
        writer->overflow = true;
        return 0;
    }
    writer->output.write(bytes, static_cast<std::streamsize>(received));
    if (!writer->output.good())
        return 0;
    writer->written += received;
    return received;
}

int enlargeSocketBuffer(void*, curl_socket_t socket, curlsocktype purpose) {
    // Borealis boots the Switch socket service with tiny default buffers;
    // a larger receive window is what keeps the NRO download off the
    // kilobytes-per-second floor.
    if (purpose == CURLSOCKTYPE_IPCXN) {
        int size = 256 * 1024;
        setsockopt(socket, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&size), sizeof(size));
    }
    return CURL_SOCKOPT_OK;
}

struct TransferProgress {
    const UpdateService::ProgressCallback* callback;
    const std::atomic<bool>* stopping;
};

int reportTransferProgress(void* opaque, curl_off_t downloadTotal,
                           curl_off_t downloadNow, curl_off_t, curl_off_t) {
    const auto* progress = static_cast<const TransferProgress*>(opaque);
    if (progress && progress->stopping &&
        progress->stopping->load(std::memory_order_relaxed))
        return 1;
    if (progress && progress->callback && *progress->callback &&
        downloadTotal > 0)
        (*progress->callback)(static_cast<uint64_t>(downloadNow),
                              static_cast<uint64_t>(downloadTotal));
    return 0;
}

bool configureCurl(CURL* curl, const std::string& url, TransferKind kind,
                   std::string& error) {
    if (!curl) {
        error = "No se pudo inicializar el cliente HTTP del actualizador.";
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "freeshop-client/" PIPENSX_VERSION);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (kind == TransferKind::Download) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L * 60L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 256L * 1024L);
        curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, enlargeSocketBuffer);
    } else {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    }
    // Peer verify stays on and imports the bundled Switch roots; without
    // CAINFO the console's own store is the only trust anchor and release
    // checks fail with an SSL certificate error.
    curlApplyTrustedSsl(curl);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    return true;
}

bool fetchText(const std::string& url, size_t limit, std::string& body,
               std::string& error,
               const std::atomic<bool>* stopping = nullptr) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!configureCurl(curl, url, TransferKind::Metadata, error))
        return false;
    HttpBuffer buffer;
    buffer.limit = limit;
    curl_slist* headers = curl_slist_append(nullptr,
        "Accept: application/vnd.github+json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    TransferProgress progress{nullptr, stopping};
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, reportTransferProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
    char sslError[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, sslError);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
        log_msg("[update] fetchText fail url=%s rc=%d (%s) errbuf='%s'\n",
                url.c_str(), result, curl_easy_strerror(result),
                sslError[0] ? sslError : "-");
        if (sslError[0])
            log_msg("[update] ssl detail: %s\n", sslError);
    }
    if (buffer.overflow)
        error = "La respuesta de actualización superó su límite de tamaño.";
    else if (result != CURLE_OK)
        error = std::string("Error de red de actualización: ") + curl_easy_strerror(result);
    else if (status < 200 || status >= 300)
        error = "El servidor de actualización devolvió HTTP " + std::to_string(status) + ".";
    else {
        body = std::move(buffer.data);
        return true;
    }
    return false;
}

bool fetchFile(const std::string& url, const std::string& path, size_t limit,
               std::string& error,
               const UpdateService::ProgressCallback* callback = nullptr,
               const std::atomic<bool>* stopping = nullptr) {
    FileWriter writer;
    writer.output.open(path, std::ios::binary | std::ios::trunc);
    writer.limit = limit;
    if (!writer.output) {
        error = "No se pudo crear la descarga de actualización.";
        return false;
    }
    CURL* curl = curl_easy_init();
    if (!configureCurl(curl, url, TransferKind::Download, error)) {
        unlink(path.c_str());
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writer);
    TransferProgress progress{callback, stopping};
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, reportTransferProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
    char sslError[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, sslError);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    writer.output.close();
    if (result != CURLE_OK) {
        log_msg("[update] fetchFile fail url=%s rc=%d (%s) errbuf='%s'\n",
                url.c_str(), result, curl_easy_strerror(result),
                sslError[0] ? sslError : "-");
        if (sslError[0])
            log_msg("[update] ssl detail: %s\n", sslError);
    }
    if (writer.overflow)
        error = "La descarga de actualización superó su límite de tamaño.";
    else if (result != CURLE_OK)
        error = std::string("Falló la descarga de actualización: ") + curl_easy_strerror(result);
    else if (status < 200 || status >= 300)
        error = "La descarga de actualización devolvió HTTP " + std::to_string(status) + ".";
    else
        return true;
    unlink(path.c_str());
    return false;
}

bool startsWith(const std::string& value, const char* prefix) {
    return value.compare(0, std::strlen(prefix), prefix) == 0;
}

bool retryableHttpError(const std::string& error) {
    const size_t marker = error.find("HTTP ");
    if (marker == std::string::npos)
        return false;
    try {
        const int status = std::stoi(error.substr(marker + 5));
        return status == 408 || status == 429 || status >= 500;
    } catch (...) {
        return false;
    }
}

bool retryableFetchError(const std::string& error) {
    return startsWith(error, "Error de red de actualización:") ||
           startsWith(error, "Falló la descarga de actualización:") ||
           retryableHttpError(error);
}

template <typename Fetch>
bool fetchWithRetry(Fetch fetch, std::string& error,
                    const std::atomic<bool>& stopping,
                    std::mutex& stopMutex,
                    std::condition_variable& stopReady) {
    for (int attempt = 1; attempt <= kFetchAttempts; ++attempt) {
        if (stopping.load(std::memory_order_relaxed)) {
            error = "Actualización cancelada.";
            return false;
        }
        error.clear();
        if (fetch())
            return true;
        const bool retryable = retryableFetchError(error);
        if (!retryable || attempt == kFetchAttempts) {
            if (retryable)
                error += " (tras " + std::to_string(attempt) + " intentos).";
            return false;
        }
        std::unique_lock<std::mutex> lock(stopMutex);
        if (stopReady.wait_for(lock, std::chrono::milliseconds(500 * attempt),
                               [&stopping] {
                                   return stopping.load(std::memory_order_relaxed);
                               })) {
            error = "Actualización cancelada.";
            return false;
        }
    }
    return false;
}

bool trustedAssetUrl(const std::string& url) {
    return url.compare(0, std::strlen(kReleaseAssetPrefix),
                       kReleaseAssetPrefix) == 0;
}

bool parseVersion(const std::string& text, std::array<uint64_t, 3>& version) {
    std::string value = text;
    if (!value.empty() && value.front() == 'v')
        value.erase(value.begin());
    size_t start = 0;
    for (size_t index = 0; index < version.size(); ++index) {
        size_t end = value.find('.', start);
        if ((index + 1 == version.size()) != (end == std::string::npos))
            return false;
        const std::string part = value.substr(start, end - start);
        if (part.empty() || !std::all_of(part.begin(), part.end(),
            [](unsigned char c) { return std::isdigit(c); }))
            return false;
        try {
            version[index] = std::stoull(part);
        } catch (...) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

bool parseChecksum(const std::string& text, std::string& checksum) {
    std::istringstream input(text);
    std::string token;
    input >> token;
    if (token.size() != 64 || !std::all_of(token.begin(), token.end(),
        [](unsigned char c) { return std::isxdigit(c); }))
        return false;
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    checksum = std::move(token);
    return true;
}

bool checksumFile(const std::string& path, std::string& checksum,
                  std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "No se pudo leer la actualización descargada.";
        return false;
    }
    const std::streamoff size = input.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(kNroLimit)) {
        error = "La actualización descargada está vacía o es demasiado grande.";
        return false;
    }
    input.seekg(0);
    std::vector<unsigned char> data(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(data.data()), size);
    if (!input) {
        error = "No se pudo leer la actualización descargada.";
        return false;
    }
    unsigned char digest[32];
    sha256(data.data(), data.size(), digest);
    static const char digits[] = "0123456789abcdef";
    checksum.clear();
    checksum.reserve(64);
    for (unsigned char byte : digest) {
        checksum.push_back(digits[byte >> 4]);
        checksum.push_back(digits[byte & 15]);
    }
    return true;
}

bool copyFileContents(const std::string& source, const std::string& destination,
                      std::string& error) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        error = std::strerror(errno);
        return false;
    }
    std::ofstream output(destination,
                         std::ios::binary | std::ios::trunc);
    if (!output) {
        error = std::strerror(errno);
        return false;
    }
    // Heap, not a stack std::array - see file_explorer_service.cpp's
    // copyFileContents for why: this runs on a background worker thread,
    // whose default stack is far smaller than the main thread's.
    std::vector<char> buffer(64 * 1024);
    while (input) {
        input.read(buffer.data(), buffer.size());
        const std::streamsize count = input.gcount();
        if (count > 0)
            output.write(buffer.data(), count);
        if (!output) {
            error = std::strerror(errno);
            return false;
        }
    }
    output.flush();
    if (input.bad() || !output) {
        error = std::strerror(errno);
        return false;
    }
    return true;
}

} // namespace

UpdateService::UpdateService(std::string targetPath,
                             MetadataFetcher metadataFetcher,
                             AssetFetcher assetFetcher,
                             std::string helperSourcePath)
    : targetPath_(std::move(targetPath)),
      helperPath_([this] {
          const size_t slash = targetPath_.find_last_of('/');
          return targetPath_.substr(0, slash == std::string::npos ? 0 : slash + 1) +
                 "freeshop-client-updater.nro";
      }()),
      helperSourcePath_(std::move(helperSourcePath)),
      metadataFetcher_(std::move(metadataFetcher)),
      assetFetcher_(std::move(assetFetcher)) {
    if (!metadataFetcher_)
        metadataFetcher_ = [this](const std::string& url, size_t limit,
                                  std::string& body, std::string& error) {
            return fetchText(url, limit, body, error, &stopping_);
        };
    if (!assetFetcher_)
        assetFetcher_ = [this](const std::string& url, const std::string& path,
                               size_t limit, std::string& error) {
            return fetchFile(url, path, limit, error, &progress_, &stopping_);
        };
}

UpdateService::~UpdateService() {
    shutdown();
}

void UpdateService::cancel() {
    stopping_.store(true, std::memory_order_relaxed);
    stopReady_.notify_all();
}

void UpdateService::shutdown() {
    cancel();
    for (std::thread& worker : workers_)
        if (worker.joinable())
            worker.join();
    workers_.clear();
}

bool UpdateService::isNewerVersion(const std::string& candidate,
                                   const std::string& current) {
    std::array<uint64_t, 3> candidateParts{};
    std::array<uint64_t, 3> currentParts{};
    return parseVersion(candidate, candidateParts) &&
           parseVersion(current, currentParts) && candidateParts > currentParts;
}

bool UpdateService::parseRelease(const std::string& json, ReleaseInfo& release,
                                 std::string& error) {
    release = {};
    nlohmann::json root = nlohmann::json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "GitHub devolvió un lanzamiento no válido.";
        return false;
    }
    if (root.value("draft", true) || root.value("prerelease", true)) {
        error = "El último lanzamiento en GitHub no está publicado ni es estable.";
        return false;
    }
    release.version = root.value("tag_name", "");
    release.notes = root.value("body", "");
    if (!isNewerVersion(release.version, "0.0.0")) {
        error = "El lanzamiento de GitHub tiene una etiqueta de versión no válida.";
        return false;
    }
    if (!root.contains("assets") || !root["assets"].is_array()) {
        error = "El lanzamiento de GitHub no tiene archivos adjuntos.";
        return false;
    }
    for (const auto& asset : root["assets"]) {
        if (!asset.is_object())
            continue;
        const std::string name = asset.value("name", "");
        const std::string url = asset.value("browser_download_url", "");
        if (!trustedAssetUrl(url))
            continue;
        if (name == "freeshop-client.nro")
            release.nroUrl = url;
        else if (name == "freeshop-client.nro.sha256")
            release.checksumUrl = url;
    }
    if (release.nroUrl.empty() || release.checksumUrl.empty()) {
        error = "El lanzamiento de GitHub debe incluir freeshop-client.nro y freeshop-client.nro.sha256.";
        return false;
    }
    return true;
}

UpdateCheckResult UpdateService::check() const {
    log_msg("[update] checking for updates (current=%s)\n", PIPENSX_VERSION);
    UpdateCheckResult result;
    std::string body;
    if (!fetchWithRetry([&] {
            return metadataFetcher_(kLatestReleaseUrl, kMetadataLimit, body,
                                    result.error);
        }, result.error, stopping_, stopMutex_, stopReady_)) {
        log_msg("[update] check failed: %s\n", result.error.c_str());
        return result;
    }
    if (!parseRelease(body, result.release, result.error)) {
        log_msg("[update] release parse failed: %s\n", result.error.c_str());
        return result;
    }
    result.ok = true;
    result.updateAvailable = isNewerVersion(result.release.version,
                                            PIPENSX_VERSION);
    log_msg("[update] latest=%s current=%s update_available=%d\n",
            result.release.version.c_str(), PIPENSX_VERSION,
            result.updateAvailable ? 1 : 0);
    return result;
}

bool UpdateService::publishHelper(std::string& error) const {
    const std::string helper = helperPath();
    const std::string helperTemporary = helper + ".tmp";
    unlink(helperTemporary.c_str());
    if (!copyFileContents(helperSourcePath_, helperTemporary, error)) {
        unlink(helperTemporary.c_str());
        return false;
    }
    std::string sourceHelperChecksum;
    std::string copiedHelperChecksum;
    if (!checksumFile(helperSourcePath_, sourceHelperChecksum, error) ||
        !checksumFile(helperTemporary, copiedHelperChecksum, error) ||
        sourceHelperChecksum != copiedHelperChecksum) {
        if (error.empty())
            error = "la suma de verificación del ayudante copiado no coincide";
        unlink(helperTemporary.c_str());
        return false;
    }
    // sdmc:'s rename() can refuse to replace an existing destination even
    // right after a fresh unlink() of it - same quirk install_journal.cpp's
    // saveInstallJournal() and main_switch.cpp's legacy-update-hop code
    // both already work around.
    unlink(helper.c_str());
    if (rename(helperTemporary.c_str(), helper.c_str()) != 0) {
        if (!copyFileContents(helperTemporary, helper, error)) {
            unlink(helperTemporary.c_str());
            return false;
        }
        unlink(helperTemporary.c_str());
    }
    return true;
}

bool UpdateService::refreshHelper(std::string& error) const {
    if (!publishHelper(error)) {
        log_msg("[update] helper refresh failed: %s\n", error.c_str());
        return false;
    }
    log_msg("[update] helper refreshed at %s\n", helperPath().c_str());
    return true;
}

bool UpdateService::install(const ReleaseInfo& release, std::string& error) const {
    log_msg("[update] installing %s\n", release.version.c_str());
    if (!trustedAssetUrl(release.nroUrl) ||
        !trustedAssetUrl(release.checksumUrl)) {
        error = "La URL del archivo de actualización no es confiable.";
        log_msg("[update] install failed: %s\n", error.c_str());
        return false;
    }
    std::string checksumText;
    if (!fetchWithRetry([&] {
            return metadataFetcher_(release.checksumUrl, kChecksumLimit,
                                    checksumText, error);
        }, error, stopping_, stopMutex_, stopReady_)) {
        log_msg("[update] checksum fetch failed: %s\n", error.c_str());
        return false;
    }
    std::string expectedChecksum;
    if (!parseChecksum(checksumText, expectedChecksum)) {
        error = "La suma de verificación de la actualización no es válida.";
        log_msg("[update] install failed: %s\n", error.c_str());
        return false;
    }
    log_msg("[update] expected checksum %s\n", expectedChecksum.c_str());
    const std::string temporary = stagedPath();
    const std::string marker = temporary + ".sha256";
    unlink(temporary.c_str());
    unlink(marker.c_str());
    log_msg("[update] downloading %s\n", release.nroUrl.c_str());
    if (!fetchWithRetry([&] {
            return assetFetcher_(release.nroUrl, temporary, kNroLimit, error);
        }, error, stopping_, stopMutex_, stopReady_)) {
        log_msg("[update] download failed: %s\n", error.c_str());
        return false;
    }
    std::string actualChecksum;
    if (!checksumFile(temporary, actualChecksum, error)) {
        log_msg("[update] checksum read failed: %s\n", error.c_str());
        unlink(temporary.c_str());
        return false;
    }
    if (actualChecksum != expectedChecksum) {
        log_msg("[update] checksum mismatch: got %s want %s\n",
                actualChecksum.c_str(), expectedChecksum.c_str());
        unlink(temporary.c_str());
        error = "La suma de verificación de la actualización no coincide con el lanzamiento de GitHub.";
        return false;
    }
    log_msg("[update] download verified\n");
    std::ofstream markerFile(marker, std::ios::binary | std::ios::trunc);
    markerFile << expectedChecksum << '\n';
    markerFile.flush();
    if (!markerFile) {
        unlink(marker.c_str());
        unlink(temporary.c_str());
        error = "No se pudo guardar la suma de verificación de la actualización preparada.";
        return false;
    }
    markerFile.close();
    std::string helperError;
    if (!publishHelper(helperError)) {
        unlink(marker.c_str());
        unlink(temporary.c_str());
        error = "No se pudo publicar el ayudante de actualización: " + helperError;
        log_msg("[update] install failed: %s\n", error.c_str());
        return false;
    }
    // Best-effort: the "what's new" screen just has nothing to show if this
    // fails, so a write error here must not fail the update itself.
    unlink(notesPath().c_str());
    if (!release.notes.empty()) {
        std::ofstream notesFile(notesPath(), std::ios::binary | std::ios::trunc);
        notesFile << release.notes;
        notesFile.flush();
        if (!notesFile)
            unlink(notesPath().c_str());
    }
#ifdef __SWITCH__
    const Result commit = fsdevCommitDevice("sdmc");
    if (R_FAILED(commit)) {
        error = "No se pudieron confirmar los archivos de actualización preparados.";
        log_msg("[update] install failed: %s\n", error.c_str());
        discardStaged();
        return false;
    }
#endif
    log_msg("[update] staged %s, ready to relaunch via updater helper\n",
            release.version.c_str());
    return true;
}

bool UpdateService::stagedReady() const {
    const std::string temporary = stagedPath();
    std::ifstream markerFile(temporary + ".sha256", std::ios::binary);
    std::ostringstream markerText;
    markerText << markerFile.rdbuf();
    std::string expectedChecksum;
    if (!markerFile || !parseChecksum(markerText.str(), expectedChecksum))
        return false;
    std::string actualChecksum;
    std::string error;
    return checksumFile(temporary, actualChecksum, error) &&
           actualChecksum == expectedChecksum;
}

bool UpdateService::hasPendingConfirmation() const {
    return access(backupPath().c_str(), F_OK) == 0 &&
           access(stagedPath().c_str(), F_OK) != 0;
}

bool UpdateService::confirmInstalled(std::string& error) const {
    log_msg("[update] confirming install after relaunch (target=%s)\n",
            targetPath_.c_str());
    const std::string staged = stagedPath();
    const std::string marker = staged + ".sha256";
    const std::string backup = backupPath();
    update_paths_t paths{targetPath_.c_str(), staged.c_str(),
                         marker.c_str(), backup.c_str()};
    char transactionError[256] = {0};
    if (!update_transaction_confirm(&paths, transactionError,
                                    sizeof(transactionError))) {
        error = transactionError;
        log_msg("[update] confirm failed: %s\n", error.c_str());
        return false;
    }
    unlink(helperPath_.c_str());
#ifdef __SWITCH__
    const Result commit = fsdevCommitDevice("sdmc");
    if (R_FAILED(commit)) {
        error = "No se pudo confirmar la limpieza de la actualización.";
        log_msg("[update] confirm failed: %s\n", error.c_str());
        return false;
    }
#endif
    log_msg("[update] confirmed, now running the new version\n");
    return true;
}

void UpdateService::discardStaged() const {
    const std::string temporary = stagedPath();
    unlink(temporary.c_str());
    unlink((temporary + ".sha256").c_str());
    unlink((helperPath() + ".tmp").c_str());
    unlink(helperPath().c_str());
    unlink(notesPath().c_str());
}

void UpdateService::checkAsync(CheckCallback callback) {
    workers_.emplace_back([this, callback = std::move(callback)]() mutable {
        UpdateCheckResult result = check();
        checkCompleted_.store(true);
        callback(std::move(result));
    });
}

void UpdateService::installAsync(ReleaseInfo release, InstallCallback callback) {
    workers_.emplace_back([this, release = std::move(release),
                           callback = std::move(callback)]() mutable {
        std::string error;
        const bool installed = install(release, error);
        callback(installed, std::move(error));
    });
}

} // namespace pipensx
