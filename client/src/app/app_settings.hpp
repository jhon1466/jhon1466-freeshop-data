#pragma once

#include "debrid_provider.hpp"

#include <cstdint>
#include <string>

namespace pipensx {

enum class CatalogFilter {
    All,
    Games,
};

enum class StreamSelection {
    AllFiles,
    PackagesOnly,
};

// Where stream installs commit content (PERF_PLAN 7.4). SystemMemory targets
// eMMC/NAND, whose write path is typically faster than SD.
enum class InstallLocation {
    SdCard,
    SystemMemory,
};

struct AppSettingsData {
    // UI language: "auto" follows the console's system language, otherwise a
    // borealis locale directory name. Read before Application::init() to set
    // Platform::APP_LOCALE_DEFAULT; borealis loads translations once, so a
    // change only takes effect on the next launch.
    std::string language = "auto";
    // Color scheme: "auto" follows the console's system theme (read once at
    // startup by borealis's SwitchPlatform, same as language), "light" and
    // "dark" force it regardless. Applied right after Application::init()
    // via Platform::setThemeVariant() - unlike language this takes effect
    // immediately, no restart needed.
    std::string themeMode = "auto";
    CatalogFilter catalogFilter = CatalogFilter::Games;
    // HTTPS URL to a switch_games.json-compatible catalog. Empty = built-in
    // FreeShop source. Validated at parse time; a hand-edited settings.json
    // cannot smuggle a non-HTTPS or credential-bearing URL.
    std::string catalogSourceUrl;
    bool refreshCatalogOnLaunch = false;
    uint64_t lastCatalogRefreshMs = 0;
    // Wall-clock seconds of the last successful catalogue download. 0 = never
    // refreshed on this console (a bundled dump does not count). Used by the
    // freshness badge — distinct from lastCatalogRefreshMs, which is
    // monotonic now_ms() for the refresh-on-launch gate.
    uint64_t lastCatalogRefreshWallSec = 0;
    uint64_t lastMetadataRefreshMs = 0;
    uint64_t lastModsRefreshMs = 0;
    StreamSelection streamSelection = StreamSelection::AllFiles;
    InstallLocation installLocation = InstallLocation::SdCard;
    bool showCompletedDownloads = true;
    bool extendedTelemetry = false;
    bool checkForUpdatesOnLaunch = true;
    // Console-native UI sound effects (borealis's SwitchAudioPlayer, reading
    // qlaunch's own bfsar at runtime - nothing bundled). Matches the
    // console's own default of on.
    bool soundEffectsEnabled = true;
    // First-run disclaimer: catalog comes from a third party. Shown once.
    bool catalogDisclaimerAcknowledged = false;
    // Web companion LAN server (plain HTTP, port 8080). The PIN gates
    // mutating endpoints (fail-closed when empty). A missing or invalid PIN
    // is replaced with a random 6-digit value on load/update so a fresh
    // install is never open on the LAN. Digits only, 4-8 long when set by
    // the user (enforced at parse time so a hand-edited settings.json
    // cannot smuggle odd values).
    bool webServerEnabled = true;
    std::string webServerPin;
    // How many torrents download at once. The default 1 is the serial
    // queue: nothing shares the link, so a single transfer runs at the speed
    // the swarm can actually give it. Raising it splits bandwidth, RAM budget
    // and SD throughput between tasks — opt-in, not something a stock install
    // does behind the user's back. Hand-edited values are clamped to the
    // supported range at parse time.
    uint32_t maxActiveDownloads = 1;
    // Debrid: transfers are fetched over HTTP(S) from a server instead of from
    // peers — TorBox hosted, or a TorrServer the user runs on their own LAN.
    // A fresh install starts debrid-first, so torrenting is off until the user
    // opts in; settings files older than v3 predate the switch and are
    // migrated to true so an upgrade does not silently stop them. The key and
    // the address are stored in the clear, like webServerPin — the SD card
    // offers nothing better to hide them behind.
    bool torrentingEnabled = false;
    std::string torboxApiKey;
    // Base URL of the TorrServer instance, e.g. "http://192.168.1.10:8090".
    std::string torrserverUrl;
    std::string realdebridApiKey;
    DebridProviderKind debridProvider = DebridProviderKind::TorBox;
    bool firstRunCompleted = false;
    // Outbound proxy for HTTPS calls the app makes (catalog, artwork,
    // updates, hosted debrid). Empty = direct. Peer traffic does NOT go
    // through it: the torrent engine speaks raw TCP/uTP, not curl. Applied
    // via ALL_PROXY, so plain-HTTP LAN TorrServer requests would also be
    // proxied — leave this empty when using a LAN TorrServer, or point at a
    // proxy that can reach that host. Validated at parse time so a
    // hand-edited settings.json cannot smuggle an odd scheme.
    std::string proxyUrl;
    // Seconds of no controller/touch input before the OLED burn-in saver
    // (ui/common/burn_in_saver.hpp) covers the screen. One of
    // kBurnInIdleSecValues; a hand-edited value snaps to the nearest one at
    // parse time (see clampBurnInIdleSec).
    uint32_t burnInIdleSec = 300;
    // Whether the OLED burn-in saver shows a dim drifting clock. Off keeps
    // the screen fully black (nothing at all) while the saver is up.
    bool burnInShowClock = true;

    bool operator==(const AppSettingsData& other) const;
    bool operator!=(const AppSettingsData& other) const {
        return !(*this == other);
    }
};

// Supported values for AppSettingsData::language, in the order the Settings
// selector lists them. Anything else is rejected at parse time, so a hand-edited
// settings.json cannot leave the app pointing at a locale we do not ship.
inline constexpr const char* kLanguageValues[] = {"auto", "en-US", "es"};

// Supported values for AppSettingsData::themeMode, in the order the
// Settings selector lists them.
inline constexpr const char* kThemeModeValues[] = {"auto", "light", "dark"};

bool isSupportedLanguage(const std::string& value);
bool isSupportedThemeMode(const std::string& value);

// True for a valid web PIN: empty (caller will auto-generate) or 4-8 ASCII digits.
bool isValidWebPin(const std::string& value);

// Random 6-digit numeric PIN for the LAN companion.
std::string generateWebPin();

// Empty (direct) or scheme://host[:port] with scheme one of http, https,
// socks4, socks5, socks5h. curl accepts far more than that; this is the
// subset worth supporting on a console, and rejecting the rest early beats
// every request failing with a curl error nobody can read.
bool isValidProxyUrl(const std::string& value);

// Empty (built-in FreeShop source) or https:// with a non-empty host and
// path, no userinfo, max 512 chars — same rules enforced at parse and in the
// UI.
bool isValidCatalogSourceUrl(const std::string& value);

// User override or the built-in switch_games.json URL.
std::string effectiveCatalogSourceUrl(const std::string& custom);

// Points libcurl at the proxy for every handle the app creates, including
// ones already constructed — curl re-reads the environment per transfer, so
// a change takes effect on the next request rather than the next launch.
// Empty clears it. Global by design: threading a setting through nine
// independent curl_easy_init() call sites buys nothing over one env var.
void applyProxySetting(const std::string& proxyUrl);

// The supported range for AppSettingsData::maxActiveDownloads lives in
// download_manager.hpp: it is the engine's limit, and settings only validate
// against it. Include that header where you need clampMaxActiveDownloads.

// Selectable values for AppSettingsData::burnInIdleSec, in the order the
// Settings selector lists them. A short 15s option is included on purpose —
// it is what you pick to actually see the saver kick in while testing,
// nobody would want it that low for daily use.
inline constexpr uint32_t kBurnInIdleSecValues[] = {15, 30, 60, 120, 300, 600, 1800};

// Snaps an arbitrary (e.g. hand-edited) value to the closest entry in
// kBurnInIdleSecValues, same "degrade to nearest supported value" contract
// as clampMaxActiveDownloads.
uint32_t clampBurnInIdleSec(uint64_t value);

bool dailyRefreshDue(uint64_t nowMs, uint64_t lastRefreshMs);

// True when `epochSec` falls on the local calendar day of `time(nullptr)`.
// epochSec <= 0 is never "today" (unknown / never refreshed).
bool isLocalToday(int64_t epochSec);

class AppSettings {
public:
    explicit AppSettings(std::string path,
                         std::string legacyTelemetryPath = {});

    bool load(std::string& error);
    bool update(const AppSettingsData& values, std::string& error);
    bool reset(std::string& error);

    const AppSettingsData& get() const { return values_; }
    uint64_t generation() const { return generation_; }
    const std::string& path() const { return path_; }

private:
    bool write(const AppSettingsData& values, std::string& error) const;

    std::string path_;
    std::string legacyTelemetryPath_;
    AppSettingsData values_;
    uint64_t generation_ = 0;
};

} // namespace pipensx
