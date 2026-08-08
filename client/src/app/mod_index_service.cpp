#include "mod_index_service.hpp"
#include "curl_https.hpp"

extern "C" {
#include "../core/util.h"
}

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <curl/curl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace pipensx {
namespace {

constexpr size_t kMaxTableBytes = 2 * 1024 * 1024;
constexpr size_t kMaxTableRows = 20000;
constexpr size_t kMaxGameNameBytes = 512;

/* The live mod index: ModCD publishes its whole mod catalogue as one table.md
   on GitHub's raw host. Fetched alongside the catalogue refresh (and once a day
   when the cache is missing or stale); never on a per-card path. Must satisfy
   isTrustedSource(). */
constexpr const char* kModIndexSourceUrl =
    "https://raw.githubusercontent.com/kawaii-flesh/ModCD/"
    "refs/heads/main/table.md";

// Marker that introduces every mod inside ModCD's third column.
constexpr const char* kModMarker = "**Name**:";

struct HttpBuffer {
    std::string data;
    bool overflow = false;
};

size_t writeHttp(void* bytes, size_t size, size_t count, void* user) {
    HttpBuffer* buffer = static_cast<HttpBuffer*>(user);
    size_t total = size * count;
    if (buffer->data.size() + total > kMaxTableBytes) {
        buffer->overflow = true;
        return 0;
    }
    buffer->data.append(static_cast<const char*>(bytes), total);
    return total;
}

bool makeDirectories(const std::string& path) {
    char buffer[1024];
    if (path.empty() || path.size() >= sizeof(buffer))
        return false;
    std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());
    for (char* cursor = buffer + 1; *cursor; ++cursor) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
            return false;
        *cursor = '/';
    }
    return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

bool readFile(const std::string& path, std::string& data, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "No se pudo abrir el archivo del índice de mods.";
        return false;
    }
    input.seekg(0, std::ios::end);
    std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size <= 0 || size > static_cast<std::streamoff>(kMaxTableBytes)) {
        error = "El archivo del índice de mods está vacío o es demasiado grande.";
        return false;
    }
    data.resize(static_cast<size_t>(size));
    input.read(data.data(), size);
    if (!input) {
        error = "No se pudo leer el archivo del índice de mods.";
        return false;
    }
    return true;
}

bool writeAtomic(const std::string& path, const std::string& data,
                 std::string& error) {
    std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "No se pudo crear la caché del índice de mods.";
            return false;
        }
        output.write(data.data(), static_cast<std::streamsize>(data.size()));
        output.flush();
        if (!output.good()) {
            unlink(temporary.c_str());
            error = "No se pudo escribir la caché del índice de mods.";
            return false;
        }
    }
    if (rename(temporary.c_str(), path.c_str()) == 0)
        return true;
    // Switch fsdev/FatFs rename() fails when the target already exists (no
    // POSIX overwrite), so drop the old file first and retry.
    if ((unlink(path.c_str()) == 0 || errno == ENOENT) &&
        rename(temporary.c_str(), path.c_str()) == 0)
        return true;
    unlink(temporary.c_str());
    error = "No se pudo reemplazar la caché del índice de mods.";
    return false;
}

bool httpGet(const std::string& url, std::string& body, std::string& error) {
    body.clear();
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "No se pudo inicializar HTTP.";
        return false;
    }
    HttpBuffer buffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "pipensx/0.4");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    /* Every handle here runs on a background thread; without this
       libcurl installs signal handlers to time out DNS, which is not
       thread-safe (libcurl-thread(3)). */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeHttp);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curlPinHttpsOnly(curl);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    char* effective = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    const std::string effectiveUrl =
        effective ? std::string(effective) : std::string();
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status < 200 || status >= 300 ||
        buffer.overflow) {
        if (buffer.overflow)
            error = "La descarga del índice de mods superó el límite de tamaño.";
        else if (result != CURLE_OK)
            error = std::string("Error de red del índice de mods: ") +
                    curl_easy_strerror(result);
        else
            error = "El servidor del índice de mods devolvió HTTP " +
                    std::to_string(status) + ".";
        return false;
    }
    if (!ModIndexService::isTrustedSource(effectiveUrl)) {
        error = "La descarga del índice de mods redirigió a un servidor no confiable.";
        return false;
    }
    body = std::move(buffer.data);
    return true;
}

std::string trim(const std::string& value) {
    size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return std::string();
    size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

size_t countOccurrences(const std::string& haystack, const char* needle) {
    size_t count = 0;
    const size_t step = std::char_traits<char>::length(needle);
    for (size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + step))
        ++count;
    return count;
}

} // namespace

ModIndexService::ModIndexService(std::string rootPath, std::string bundledPath)
    : rootPath_(std::move(rootPath)),
      catalogRoot_(rootPath_ + "/catalog"),
      cachePath_(catalogRoot_ + "/modcd_table.md"),
      bundledPath_(std::move(bundledPath)) {
    makeDirectories(catalogRoot_);
}

std::string ModIndexService::normalizeTitleId(const std::string& titleId) {
    if (titleId.size() != 16)
        return std::string();
    uint64_t value = 0;
    for (char c : titleId) {
        int digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            return std::string();
        value = (value << 4) | static_cast<uint64_t>(digit);
    }
    // Base application id: updates set bit 11 (…800) and DLC increments the low
    // 12 bits, so masking them off maps every variant onto the base title.
    value &= ~static_cast<uint64_t>(0x1FFF);
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llX",
                  static_cast<unsigned long long>(value));
    return std::string(buffer);
}

bool ModIndexService::isTrustedSource(const std::string& url) {
    // Host (with path prefix) allowed to serve mod-index bytes. Only the ModCD
    // repo on GitHub's raw host; the fetch is gated on this so a redirect to
    // another host is refused before any parse.
    static const char* const kPrefixes[] = {
        "https://raw.githubusercontent.com/kawaii-flesh/ModCD/",
    };
    for (const char* prefix : kPrefixes)
        if (url.rfind(prefix, 0) == 0)
            return true;
    return false;
}

bool ModIndexService::parseTable(const std::string& markdown,
                                 std::vector<ModIndexEntry>& entries,
                                 std::string& error) {
    entries.clear();
    if (markdown.empty() || markdown.size() > kMaxTableBytes) {
        error = "La tabla del índice de mods está vacía o es demasiado grande.";
        return false;
    }
    size_t line = 0;
    while (line < markdown.size() && entries.size() < kMaxTableRows) {
        size_t end = markdown.find('\n', line);
        if (end == std::string::npos)
            end = markdown.size();
        const std::string row = markdown.substr(line, end - line);
        line = end + 1;
        if (row.empty() || row[0] != '|')
            continue;
        // "| <title id> | <game name> | <mods…>"; the header and the |---| rule
        // fall out here because their first cell is not a title id.
        const size_t firstCell = row.find('|', 1);
        if (firstCell == std::string::npos)
            continue;
        ModIndexEntry entry;
        entry.titleId = normalizeTitleId(trim(row.substr(1, firstCell - 1)));
        if (entry.titleId.empty())
            continue;
        size_t secondCell = row.find('|', firstCell + 1);
        if (secondCell == std::string::npos)
            secondCell = row.size();
        entry.gameName =
            trim(row.substr(firstCell + 1, secondCell - firstCell - 1));
        if (entry.gameName.size() > kMaxGameNameBytes)
            entry.gameName.resize(kMaxGameNameBytes);
        const std::string mods = secondCell < row.size()
            ? row.substr(secondCell + 1)
            : std::string();
        // 0 = the row matched but ModCD's formatting drifted away from the
        // **Name**: marker; the UI then says "available" instead of a count it
        // cannot trust.
        entry.modCount =
            static_cast<uint32_t>(countOccurrences(mods, kModMarker));
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        error = "La tabla del índice de mods no contiene ningún title id.";
        return false;
    }
    return true;
}

bool ModIndexService::loadFile(const std::string& path,
                               const std::string& label) {
    std::string data;
    std::string error;
    if (!readFile(path, data, error))
        return false;
    ModIndexSnapshot snapshot;
    if (!parseTable(data, snapshot.items, error)) {
        log_msg("[mods] %s parse failed: %s\n", label.c_str(), error.c_str());
        return false;
    }
    snapshot.rawTable = std::move(data);
    adopt(std::move(snapshot));
    log_msg("[mods] loaded %zu titles from %s\n", byBaseId_.size(),
            path.c_str());
    return true;
}

bool ModIndexService::load(std::string& error) {
    error.clear();
    if (loadFile(cachePath_, "cached mod index"))
        return true;
    if (!bundledPath_.empty() && loadFile(bundledPath_, "bundled mod index"))
        return true;
    // No cache yet: the catalogue view fetches the table in the background.
    // An empty index simply means no chips, never a failed startup.
    byBaseId_.clear();
    error = "El índice de mods aún no está en caché.";
    return false;
}

bool ModIndexService::fetchLatest(ModIndexSnapshot& snapshot,
                                  std::string& error) const {
    // Worker thread: network fetch + parse + cache write only, so it never
    // touches byBaseId_. The cached index in memory survives a failure.
    snapshot.items.clear();
    snapshot.rawTable.clear();
    if (!isTrustedSource(kModIndexSourceUrl)) {
        error = "La URL del índice de mods no está en la lista de hosts confiables.";
        return false;
    }
    std::string body;
    if (!httpGet(kModIndexSourceUrl, body, error))
        return false;
    if (!parseTable(body, snapshot.items, error))
        return false;
    if (!writeAtomic(cachePath_, body, error))
        return false;
    snapshot.rawTable = std::move(body);
    return true;
}

void ModIndexService::adopt(ModIndexSnapshot snapshot) {
    // UI thread only: byBaseId_ is read unsynchronised by the render thread, so
    // this swap must never happen on the fetch worker (data race → UAF).
    std::unordered_map<std::string, uint32_t> byBaseId;
    byBaseId.reserve(snapshot.items.size());
    for (const ModIndexEntry& entry : snapshot.items) {
        uint32_t& count = byBaseId[entry.titleId];
        // Re-releases of one title collapse onto the base id; keep the richest.
        count = std::max(count, entry.modCount);
    }
    byBaseId_ = std::move(byBaseId);
}

bool ModIndexService::has(const std::string& titleId) const {
    if (byBaseId_.empty())
        return false;
    const std::string base = normalizeTitleId(titleId);
    return !base.empty() && byBaseId_.count(base) != 0;
}

uint32_t ModIndexService::modCount(const std::string& titleId) const {
    if (byBaseId_.empty())
        return 0;
    const std::string base = normalizeTitleId(titleId);
    if (base.empty())
        return 0;
    auto it = byBaseId_.find(base);
    return it == byBaseId_.end() ? 0 : it->second;
}

} // namespace pipensx
