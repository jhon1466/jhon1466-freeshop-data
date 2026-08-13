#include "companion_settings.hpp"

#include "torrserver_provider.hpp"

#include <borealis/extern/nlohmann/json.hpp>

namespace pipensx {
namespace {

using Json = nlohmann::json;

const char* debridProviderName(DebridProviderKind kind) {
    return kind == DebridProviderKind::TorrServer ? "torrserver" : "torbox";
}

bool parseDebridProvider(const std::string& text, DebridProviderKind& out) {
    if (text == "torbox") {
        out = DebridProviderKind::TorBox;
        return true;
    }
    if (text == "torrserver") {
        out = DebridProviderKind::TorrServer;
        return true;
    }
    return false;
}

const char* installLocationName(InstallLocation location) {
    return location == InstallLocation::SystemMemory ? "system" : "sdcard";
}

bool parseInstallLocation(const std::string& text, InstallLocation& out) {
    if (text == "sdcard") {
        out = InstallLocation::SdCard;
        return true;
    }
    if (text == "system") {
        out = InstallLocation::SystemMemory;
        return true;
    }
    return false;
}

}  // namespace

std::string companionSettingsJson(const AppSettingsData& values) {
    Json j;
    j["debridProvider"] = debridProviderName(values.debridProvider);
    j["torrentingEnabled"] = values.torrentingEnabled;
    j["installLocation"] = installLocationName(values.installLocation);
    j["maxActiveDownloads"] = values.maxActiveDownloads;
    // Secrets never round-trip as values over LAN HTTP - only whether one is
    // set, so the companion UI can show "configured" without ever handling
    // the key itself.
    j["torboxApiKeySet"] = !values.torboxApiKey.empty();
    j["torrserverUrlSet"] = !values.torrserverUrl.empty();
    j["proxyUrlSet"] = !values.proxyUrl.empty();
    return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

bool applyCompanionSettingsPatch(AppSettingsData& values,
                                 const std::string& jsonBody,
                                 std::string& error) {
    error.clear();
    Json body;
    try {
        body = Json::parse(jsonBody);
    } catch (const Json::exception&) {
        error = "Invalid JSON body.";
        return false;
    }
    if (!body.is_object()) {
        error = "Body must be a JSON object.";
        return false;
    }

    static const char* kKnownKeys[] = {
        "debridProvider",   "torrentingEnabled", "installLocation",
        "maxActiveDownloads", "torboxApiKey",    "torrserverUrl",
        "proxyUrl",
    };
    for (auto it = body.begin(); it != body.end(); ++it) {
        bool known = false;
        for (const char* key : kKnownKeys)
            if (it.key() == key) { known = true; break; }
        if (!known) {
            error = "Unknown setting: " + it.key();
            return false;
        }
    }

    if (body.contains("debridProvider")) {
        if (!body["debridProvider"].is_string() ||
            !parseDebridProvider(body["debridProvider"].get<std::string>(),
                                 values.debridProvider)) {
            error = "debridProvider must be \"torbox\" or \"torrserver\".";
            return false;
        }
    }
    if (body.contains("torrentingEnabled")) {
        if (!body["torrentingEnabled"].is_boolean()) {
            error = "torrentingEnabled must be a boolean.";
            return false;
        }
        values.torrentingEnabled = body["torrentingEnabled"].get<bool>();
    }
    if (body.contains("installLocation")) {
        if (!body["installLocation"].is_string() ||
            !parseInstallLocation(body["installLocation"].get<std::string>(),
                                  values.installLocation)) {
            error = "installLocation must be \"sdcard\" or \"system\".";
            return false;
        }
    }
    if (body.contains("maxActiveDownloads")) {
        if (!body["maxActiveDownloads"].is_number_unsigned()) {
            error = "maxActiveDownloads must be a non-negative integer.";
            return false;
        }
        values.maxActiveDownloads =
            clampMaxActiveDownloads(body["maxActiveDownloads"].get<uint64_t>());
    }
    if (body.contains("torboxApiKey")) {
        if (!body["torboxApiKey"].is_string()) {
            error = "torboxApiKey must be a string.";
            return false;
        }
        values.torboxApiKey = body["torboxApiKey"].get<std::string>();
    }
    if (body.contains("torrserverUrl")) {
        if (!body["torrserverUrl"].is_string()) {
            error = "torrserverUrl must be a string.";
            return false;
        }
        values.torrserverUrl = TorrserverProvider::normalizeBaseUrl(
            body["torrserverUrl"].get<std::string>());
    }
    if (body.contains("proxyUrl")) {
        if (!body["proxyUrl"].is_string()) {
            error = "proxyUrl must be a string.";
            return false;
        }
        std::string proxyUrl = body["proxyUrl"].get<std::string>();
        if (!isValidProxyUrl(proxyUrl)) {
            error = "proxyUrl must be empty or scheme://host[:port] "
                    "(http, https, socks4, socks5, socks5h).";
            return false;
        }
        values.proxyUrl = std::move(proxyUrl);
    }
    return true;
}

}  // namespace pipensx
