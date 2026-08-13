#include "app/app_settings.hpp"
#include "app/catalog_service.hpp"
#include "app/companion_settings.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "app/update_service.hpp"
#include "app/web_server.hpp"
#include "platform/switch_crashlog.h"
#include "platform/switch_performance.hpp"

extern "C" {
#include "core/dht.h"
#include "core/util.h"
}

#include <borealis.hpp>
#include <borealis/core/audio.hpp>
#include <curl/curl.h>
#include <switch.h>
#include <switch-ipcext.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "app/mod_index_service.hpp"
#include "ui/catalog/catalog_view.hpp"
#include "ui/common/burn_in_saver.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/web_qr.hpp"
#include "ui/first_run_view.hpp"
#include "ui/i18n.hpp"
#include "ui/main_frame.hpp"
#include "ui/mtp_view.hpp"
#include "ui/downloads/downloads_view.hpp"
#include "ui/explorer/explorer_view.hpp"
#include "ui/installed/installed_view.hpp"
#include "ui/saves/saves_view.hpp"
#include "ui/settings/about_view.hpp"
#include "ui/settings/help_view.hpp"
#include "ui/settings/settings_view.hpp"
#include "ui/theme.hpp"
#include "ui/update_notes_view.hpp"
#include "ui/welcome_view.hpp"

using pipensx::AppSettings;
using pipensx::CatalogService;
using pipensx::DownloadManager;
using pipensx::GameMetadataService;
using pipensx::InstalledTitleService;
using pipensx::FavoritesService;
using pipensx::ModIndexService;
using pipensx::SwitchPerformanceController;
using pipensx::UpdateCheckResult;
using pipensx::UpdateService;
using pipensx::WebServer;

using namespace pipensx::ui;

namespace {

constexpr const char* BundledCatalogPath =
    "romfs:/catalog/switch_games.json.zst";

// AppSettingsData::language -> the borealis locale to load. LOCALE_AUTO makes
// SwitchPlatform read the console's system language, so a Spanish console gets
// a Spanish UI with no user action; anything we do not ship a locale directory
// for falls back to en-US per key.
//
// SwitchPlatform stores the raw libnx language code as-is (switch_platform.cpp
// reinterprets LanguageCode's bytes directly, no enum mapping), and borealis's
// loadLocale() does exact romfs directory-name matching with no prefix
// fallback - so "Auto" only works if we ship a folder matching the console's
// exact code. The Switch reports two different Spanish codes depending on the
// system language chosen in System Settings: "es" for Español (España) and
// "es-419" for Español (Latinoamérica). Both resources/i18n/es/ and
// resources/i18n/es-419/ exist (identical content) so Auto resolves either
// one to Spanish instead of silently falling back to English.
// Joins on scope exit so an exception between spawn and the explicit join
// unwinds cleanly instead of hitting std::terminate in ~thread().
struct ThreadJoiner {
    std::thread thread;
    ~ThreadJoiner() {
        if (thread.joinable())
            thread.join();
    }
};

const std::string& borealisLocaleFor(const std::string& language) {
    if (language == "es")
        return brls::LOCALE_ES;
    if (language == "en-US")
        return brls::LOCALE_EN_US;
    return brls::LOCALE_AUTO;
}

class MainActivity : public brls::Activity {
public:
    MainActivity(DownloadManager* manager, CatalogService* catalog,
                 GameMetadataService* metadata,
                 InstalledTitleService* installed, AppSettings* settings,
                 UpdateService* updater, ModIndexService* mods,
                 FavoritesService* favorites, WebServer* webServer)
        : manager_(manager), catalog_(catalog), metadata_(metadata),
          installed_(installed), settings_(settings), updater_(updater),
          mods_(mods), favorites_(favorites), webServer_(webServer) {
        auto* tabs = new pipensx::ui::MainFrame();
        using pipensx::ui::NavIconType;
        tabs->addNavTab(tr("pipensx/nav/catalog"), NavIconType::Catalog,
                        [manager, catalog, metadata, installed,
                         settings, mods, favorites, tabs] {
            return new CatalogView(manager, catalog, metadata, installed,
                                   settings, [tabs] { tabs->focusTab(1); },
                                   mods, favorites);
        });
        tabs->addNavTab(tr("pipensx/nav/downloads"), NavIconType::Downloads,
                        [manager, catalog, metadata, settings] {
            return new MainView(manager, catalog, metadata, settings);
        });
        tabs->addNavTab(tr("pipensx/nav/installed"), NavIconType::Installed,
                        [installed, manager, metadata, settings, catalog] {
            return new InstalledView(installed, manager, metadata, settings,
                                     catalog);
        });
        tabs->addNavTab(tr("pipensx/nav/explorer"), NavIconType::Explorer,
                        [settings] {
            return new ExplorerView(settings);
        });
        tabs->addNavTab(tr("pipensx/nav/saves"), NavIconType::Saves,
                        [installed, metadata] {
            return new SavesView(installed, metadata);
        });
        tabs->addNavTab(tr("pipensx/nav/mtp"), NavIconType::Mtp,
                        [settings] {
            return new MtpView(settings);
        });
        tabs->addNavTab(tr("pipensx/nav/settings"), NavIconType::Settings,
                        [settings, manager, catalog, metadata,
                         installed, updater, mods, webServer] {
            return new SettingsView(settings, manager, catalog, metadata,
                                    installed, updater, mods, webServer);
        });
        tabs->addNavTab(tr("pipensx/nav/help"), NavIconType::Help,
                        [manager, catalog, metadata, installed] {
            return new HelpView(manager, catalog, metadata, installed);
        });
        tabs->addNavTab(tr("pipensx/nav/about"), NavIconType::About, [] {
            return new AboutView();
        });
        frame_ = new brls::AppletFrame(tabs);
        frame_->setTitle(tr("pipensx/app/title"));

        // Free space + web-companion address + battery/wireless/clock in the
        // header's own right-side slot (top of the screen, matching the
        // console's own status row) instead of borealis's stock bottom-right
        // BottomBar. hint_box is the header's second child (see vendor/
        // borealis's appletFrameXML) — reached the same way addNavTab reaches
        // sidebar internals, through the public Box::getChildren() surface
        // rather than a vendor patch.
        //
        // The footer itself must stay VISIBLE: it also carries the button
        // action hints (A/B/Y/etc, brls::Hints), not just the battery/
        // wireless/clock trio — hiding the whole footer silently dropped
        // every hint along with it. Only the now-duplicated battery/
        // wireless/clock sub-views are hidden, by their borealis-internal
        // ids (see vendor/borealis's bottomBarXML).
        if (brls::Box* footer = frame_->getFooter()) {
            if (brls::View* battery = footer->getView("brls/battery"))
                battery->setVisibility(brls::Visibility::GONE);
            if (brls::View* wireless = footer->getView("brls/wireless"))
                wireless->setVisibility(brls::Visibility::GONE);
            if (brls::View* clock = footer->getView("brls/hints/time"))
                clock->setVisibility(brls::Visibility::GONE);
        }
        if (brls::Box* header = frame_->getHeader()) {
            std::vector<brls::View*>& headerKids = header->getChildren();
            if (headerKids.size() >= 2) {
                if (auto* hintBox = dynamic_cast<brls::Box*>(headerKids[1]))
                    hintBox->addView(
                        new pipensx::ui::TopStatusRow(manager, webServer));
            }
        }
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        // Hidden hint: this action sits on the frame, so its label would ride
        // the bottom bar on every screen under MainActivity — and the catalog
        // already registers more hints than a 1280px bar holds in Russian.
        // Plus-to-exit is a console convention, and HOME works regardless.
        registerAction(tr("pipensx/app/exit"), brls::BUTTON_START,
            [this](brls::View*) {
                startupStage("quit requested by Plus");
                brls::Application::quit();
                return true;
            }, /*hidden=*/true);
        // Visible on every screen: the web companion QR is the whole pairing
        // story, so it must not stay buried three levels deep in Settings.
        registerAction(tr("pipensx/app/web_qr"), brls::BUTTON_BACK,
            [this](brls::View*) {
                const pipensx::AppSettingsData& values = settings_->get();
                const std::string url = pipensx::ui::webCompanionUrl(
                    webServer_, values.webServerEnabled);
                if (url.empty()) {
                    brls::Application::notify(
                        tr(values.webServerEnabled
                               ? "pipensx/settings/web_address_none"
                               : "pipensx/web/off"));
                    return true;
                }
                pipensx::ui::showWebQrDialog(url, values.webServerPin);
                return true;
            });
    }

private:
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    AppSettings* settings_;
    UpdateService* updater_;
    ModIndexService* mods_;
    FavoritesService* favorites_;
    WebServer* webServer_;
    brls::AppletFrame* frame_;
};

}  // namespace

int main(int argc, char** argv) {
    // A library applet must only terminate after qlaunch asks it to close.
    // Keep this path before logging, settings, and custom signal handlers so
    // the unsupported mode uses only libnx's normal applet lifecycle.
    if (!isApplicationMode()) {
        showApplicationModeRequired();
        return 0;
    }

    switch_crashlog_install();
    switch_crashlog_stage("creating application directories");
    mkdir("sdmc:/switch", 0755);
    mkdir("sdmc:/switch/freeshop-client", 0755);
    log_init(LogPath);

    // The path this process actually launched from. UpdateService needs
    // this as its target - its hardcoded default assumes the app lives at
    // sdmc:/switch/freeshop-client/freeshop-client.nro, which isn't where
    // every install actually is (dropped straight in sdmc:/switch/, a
    // differently-named copy, etc). Updating against the wrong assumed
    // path doesn't fail loudly - it stages the download onto whatever file
    // happens to sit at the hardcoded path, which may not exist yet or may
    // be a stale copy from a previous experiment, leaving two separate
    // launchable .nro's on the card instead of replacing the one actually
    // in use.
    std::string resolvedSelfPath =
        (argc > 0 && argv[0] && argv[0][0]) ? argv[0] : std::string();

    // Legacy self-update compatibility (v1.6.4 and earlier, pre-Borealis).
    // Those clients download the new build to "<self>.update" and
    // chain-load straight into it, expecting THAT process to swap itself
    // onto the canonical path (see the removed self_update.c's
    // self_update_finish_swap). This codebase's own updater (UpdateService/
    // update_transaction.c) uses a completely different protocol via a
    // separate helper .nro and has no idea it might be running from a
    // ".update" staging copy - without this check, a v1.6.4-initiated
    // update runs once from the leftover file and never touches the real
    // launch path, so closing and reopening from hbmenu goes right back to
    // the old binary.
    //
    // Deliberately does NOT chain-load into the canonical path afterward
    // (unlike the old code): envSetNextLoad-ing a Borealis app into itself
    // re-initializes the graphics/applet/service stack a second time in the
    // same hbloader session and hangs at a black screen - confirmed on
    // hardware, and exactly why update_helper.c's own comment says the same
    // about relaunching the main app. Just fix the file on disk and keep
    // running this same process normally; the swap is what future launches
    // needed anyway.
    if (!resolvedSelfPath.empty()) {
        const std::string suffix = ".update";
        if (resolvedSelfPath.size() > suffix.size() &&
            resolvedSelfPath.compare(resolvedSelfPath.size() - suffix.size(),
                                     suffix.size(), suffix) == 0) {
            const std::string canonical = resolvedSelfPath.substr(
                0, resolvedSelfPath.size() - suffix.size());
            log_msg("[startup] legacy update hop: %s -> %s\n",
                    resolvedSelfPath.c_str(), canonical.c_str());
            std::remove(canonical.c_str());
            bool swapped =
                std::rename(resolvedSelfPath.c_str(), canonical.c_str()) == 0;
            if (!swapped) {
                // sdmc:'s rename() can refuse to replace an existing
                // destination even right after remove() (same quirk
                // worked around in install_journal.cpp's journal save) -
                // fall back to a full copy.
                std::ifstream input(resolvedSelfPath, std::ios::binary);
                std::ofstream output(canonical,
                                     std::ios::binary | std::ios::trunc);
                if (input && output) {
                    output << input.rdbuf();
                    swapped = static_cast<bool>(output);
                    output.flush();
                }
                if (swapped)
                    std::remove(resolvedSelfPath.c_str());
                log_msg("[startup] legacy update hop: rename failed, copy "
                        "fallback %s\n", swapped ? "ok" : "failed");
            }
            log_msg("[startup] legacy update hop: swap %s, continuing in "
                    "this process\n", swapped ? "ok" : "failed");
            // Either way, the real file (updated or not) now lives at
            // `canonical`, not the ".update" path this process started at.
            resolvedSelfPath = canonical;
        }
    }
    const std::string updateTargetPath = !resolvedSelfPath.empty()
        ? resolvedSelfPath
        : "sdmc:/switch/freeshop-client/freeshop-client.nro";
    log_msg("[startup] update target path: %s\n", updateTargetPath.c_str());

    UpdateService launchUpdater(updateTargetPath);
    const bool updatePendingConfirmation =
        launchUpdater.hasPendingConfirmation();
    // A verified download staged by a previous session that quit before the
    // helper finished the swap. Do NOT auto-chain to the helper here: an
    // unconditional envSetNextLoad + quit on every launch turns a single failed
    // helper load into a crash loop that bricks the app. It is surfaced as a
    // user-triggered "install now?" prompt once the UI is up (see below).
    const bool updatePendingFinish = launchUpdater.stagedReady();
    AppSettings settings(SettingsPath, TelemetryFlagPath);
    std::string settingsError;
    if (!settings.load(settingsError))
        diagnostic_error("settings", "startup", "error=%s",
                         settingsError.c_str());
    telemetry_set_enabled(settings.get().extendedTelemetry ? 1 : 0);
    // Before curl_global_init and before any service builds a handle: curl
    // reads the proxy from the environment when it sets up a transfer.
    pipensx::applyProxySetting(settings.get().proxyUrl);
    if (!settings.get().proxyUrl.empty())
        log_msg("[startup] proxy %s\n", settings.get().proxyUrl.c_str());
    log_msg("[telemetry] setting enabled=%d interval_ms=5000 build='%s %s'\n",
            telemetry_enabled(), __DATE__, __TIME__);
    log_msg("[TEST] build %s arrancando - marcador de prueba de autoupdate\n",
            PIPENSX_VERSION);
    startupStage("entered main");

    openBorealisLog();

    std::set_terminate([] {
        switch_crashlog_stage("uncaught C++ exception");
        // "std::terminate called" on its own names nothing. Rethrowing the
        // in-flight exception is the only way to get its type and message
        // into the log, and it is safe here: we _Exit either way.
        if (std::exception_ptr current = std::current_exception()) {
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& e) {
                log_msg("[crash] std::terminate: %s: %s\n",
                        typeid(e).name(), e.what());
            } catch (...) {
                log_msg("[crash] std::terminate: non-std exception\n");
            }
        } else {
            log_msg("[crash] std::terminate called with no live exception\n");
        }
        log_flush();
        std::_Exit(134);
    });

    bool curlReady = false;
    bool ncmReady = false;
    bool nsReady = false;
    bool esReady = false;
    bool accountReady = false;
    try {
        log_msg("[startup] applet_type=%d operation_mode=%d\n",
                (int)appletGetAppletType(), (int)appletGetOperationMode());

        // Our own sockets pass MSG_NOSIGNAL, and every curl handle now sets
        // CURLOPT_NOSIGNAL — which also stops libcurl asking the system to
        // ignore SIGPIPE. Do it here instead, as main_pc.c already does.
        signal(SIGPIPE, SIG_IGN);

        startupStage("curl_global_init");
        CURLcode curlResult = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (curlResult != CURLE_OK) {
            log_msg("[startup] curl_global_init failed: %d\n",
                    (int)curlResult);
            throw std::runtime_error("curl_global_init failed");
        }
        curlReady = true;

        startupStage("installer services");
        Result rc = ncmInitialize();
        if (R_FAILED(rc))
            throw std::runtime_error("ncmInitialize failed");
        ncmReady = true;
        rc = nsInitialize();
        if (R_FAILED(rc))
            throw std::runtime_error("nsInitialize failed");
        nsReady = true;
        rc = esInitialize();
        if (R_FAILED(rc))
            throw std::runtime_error("esInitialize failed");
        esReady = true;
        // acc:u0 - the Saves tab needs the active profile to open another
        // title's save data. Not fatal on failure: Saves just reports no
        // profile available rather than blocking the whole app over it.
        accountReady = R_SUCCEEDED(accountInitialize(AccountServiceType_Application));

        startupStage("Borealis Application::init");
        // Must precede init(): the platform captures the locale in its
        // constructor and Application::init() loads translations exactly once,
        // which is why a language change only lands on the next launch.
        brls::Platform::APP_LOCALE_DEFAULT =
            borealisLocaleFor(settings.get().language);
        if (!brls::Application::init())
            throw std::runtime_error("Borealis Application::init failed");
        pipensx::ui::theme::registerColors();
        pipensx::ui::installSidebarStyle();
        // Re-enabled: the std::terminate crash hardware testing traced to
        // "right after an action fires" turned out to be a std::thread
        // spawned from deep inside Application::handleAction()'s own call
        // stack overflowing the main thread (fixed by moving Guardados/
        // Explorador/Debrid to brls::async instead) - audio itself was
        // never the cause, it just shared the timing. See save_detail_
        // activity.hpp's makeBackup() for the actual fix.
        brls::AudioPlayer::enabled = settings.get().soundEffectsEnabled;

        startupStage("Borealis createWindow");
        brls::Application::createWindow("pipensx");
        brls::Application::setGlobalQuit(false);

        // "auto" leaves whatever SwitchPlatform's constructor already read
        // from the console's own theme setting - only override for an
        // explicit choice. Must come after createWindow(): setThemeVariant()
        // re-records the deko3d static command list against the video
        // context's swapchain/framebuffers, which do not exist yet before
        // createWindow() runs - calling it earlier crashed on hardware with
        // a null-pointer data abort.
        if (settings.get().themeMode == "light")
            brls::Application::getPlatform()->setThemeVariant(
                brls::ThemeVariant::LIGHT);
        else if (settings.get().themeMode == "dark")
            brls::Application::getPlatform()->setThemeVariant(
                brls::ThemeVariant::DARK);

        startupStage("CatalogService construction");
        log_msg("[startup] image relay: relays-first + disk cache (rev4)\n");
        unlink("sdmc:/switch/freeshop-client/rutracker.cfg");
        unlink("sdmc:/switch/freeshop-client/rutracker_cookies.txt");
        CatalogService catalog("sdmc:/switch/freeshop-client", BundledCatalogPath);

        // The metadata index parse (an ~8 MB JSON) runs on a worker thread in
        // parallel with the catalog parse below; the service is not touched by
        // anything else until the join before MainActivity construction, after
        // which all access is UI-thread as before. Startup pays
        // max(catalog, metadata) instead of their sum.
        startupStage("GameMetadataService construction");
        GameMetadataService metadata("sdmc:/switch/freeshop-client");
        std::string metadataError;
        bool metadataOk = true;
        ThreadJoiner metadataLoader{
            std::thread([&metadata, &metadataError, &metadataOk] {
                metadataOk = runGuarded(
                    [&](std::string& err) { return metadata.load(err); },
                    metadataError);
            })};

        std::string catalogError;
        if (!catalog.load(catalogError))
            log_msg("[catalog] initial load failed: %s\n",
                    catalogError.c_str());

        startupStage("ModIndexService construction");
        ModIndexService mods("sdmc:/switch/freeshop-client");
        std::string modsError;
        if (!mods.load(modsError))
            log_msg("[mods] initial load skipped: %s\n", modsError.c_str());

        startupStage("FavoritesService construction");
        FavoritesService favorites("sdmc:/switch/freeshop-client");
        std::string favoritesError;
        if (!favorites.load(favoritesError))
            log_msg("[favorites] initial load skipped: %s\n",
                    favoritesError.c_str());

        // The installed-title scan does one full control-data IPC read (NACP +
        // up to 128 KB icon) per installed title — on a full console this was
        // the single largest startup cost, all before the first frame. The
        // service is internally locked and the UI already refreshes it via
        // brls::async, so run the initial scan on its own thread and let the
        // UI come up immediately; the list fills in when the scan lands.
        startupStage("InstalledTitleService refresh (async)");
        InstalledTitleService installed("sdmc:/switch/freeshop-client");
        ThreadJoiner installedScanner{std::thread([&installed] {
            std::string installedError;
            if (!runGuarded(
                    [&](std::string& err) { return installed.refresh(err); },
                    installedError))
                diagnostic_error("installed", "startup", "error=%s",
                                 installedError.c_str());
        })};

        startupStage("DownloadManager construction");
        SwitchPerformanceController performance;
        // Whole-session, not tied to hasActiveTransfer(): a paused or queued
        // task can start moving bytes again at any moment, and the console
        // sleeping mid-transfer is exactly the failure this exists to avoid.
        performance.setKeepAwake(true);
        dht_engine_set_cache_path("sdmc:/switch/freeshop-client/dht.cache");
        DownloadManager manager("sdmc:/switch/freeshop-client");
        manager.setInstallTarget(
            installTargetFor(settings.get().installLocation));
        manager.setMaxActiveDownloads(settings.get().maxActiveDownloads);
        manager.setTorboxApiKey(settings.get().torboxApiKey);
        manager.setTorrserverUrl(settings.get().torrserverUrl);
        manager.setTorrentingEnabled(settings.get().torrentingEnabled);
        metadata.setImageNetwork(
            manager.hasActiveTransfer()
                ? GameMetadataService::ImageNetwork::Throttled
                : GameMetadataService::ImageNetwork::Full);

        UpdateService updater(updateTargetPath);

        startupStage("WebServer construction");
        WebServer webServer(manager, "romfs:/web", PIPENSX_VERSION);
        webServer.setPin(settings.get().webServerPin);
        webServer.setStreamSelection(settings.get().streamSelection);
        webServer.updateSettingsSnapshot(
            pipensx::companionSettingsJson(settings.get()));
        webServer.updateCatalog(catalog.sharedEntries());
        // Every later adopt() (launch refresh, settings refresh, catalog tab)
        // lands on the UI thread, so this callback keeps the companion's
        // catalogue reference current from all of them.
        catalog.setOnAdopt(
            [&webServer, &metadata](
                std::shared_ptr<const std::vector<pipensx::CatalogEntry>> e) {
                webServer.updateCatalog(std::make_shared<
                    const std::vector<pipensx::CatalogEntry>>(
                    withPreferredDescriptions(*e, metadata,
                                              catalogTextPreference())));
            });
        if (settings.get().webServerEnabled)
            webServer.start();

        // Barrier: from here on the UI reads the metadata service, so the
        // parallel index parse must have landed.
        startupStage("join metadata loader");
        metadataLoader.thread.join();
        if (!metadataOk)
            log_msg("[metadata] initial load failed: %s\n",
                    metadataError.c_str());
        // The web companion is served over the network to a general-purpose
        // browser (no Russian locale option there), so it should always read
        // like the in-app catalog does once metadata is available - refresh
        // the snapshot handed to it at construction with metadata-preferred
        // descriptions now that the loader above has landed.
        webServer.updateCatalog(
            std::make_shared<const std::vector<pipensx::CatalogEntry>>(
                withPreferredDescriptions(*catalog.sharedEntries(), metadata,
                                          catalogTextPreference())));

        startupStage("MainActivity construction");
        auto* activity = new MainActivity(&manager, &catalog, &metadata,
                                          &installed, &settings, &updater,
                                          &mods, &favorites, &webServer);

        startupStage("push MainActivity");
        brls::Application::pushActivity(activity);
        if (updatePendingFinish) {
            // Finish an update staged before a previous quit. User-triggered so
            // a helper that fails to load can never become an automatic loop.
            startupStage("pending update prompt");
            const std::string helper = launchUpdater.helperPath();
            auto* dialog = new brls::Dialog(
                tr("pipensx/settings/update_pending_install"));
            dialog->addButton(tr("pipensx/settings/install_and_restart"),
                              [helper, &launchUpdater] {
#ifdef __SWITCH__
                if (!envHasNextLoad()) {
                    brls::Application::notify(
                        tr("pipensx/settings/update_no_restart"));
                    return;
                }
                // The helper on disk may have been staged by an EARLIER
                // run and never touched since - including one built before
                // a bug fix that shipped in this very build. Re-publish it
                // from this build's own bundled copy every time, so a
                // stale/broken helper can never get reused just because
                // the main .nro was swapped in some other way (manually,
                // etc.) without going through a fresh install().
                std::string refreshError;
                if (!launchUpdater.refreshHelper(refreshError)) {
                    brls::Application::notify(
                        tr("pipensx/settings/update_restart_failed"));
                    return;
                }
                const std::string arguments =
                    "\"" + helper + "\" --finish-update";
                const Result result = envSetNextLoad(helper.c_str(),
                                                     arguments.c_str());
                if (R_FAILED(result)) {
                    brls::Application::notify(
                        tr("pipensx/settings/update_restart_failed"));
                    return;
                }
#endif
                brls::Application::quit();
            });
            dialog->addButton(tr("pipensx/common/later"), [] {});
            dialog->open();
        }
        if (!updatePendingFinish && settings.get().checkForUpdatesOnLaunch) {
            updater.checkAsync([](UpdateCheckResult result) {
                if (!result.ok || !result.updateAvailable)
                    return;
                brls::sync([version = std::move(result.release.version)] {
                    brls::Application::notify(
                        tr("pipensx/settings/update_available_launch", version));
                });
            });
        }

        // No first-run method-choice wizard and no catalog disclaimer: this
        // fork ships one download path (straight from the swarm, against our
        // own catalog) rather than pipensx's upstream choice between that,
        // TorBox and a user-run TorrServer, so there's nothing to ask on
        // first launch. TorrServer/TorBox stay reachable from Settings for
        // anyone who wants them later - only the forced first-run prompts
        // are skipped.
        {
            startupStage("first-run defaults");
            pipensx::AppSettingsData values = settings.get();
            const bool needsDefaults = !values.torrentingEnabled ||
                !values.firstRunCompleted ||
                !values.catalogDisclaimerAcknowledged;
            const bool showWelcome = !values.firstRunCompleted;
            if (needsDefaults) {
                values.torrentingEnabled = true;
                values.firstRunCompleted = true;
                values.catalogDisclaimerAcknowledged = true;
                std::string error;
                if (!settings.update(values, error))
                    log_msg("[settings] first-run defaults persist failed: %s\n",
                            error.c_str());
            }
            if (showWelcome)
                pipensx::ui::showWelcomeScreen();
            manager.setTorrentingEnabled(true);
        }

        startupStage("first main loop");
        bool firstFrame = true;
        uint64_t lastInputMs = now_ms();
        while (true) {
            bool activeTransfer = manager.hasActiveTransfer();
            performance.setCpuBoostActive(activeTransfer);
            metadata.setImageNetwork(
                activeTransfer ? GameMetadataService::ImageNetwork::Throttled
                               : GameMetadataService::ImageNetwork::Full);
            if (!brls::Application::mainLoop())
                break;

            // OLED burn-in guard: after the configured idle delay (Ajustes ->
            // "OLED screen saver delay", default 5 min) without a
            // button/touch, cover the UI with a drifting black saver. Any
            // input dismisses it (including D-pad and touch) and resets the
            // idle clock. Open state is derived from the activity stack so a
            // dismiss cannot desync a bool and stack another saver on the
            // next idle period.
            brls::ControllerState pad {};
            std::vector<brls::RawTouchState> touches;
            auto* input = brls::Application::getPlatform()->getInputManager();
            input->updateUnifiedControllerState(&pad);
            input->updateTouchStates(&touches);
            bool touched = false;
            for (const auto& touch : touches) {
                if (touch.pressed) {
                    touched = true;
                    break;
                }
            }
            const bool saverOpen = pipensx::ui::burnInSaverIsTop();
            const uint64_t burnInIdleMs =
                static_cast<uint64_t>(settings.get().burnInIdleSec) * 1000ULL;
            if (pipensx::ui::controllerHasButtonDown(pad) || touched) {
                lastInputMs = now_ms();
                if (saverOpen)
                    brls::Application::popActivity(
                        brls::TransitionAnimation::NONE);
            } else if (!saverOpen &&
                       now_ms() - lastInputMs >= burnInIdleMs) {
                brls::Application::pushActivity(
                    new pipensx::ui::BurnInSaverActivity(
                        settings.get().burnInShowClock),
                    brls::TransitionAnimation::NONE);
                lastInputMs = now_ms();
            }

            // Companion Settings tab: apply at most one queued remote update
            // per frame. AppSettings/DownloadManager are UI-thread objects,
            // so this can't happen on the server thread itself - see
            // WebServer::pumpSettingsPatch.
            webServer.pumpSettingsPatch(
                [&settings, &manager, &webServer](
                    const std::string& requestJson, std::string& resultJson,
                    std::string& error) {
                    pipensx::AppSettingsData values = settings.get();
                    if (!pipensx::applyCompanionSettingsPatch(
                            values, requestJson, error))
                        return false;
                    std::string updateError;
                    if (!settings.update(values, updateError)) {
                        error = updateError;
                        return false;
                    }
                    pipensx::applyCompanionSettingsRuntime(values, manager);
                    resultJson = pipensx::companionSettingsJson(values);
                    webServer.updateSettingsSnapshot(resultJson);
                    return true;
                });

            if (firstFrame) {
                startupStage("main loop running");
                if (updatePendingConfirmation) {
                    std::string error;
                    if (!launchUpdater.confirmInstalled(error))
                        diagnostic_error("update", "confirm", "error=%s",
                                         error.c_str());
                    else {
                        log_msg("[update] installed update confirmed\n");
                        brls::Application::notify(
                            tr("pipensx/settings/updated_to", PIPENSX_VERSION));
                        const std::string notesPath = launchUpdater.notesPath();
                        std::ifstream notesFile(notesPath, std::ios::binary);
                        if (notesFile) {
                            std::ostringstream buffer;
                            buffer << notesFile.rdbuf();
                            notesFile.close();
                            std::remove(notesPath.c_str());
                            const std::string notes = buffer.str();
                            if (!notes.empty())
                                pipensx::ui::showUpdateNotesScreen(
                                    PIPENSX_VERSION, notes);
                        }
                    }
                }
                firstFrame = false;
            }
        }

        startupStage("manager shutdown");
        // The startup title scan references `installed`, which lives on this
        // stack frame — join before anything here is torn down.
        if (installedScanner.thread.joinable())
            installedScanner.thread.join();
        // The web server goes first: its threads call into manager, so they
        // must be joined before the manager dies.
        webServer.shutdown();
        updater.shutdown();
        manager.shutdown();
        performance.setCpuBoostActive(false);
        performance.setKeepAwake(false);
    } catch (const std::exception& error) {
        log_msg("[crash] exception at stage '%s': %s\n",
                "see previous startup marker", error.what());
    } catch (...) {
        log_msg("[crash] unknown exception\n");
    }

    startupStage("app-owned teardown complete");
    startupStage("cleanup");
    if (accountReady)
        accountExit();
    if (esReady)
        esExit();
    if (nsReady)
        nsExit();
    if (ncmReady)
        ncmExit();
    if (curlReady)
        curl_global_cleanup();
    // Borealis writes into the same handle log_close() owns; drop its pointer
    // before that handle goes away.
    brls::Logger::setLogOutput(nullptr);
    log_close();
    return 0;
}
