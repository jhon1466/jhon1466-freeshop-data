#pragma once

#include "app_settings.hpp"
#include "download_manager.hpp"

#include <string>

namespace pipensx {

// Whitelist JSON for the web companion Settings tab: lets a phone or PC edit
// the handful of AppSettings fields that are painful to type with the
// Switch's on-screen keyboard (API keys, a proxy URL) instead of the console
// keyboard. Secrets (torboxApiKey/torrserverUrl/realdebridApiKey/proxyUrl,
// which can carry embedded credentials) are reported as a "set" boolean,
// never their value - this JSON goes out over plain LAN HTTP.
std::string companionSettingsJson(const AppSettingsData& values);

// Partial update of the companion whitelist. Unknown top-level keys fail
// (typo guard). An empty string clears a secret field; omitted keys are left
// alone. Validates the same way the Settings screen does (proxy scheme,
// download-count range) so a bad PATCH can't smuggle in an invalid value.
bool applyCompanionSettingsPatch(AppSettingsData& values,
                                 const std::string& jsonBody,
                                 std::string& error);

// Runtime side effects SettingsView's persist() already applies after a
// console edit - the companion PATCH needs the same ones. Catalog
// URL/filter and the web server's own pin/port stay console-only actions.
inline void applyCompanionSettingsRuntime(const AppSettingsData& values,
                                          DownloadManager& manager) {
    manager.setMaxActiveDownloads(values.maxActiveDownloads);
    manager.setInstallTarget(values.installLocation ==
                                     InstallLocation::SystemMemory
                                 ? install::InstallStorageTarget::Nand
                                 : install::InstallStorageTarget::SdCard);
    manager.setTorboxApiKey(values.torboxApiKey);
    manager.setTorrserverUrl(values.torrserverUrl);
    manager.setRealdebridApiKey(values.realdebridApiKey);
    applyProxySetting(values.proxyUrl);
}

}  // namespace pipensx
