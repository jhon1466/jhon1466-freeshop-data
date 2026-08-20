#pragma once

// Settings hub panels. SettingsView (settings_view.hpp) hosts a section rail
// on the left; each rail entry shows one of the panels defined here on the
// right. The panels absorb the former SettingsView long list, the Advanced
// sub-page (proxy -> Network, diagnostics/reset -> System), the Storage
// Manager screen (Storage) and the Network Health screen (Network). Nothing
// here changes persist/network/web-server logic — it only moves.
//
// Fork notes: the General panel keeps the fork's theme / sound / OLED burn-in
// rows, the Source panel keeps the fork's three-provider debrid flow (TorBox,
// TorrServer, Real-Debrid), and every persist call pushes the web companion's
// settings snapshot so GET /api/settings stays in sync with console edits.

#include <atomic>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "app/storage_manager.hpp"
#include "app/update_service.hpp"
#include "app/web_server.hpp"
#include "ui/common/storage_meter.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/web_qr.hpp"
#include "ui/debrid_ui.hpp"
#include "ui/i18n.hpp"
#include "ui/settings/settings_cells.hpp"
#include "ui/theme.hpp"

extern "C" {
#include "../core/dht.h"
}

namespace pipensx::ui {

enum class SettingsSection : size_t {
    General,
    Downloads,
    Source,
    Network,
    Catalog,
    Storage,
    System,
};

inline constexpr size_t kSettingsSectionCount = 7;

inline const char* settingsSectionTag(SettingsSection section) {
    switch (section) {
        case SettingsSection::General: return "general";
        case SettingsSection::Downloads: return "downloads";
        case SettingsSection::Source: return "source";
        case SettingsSection::Network: return "network";
        case SettingsSection::Catalog: return "catalog";
        case SettingsSection::Storage: return "storage";
        case SettingsSection::System: return "system";
    }
    return "unknown";
}

inline const char* settingsSectionLabelKey(SettingsSection section) {
    switch (section) {
        case SettingsSection::General: return "pipensx/settings/section_general";
        case SettingsSection::Downloads: return "pipensx/settings/section_downloads";
        case SettingsSection::Source: return "pipensx/settings/section_debrid";
        case SettingsSection::Network: return "pipensx/settings/section_network";
        case SettingsSection::Catalog: return "pipensx/settings/section_catalog";
        case SettingsSection::Storage: return "pipensx/storage/title";
        case SettingsSection::System: return "pipensx/settings/section_system";
    }
    return "pipensx/settings/section_general";
}

// ---------------------------------------------------------------------------
// Section rail (the settings sidebar)
// ---------------------------------------------------------------------------

// Line glyph for one settings section, drawn in the same 24px box and stroke
// style as the MainFrame nav icons. Colour follows the item's active state.
class SettingsSectionIcon : public brls::View {
public:
    explicit SettingsSectionIcon(SettingsSection section) : section_(section) {
        setWidth(28.0f);
        setHeight(28.0f);
        setAlignSelf(brls::AlignSelf::CENTER);
        setFocusable(false);
        setMarginRight(12.0f);
    }

    void setActive(bool active) {
        active_ = active;
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        const NVGcolor c = active_ ? theme::accent() : theme::textSecondary();
        nvgStrokeColor(vg, c);
        nvgFillColor(vg, c);
        nvgStrokeWidth(vg, 2.0f);
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);

        const float s = 24.0f;
        const float gx = x + (width - s) / 2.0f;
        const float gy = y + (height - s) / 2.0f;
        switch (section_) {
            case SettingsSection::General: drawGeneral(vg, gx, gy, s); break;
            case SettingsSection::Downloads: drawDownloads(vg, gx, gy, s); break;
            case SettingsSection::Source: drawSource(vg, gx, gy, s); break;
            case SettingsSection::Network: drawNetwork(vg, gx, gy, s); break;
            case SettingsSection::Catalog: drawCatalog(vg, gx, gy, s); break;
            case SettingsSection::Storage: drawStorage(vg, gx, gy, s); break;
            case SettingsSection::System: drawSystem(vg, gx, gy, s); break;
        }
    }

private:
    // Gear/cogwheel: universal settings glyph.
    static void drawGeneral(NVGcontext* vg, float gx, float gy, float s) {
        const float cx = gx + s / 2.0f;
        const float cy = gy + s / 2.0f;
        const float r_outer = s / 2.0f - 2.0f;
        const float r_inner = r_outer * 0.45f;
        const int teeth = 8;
        nvgBeginPath(vg);
        for (int i = 0; i < teeth; i++) {
            const float a1 = (float)i * NVG_PI * 2.0f / teeth;
            const float a2 = a1 + NVG_PI / teeth;
            const float a_mid = a1 + NVG_PI / (teeth * 2.0f);
            const float x1_outer = cx + cosf(a1) * r_outer;
            const float y1_outer = cy + sinf(a1) * r_outer;
            const float x2_outer = cx + cosf(a2) * r_outer;
            const float y2_outer = cy + sinf(a2) * r_outer;
            const float x_mid = cx + cosf(a_mid) * (r_outer + 1.5f);
            const float y_mid = cy + sinf(a_mid) * (r_outer + 1.5f);
            if (i == 0) {
                nvgMoveTo(vg, x1_outer, y1_outer);
            } else {
                nvgLineTo(vg, x1_outer, y1_outer);
            }
            nvgLineTo(vg, x_mid, y_mid);
            nvgLineTo(vg, x2_outer, y2_outer);
        }
        nvgClosePath(vg);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r_inner);
        nvgStroke(vg);
    }

    // Down arrow dropping into a tray.
    static void drawDownloads(NVGcontext* vg, float gx, float gy, float s) {
        const float cx = gx + s / 2.0f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, gy + 1.0f);
        nvgLineTo(vg, cx, gy + 14.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 5.0f, gy + 9.0f);
        nvgLineTo(vg, cx, gy + 14.0f);
        nvgLineTo(vg, cx + 5.0f, gy + 9.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 3.0f, gy + 15.0f);
        nvgLineTo(vg, gx + 3.0f, gy + 21.0f);
        nvgLineTo(vg, gx + s - 3.0f, gy + 21.0f);
        nvgLineTo(vg, gx + s - 3.0f, gy + 15.0f);
        nvgStroke(vg);
    }

    // Database cylinder: three stacked ellipses with sides.
    static void drawSource(NVGcontext* vg, float gx, float gy, float s) {
        const float cx = gx + s / 2.0f;
        const float cy = gy + s / 2.0f;
        const float r = s * 0.38f;
        const float h = 3.0f;
        const float dy = 5.5f;
        // Top ellipse
        nvgBeginPath(vg);
        nvgEllipse(vg, cx, cy - dy, r, h);
        nvgStroke(vg);
        // Middle ellipse
        nvgBeginPath(vg);
        nvgEllipse(vg, cx, cy, r, h);
        nvgStroke(vg);
        // Bottom ellipse
        nvgBeginPath(vg);
        nvgEllipse(vg, cx, cy + dy, r, h);
        nvgStroke(vg);
        // Side lines connecting ellipses
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - r, cy - dy);
        nvgLineTo(vg, cx - r, cy + dy);
        nvgMoveTo(vg, cx + r, cy - dy);
        nvgLineTo(vg, cx + r, cy + dy);
        nvgStroke(vg);
    }

    // Wi-fi: three arcs and a dot.
    static void drawNetwork(NVGcontext* vg, float gx, float gy, float s) {
        const float cx = gx + s / 2.0f;
        const float cy = gy + s / 2.0f;
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, 9.0f, NVG_PI + 0.55f, -0.55f, NVG_CW);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, 6.0f, NVG_PI + 0.55f, -0.55f, NVG_CW);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy + 5.5f, 1.6f);
        nvgFill(vg);
    }

    // 2x2 grid: represents a catalog/list view.
    static void drawCatalog(NVGcontext* vg, float gx, float gy, float s) {
        const float cell = 8.5f;
        const float gap = 3.0f;
        const float startX = gx + (s - (cell * 2 + gap)) / 2.0f;
        const float startY = gy + (s - (cell * 2 + gap)) / 2.0f;
        for (int row = 0; row < 2; row++) {
            for (int col = 0; col < 2; col++) {
                const float px = startX + col * (cell + gap);
                const float py = startY + row * (cell + gap);
                nvgBeginPath(vg);
                nvgRoundedRect(vg, px, py, cell, cell, 2.0f);
                nvgStroke(vg);
            }
        }
    }

    // SD card: body, corner notch, contact pins.
    static void drawStorage(NVGcontext* vg, float gx, float gy, float s) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, gx + 2.0f, gy + 3.0f, s - 4.0f, s - 6.0f, 2.0f);
        nvgStroke(vg);
        // Corner cut at the top right.
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + s - 2.0f, gy + 3.0f);
        nvgLineTo(vg, gx + s - 6.0f, gy + 3.0f);
        nvgLineTo(vg, gx + s - 6.0f, gy + 6.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 5.0f, gy + 12.5f);
        nvgLineTo(vg, gx + 10.0f, gy + 12.5f);
        nvgMoveTo(vg, gx + 14.0f, gy + 12.5f);
        nvgLineTo(vg, gx + s - 5.0f, gy + 12.5f);
        nvgStroke(vg);
    }

    // Wrench: maintenance/tools.
    static void drawSystem(NVGcontext* vg, float gx, float gy, float s) {
        const float cx = gx + s / 2.0f;
        const float cy = gy + s / 2.0f;
        // Handle
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx - 6.5f, cy + 8.0f);
        nvgLineTo(vg, cx + 6.5f, cy - 8.0f);
        nvgStrokeWidth(vg, 3.0f);
        nvgStroke(vg);
        nvgStrokeWidth(vg, 2.0f);
        // Jaw
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx + 5.5f, cy - 9.0f);
        nvgLineTo(vg, cx + 9.5f, cy - 5.0f);
        nvgLineTo(vg, cx + 6.5f, cy - 8.0f);
        nvgLineTo(vg, cx + 4.5f, cy - 11.0f);
        nvgClosePath(vg);
        nvgStroke(vg);
        // Jaw opening
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx + 6.0f, cy - 9.5f);
        nvgLineTo(vg, cx + 7.5f, cy - 7.5f);
        nvgStroke(vg);
    }

    SettingsSection section_;
    bool active_ = false;
};

// Accent bar pinned to the left edge of an active section item. Always
// occupies its slot so labels keep a constant inset across the rail.
class SettingsSectionBar : public brls::View {
public:
    SettingsSectionBar() {
        setWidth(4.0f);
        setHeight(36.0f);
        setAlignSelf(brls::AlignSelf::CENTER);
        setFocusable(false);
        setMarginRight(18.0f);  // total left inset 22, matching the rail pad
    }

    void setActive(bool active) {
        active_ = active;
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        if (!active_)
            return;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 2.0f);
        nvgFillColor(vg, theme::accent());
        nvgFill(vg);
    }

private:
    bool active_ = false;
};

// One rail entry. Focus is the selection: gaining focus switches the hub to
// this section, exactly like the main sidebar selects tabs.
class SettingsNavItem : public brls::Box {
public:
    SettingsNavItem(SettingsSection section, std::string label,
                    std::function<void(SettingsSection)> onSelected)
        : brls::Box(brls::Axis::ROW), section_(section),
          onSelected_(std::move(onSelected)) {
        setFocusable(true);
        setHeight(56.0f);
        setAlignItems(brls::AlignItems::CENTER);
        setBackgroundColor(theme::sidebar());

        bar_ = new SettingsSectionBar();
        addView(bar_);

        icon_ = new SettingsSectionIcon(section);
        addView(icon_);

        label_ = new brls::Label();
        label_->setSingleLine(true);
        label_->setGrow(1);
        label_->setFontSize(18);
        label_->setTextColor(theme::textSecondary());
        label_->setText(std::move(label));
        addView(label_);
    }

    void setActive(bool active) {
        active_ = active;
        bar_->setActive(active);
        icon_->setActive(active);
        label_->setTextColor(active ? theme::textPrimary()
                                    : theme::textSecondary());
        setBackgroundColor(active ? theme::sidebarActive()
                                  : theme::sidebar());
    }

    void onFocusGained() override {
        brls::Box::onFocusGained();
        if (onSelected_)
            onSelected_(section_);
    }

    SettingsSection section() const { return section_; }

private:
    SettingsSection section_;
    std::function<void(SettingsSection)> onSelected_;
    SettingsSectionBar* bar_ = nullptr;
    SettingsSectionIcon* icon_ = nullptr;
    brls::Label* label_ = nullptr;
    bool active_ = false;
};

// The rail: a header plus the seven section items. Fixed width, full height.
class SettingsSidebar : public brls::Box {
public:
    explicit SettingsSidebar(std::function<void(SettingsSection)> onSelected)
        : brls::Box(brls::Axis::COLUMN),
          onSelected_(std::move(onSelected)) {
        setWidth(kWidth);
        setAlignSelf(brls::AlignSelf::STRETCH);
        setBackgroundColor(theme::sidebar());
        setPadding(26, 0, 20, 0);

        auto* head = new brls::Label();
        head->setText(tr("pipensx/nav/settings"));
        head->setSingleLine(true);
        head->setFontSize(theme::kFontCaption);
        head->setTextColor(theme::textTertiary());
        head->setMarginBottom(10);
        head->setMarginLeft(24);
        addView(head);

        for (size_t i = 0; i < kSettingsSectionCount; ++i) {
            const auto section = static_cast<SettingsSection>(i);
            auto* item = new SettingsNavItem(
                section, tr(settingsSectionLabelKey(section)),
                [this](SettingsSection picked) {
                    if (onSelected_)
                        onSelected_(picked);
                });
            item->setId(std::string("settings-nav-") +
                        settingsSectionTag(section));
            items_[i] = item;
            addView(item);
        }
    }

    SettingsNavItem* item(SettingsSection section) {
        return items_[static_cast<size_t>(section)];
    }

    void setActive(SettingsSection section) {
        for (size_t i = 0; i < kSettingsSectionCount; ++i)
            items_[i]->setActive(static_cast<SettingsSection>(i) == section);
    }

    static constexpr float kWidth = 296.0f;

private:
    std::function<void(SettingsSection)> onSelected_;
    SettingsNavItem* items_[kSettingsSectionCount] = {};
};

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------

// Common pane scaffold: padded content inside a scrolling frame that fills
// the right-hand column. Panels stay constructed (and hidden) so switching
// sections is instant and every cell keeps its state.
class SettingsPanel : public brls::Box {
public:
    SettingsPanel() : brls::Box(brls::Axis::COLUMN) {
        setGrow(1);
        content_ = new brls::Box(brls::Axis::COLUMN);
        content_->setPadding(24, 34, 24, 34);
        scroll_ = new brls::ScrollingFrame();
        scroll_->setGrow(1);
        scroll_->setContentView(content_);
        addView(scroll_);
    }

    // The panel became the visible one.
    virtual void onShown() {}

    // Re-sync cells from persisted settings (after a factory reset, a
    // first-run chooser round-trip, or the tab coming back on screen).
    virtual void applyValues() {}

protected:
    brls::Box* content_ = nullptr;
    brls::ScrollingFrame* scroll_ = nullptr;
};

// --- General: language, theme, launch behaviour ----------------------------

class GeneralPanel : public SettingsPanel {
public:
    GeneralPanel(AppSettings* settings, WebServer* webServer)
        : settings_(settings), webServer_(webServer) {
        addSection(content_, tr("pipensx/settings/section_general"));
        language_ = new brls::SelectorCell();
        language_->init(tr("pipensx/settings/language"),
            {tr("pipensx/settings/language_auto"),
             tr("pipensx/settings/language_en"),
             tr("pipensx/settings/language_es"),
             tr("pipensx/settings/language_zh"),
             tr("pipensx/settings/language_fr")},
            languageIndex(settings_->get().language),
            [this](int selected) {
                AppSettingsData values = settings_->get();
                const std::string previous = values.language;
                values.language = kLanguageValues[selected];
                if (!persistSettings(settings_, values, "language", webServer_)) {
                    language_->setSelection(languageIndex(previous), true);
                    return;
                }
                // Borealis loads translations once, inside Application::init().
                brls::Application::notify(
                    tr("pipensx/settings/language_restart"));
            });
        content_->addView(language_);

        theme_ = new brls::SelectorCell();
        theme_->init(tr("pipensx/settings/theme"),
            {tr("pipensx/settings/theme_auto"),
             tr("pipensx/settings/theme_light"),
             tr("pipensx/settings/theme_dark")},
            themeModeIndex(settings_->get().themeMode),
            [this](int selected) {
                AppSettingsData values = settings_->get();
                const std::string previous = values.themeMode;
                values.themeMode = kThemeModeValues[selected];
                if (!persistSettings(settings_, values, "theme_mode", webServer_)) {
                    theme_->setSelection(themeModeIndex(previous), true);
                    return;
                }
                // Most of the app's own colors are set once when each
                // screen is built (setBackgroundColor(theme::x()) etc.),
                // not re-evaluated every frame - only borealis's own
                // generic focus highlight reads the live theme per frame.
                // Applying immediately made just that one highlight flip
                // while everything else stayed on the old colors until
                // restart, which read as broken. Same restart notice as
                // language, for the same reason.
                brls::Application::notify(
                    tr("pipensx/settings/theme_restart"));
            });
        content_->addView(theme_);

        soundEffects_ = new brls::BooleanCell();
        soundEffects_->init(tr("pipensx/settings/sound_effects"),
            settings_->get().soundEffectsEnabled,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.soundEffectsEnabled;
                values.soundEffectsEnabled = enabled;
                if (!persistSettings(settings_, values, "sound_effects", webServer_))
                    soundEffects_->setOn(previous, false);
                else
                    brls::AudioPlayer::enabled = enabled;
            });
        content_->addView(soundEffects_);

        burnInIdle_ = new brls::SelectorCell();
        burnInIdle_->init(tr("pipensx/settings/burn_in_idle"),
            {"15 s", "30 s", "1 min", "2 min", "5 min", "10 min", "30 min"},
            burnInIdleIndex(settings_->get().burnInIdleSec),
            [this](int selected) {
                AppSettingsData values = settings_->get();
                uint32_t previous = values.burnInIdleSec;
                values.burnInIdleSec = kBurnInIdleSecValues[selected];
                if (!persistSettings(settings_, values, "burn_in_idle_sec", webServer_)) {
                    burnInIdle_->setSelection(burnInIdleIndex(previous), true);
                }
                // main_switch's loop reads settings_->get().burnInIdleSec
                // live every frame - no restart needed.
            });
        content_->addView(burnInIdle_);

        burnInShowClock_ = new brls::BooleanCell();
        burnInShowClock_->init(tr("pipensx/settings/burn_in_clock"),
            settings_->get().burnInShowClock,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.burnInShowClock;
                values.burnInShowClock = enabled;
                if (!persistSettings(settings_, values, "burn_in_show_clock", webServer_))
                    burnInShowClock_->setOn(previous, false);
                // main_switch reads settings_->get().burnInShowClock when it
                // next opens the saver - no restart needed.
            });
        content_->addView(burnInShowClock_);

        checkForUpdates_ = new brls::BooleanCell();
        checkForUpdates_->init(tr("pipensx/settings/check_updates"),
            settings_->get().checkForUpdatesOnLaunch,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.checkForUpdatesOnLaunch;
                values.checkForUpdatesOnLaunch = enabled;
                if (!persistSettings(settings_, values, "update_check", webServer_))
                    checkForUpdates_->setOn(previous, false);
            });
        content_->addView(checkForUpdates_);
    }

    void applyValues() override {
        const AppSettingsData& values = settings_->get();
        language_->setSelection(languageIndex(values.language), true);
        theme_->setSelection(themeModeIndex(values.themeMode), true);
        burnInIdle_->setSelection(burnInIdleIndex(values.burnInIdleSec), true);
        burnInShowClock_->setOn(values.burnInShowClock, false);
        soundEffects_->setOn(values.soundEffectsEnabled, false);
        checkForUpdates_->setOn(values.checkForUpdatesOnLaunch, false);
    }

private:
    // Settings-selector row for a stored language value; falls back to the
    // "auto" row so a value from a newer build cannot leave the cell blank.
    static int languageIndex(const std::string& value) {
        for (size_t i = 0; i < std::size(kLanguageValues); ++i) {
            if (value == kLanguageValues[i])
                return static_cast<int>(i);
        }
        return 0;
    }

    static int themeModeIndex(const std::string& value) {
        for (size_t i = 0; i < std::size(kThemeModeValues); ++i) {
            if (value == kThemeModeValues[i])
                return static_cast<int>(i);
        }
        return 0;
    }

    static int burnInIdleIndex(uint32_t seconds) {
        for (size_t i = 0; i < std::size(kBurnInIdleSecValues); ++i) {
            if (seconds == kBurnInIdleSecValues[i])
                return static_cast<int>(i);
        }
        return 0;
    }

    AppSettings* settings_;
    WebServer* webServer_;
    brls::SelectorCell* language_ = nullptr;
    brls::SelectorCell* theme_ = nullptr;
    brls::SelectorCell* burnInIdle_ = nullptr;
    brls::BooleanCell* burnInShowClock_ = nullptr;
    brls::BooleanCell* checkForUpdates_ = nullptr;
    brls::BooleanCell* soundEffects_ = nullptr;
};

// --- Downloads: queue behaviour + install target ---------------------------

class DownloadsPanel : public SettingsPanel {
public:
    DownloadsPanel(AppSettings* settings, DownloadManager* manager,
                   WebServer* webServer)
        : settings_(settings), manager_(manager), webServer_(webServer) {
        addSection(content_, tr("pipensx/settings/section_downloads"));
        streamSelection_ = new brls::SelectorCell();
        streamSelection_->init(tr("pipensx/settings/stream_selection"),
            {tr("pipensx/settings/stream_all"),
             tr("pipensx/settings/stream_packages")},
            settings_->get().streamSelection == StreamSelection::PackagesOnly
                ? 1 : 0,
            [this](int selected) {
                AppSettingsData values = settings_->get();
                StreamSelection previous = values.streamSelection;
                values.streamSelection = selected == 1
                    ? StreamSelection::PackagesOnly
                    : StreamSelection::AllFiles;
                if (!persistSettings(settings_, values, "stream_selection", webServer_)) {
                    streamSelection_->setSelection(
                        previous == StreamSelection::PackagesOnly ? 1 : 0,
                        true);
                    return;
                }
                if (webServer_)
                    webServer_->setStreamSelection(values.streamSelection);
            });
        content_->addView(streamSelection_);

        installLocation_ = new brls::SelectorCell();
        installLocation_->init(tr("pipensx/settings/install_location"),
            {tr("pipensx/settings/install_sd"),
             tr("pipensx/settings/install_nand")},
            settings_->get().installLocation == InstallLocation::SystemMemory
                ? 1 : 0,
            [this](int selected) {
                AppSettingsData values = settings_->get();
                InstallLocation previous = values.installLocation;
                values.installLocation = selected == 1
                    ? InstallLocation::SystemMemory
                    : InstallLocation::SdCard;
                if (!persistSettings(settings_, values, "install_location", webServer_)) {
                    installLocation_->setSelection(
                        previous == InstallLocation::SystemMemory ? 1 : 0,
                        true);
                    return;
                }
                if (manager_)
                    manager_->setInstallTarget(
                        installTargetFor(values.installLocation));
            });
        content_->addView(installLocation_);

        maxActiveDownloads_ = new brls::SelectorCell();
        maxActiveDownloads_->init(tr("pipensx/settings/max_active_downloads"),
            {"1", "2", "3", "4"},
            static_cast<int>(settings_->get().maxActiveDownloads) - 1,
            [this](int selected) {
                AppSettingsData values = settings_->get();
                uint32_t previous = values.maxActiveDownloads;
                values.maxActiveDownloads =
                    pipensx::clampMaxActiveDownloads(
                        static_cast<uint64_t>(selected) + 1);
                if (!persistSettings(settings_, values,
                                     "max_active_downloads", webServer_)) {
                    maxActiveDownloads_->setSelection(
                        static_cast<int>(previous) - 1, true);
                    return;
                }
                if (manager_)
                    manager_->setMaxActiveDownloads(
                        values.maxActiveDownloads);
            });
        content_->addView(maxActiveDownloads_);

        showCompleted_ = new brls::BooleanCell();
        showCompleted_->init(tr("pipensx/settings/show_completed"),
            settings_->get().showCompletedDownloads,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.showCompletedDownloads;
                values.showCompletedDownloads = enabled;
                if (!persistSettings(settings_, values, "show_completed", webServer_))
                    showCompleted_->setOn(previous, false);
            });
        content_->addView(showCompleted_);
    }

    void applyValues() override {
        const AppSettingsData& values = settings_->get();
        streamSelection_->setSelection(
            values.streamSelection == StreamSelection::PackagesOnly ? 1 : 0,
            true);
        installLocation_->setSelection(
            values.installLocation == InstallLocation::SystemMemory ? 1 : 0,
            true);
        maxActiveDownloads_->setSelection(
            static_cast<int>(values.maxActiveDownloads) - 1, true);
        showCompleted_->setOn(values.showCompletedDownloads, false);
        if (manager_)
            manager_->setInstallTarget(
                installTargetFor(values.installLocation));
    }

private:
    AppSettings* settings_;
    DownloadManager* manager_;
    WebServer* webServer_;
    brls::SelectorCell* streamSelection_ = nullptr;
    brls::SelectorCell* installLocation_ = nullptr;
    brls::SelectorCell* maxActiveDownloads_ = nullptr;
    brls::BooleanCell* showCompleted_ = nullptr;
};

// --- Source: the debrid/torrenting fetch method ----------------------------

class SourcePanel : public SettingsPanel {
public:
    SourcePanel(AppSettings* settings, DownloadManager* manager,
                WebServer* webServer)
        : settings_(settings), manager_(manager), webServer_(webServer) {
        addSection(content_, tr("pipensx/settings/section_debrid"));
        torrenting_ = new brls::BooleanCell();
        torrenting_->init(tr("pipensx/settings/torrenting"),
            settings_->get().torrentingEnabled,
            [this](bool enabled) { setTorrenting(enabled); });
        content_->addView(torrenting_);

        debridProvider_ = new brls::SelectorCell();
        debridProvider_->init(tr("pipensx/settings/debrid_provider"),
            {"TorBox", "TorrServer", "Real-Debrid"},
            debridProviderIndex(settings_->get().debridProvider),
            [this](int selected) {
                AppSettingsData values = settings_->get();
                const DebridProviderKind previous = values.debridProvider;
                values.debridProvider = debridProviderForIndex(selected);
                if (!persistSettings(settings_, values, "debrid_provider", webServer_))
                    debridProvider_->setSelection(
                        debridProviderIndex(previous), true);
                refreshDebridLinkDetail();
            });
        content_->addView(debridProvider_);

        debridLink_ = actionCell(tr("pipensx/settings/debrid_link"), "",
            [this] {
                DebridLinkView::push(settings_, manager_,
                                     settings_->get().debridProvider);
            });
        // Named so the golden runner can focus it: the debrid section sits
        // far below the fold, and scrolling to it by counting d-pad presses
        // would break every time a row above it is added.
        debridLink_->setId("settings-debrid-link");
        content_->addView(debridLink_);
        refreshDebridLinkDetail();
    }

    void applyValues() override {
        const AppSettingsData& values = settings_->get();
        torrenting_->setOn(values.torrentingEnabled, false);
        debridProvider_->setSelection(
            debridProviderIndex(values.debridProvider), true);
        refreshDebridLinkDetail();
    }

private:
    static int debridProviderIndex(DebridProviderKind kind) {
        if (kind == DebridProviderKind::TorrServer)
            return 1;
        if (kind == DebridProviderKind::RealDebrid)
            return 2;
        return 0;
    }

    static DebridProviderKind debridProviderForIndex(int index) {
        if (index == 1)
            return DebridProviderKind::TorrServer;
        if (index == 2)
            return DebridProviderKind::RealDebrid;
        return DebridProviderKind::TorBox;
    }

    // Turning torrenting ON is the risky direction, so it goes through a
    // confirmation; turning it off needs none. The toggle is snapped back to
    // false first so the cell never shows "on" while the dialog is up.
    void setTorrenting(bool enabled) {
        const bool previous = settings_->get().torrentingEnabled;
        if (enabled && !previous) {
            torrenting_->setOn(false, false);
            auto* dialog = new brls::Dialog(
                tr("pipensx/settings/torrenting_warning"));
            dialog->addButton(tr("pipensx/settings/torrenting_enable"),
                [this] {
                    AppSettingsData values = settings_->get();
                    values.torrentingEnabled = true;
                    if (persistSettings(settings_, values, "torrenting", webServer_)) {
                        manager_->setTorrentingEnabled(true);
                        torrenting_->setOn(true, false);
                    }
                });
            dialog->addButton(tr("pipensx/common/cancel"), [] {});
            dialog->open();
            return;
        }
        AppSettingsData values = settings_->get();
        values.torrentingEnabled = enabled;
        if (!persistSettings(settings_, values, "torrenting", webServer_)) {
            torrenting_->setOn(previous, false);
            return;
        }
        manager_->setTorrentingEnabled(enabled);
    }

    void refreshDebridLinkDetail() {
        if (!debridLink_)
            return;
        const AppSettingsData& values = settings_->get();
        const char* provider = debridProviderName(values.debridProvider);
        // Spelled out rather than picking the key with a ternary: the i18n
        // checker only sees keys that appear as a literal first argument.
        debridLink_->setDetailText(
            activeDebridKey(values).empty()
                ? tr("pipensx/settings/debrid_not_linked", provider)
                : tr("pipensx/settings/debrid_linked", provider));
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    WebServer* webServer_;
    brls::BooleanCell* torrenting_ = nullptr;
    brls::SelectorCell* debridProvider_ = nullptr;
    brls::DetailCell* debridLink_ = nullptr;
};

// --- Network: web companion, proxy, live status ----------------------------

class NetworkPanel : public SettingsPanel {
public:
    NetworkPanel(AppSettings* settings, DownloadManager* manager,
                 WebServer* webServer, std::string ipAddress)
        : settings_(settings), manager_(manager), webServer_(webServer),
          ipAddress_(std::move(ipAddress)) {
        if (ipAddress_.empty())
            ipAddress_ = brls::Application::getPlatform()->getIpAddress();

        addSection(content_, tr("pipensx/settings/section_web"));
        webToggle_ = new brls::BooleanCell();
        webToggle_->init(tr("pipensx/settings/web_toggle"),
            settings_->get().webServerEnabled,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.webServerEnabled;
                values.webServerEnabled = enabled;
                if (!persistSettings(settings_, values, "web_server", webServer_)) {
                    webToggle_->setOn(previous, false);
                    return;
                }
                if (webServer_) {
                    if (enabled) {
                        if (!webServer_->start())
                            brls::Application::notify(
                                tr("pipensx/settings/web_start_failed"));
                    } else {
                        webServer_->stop();
                    }
                }
                updateWebCells();
            });
        content_->addView(webToggle_);
        webAddress_ = actionCell(tr("pipensx/settings/web_address"),
            "", [this] { showWebQr(); });
        content_->addView(webAddress_);
        webPin_ = actionCell(tr("pipensx/settings/web_pin"),
            "", [this] { editWebPin(); });
        content_->addView(webPin_);
        updateWebCells();

        addSection(content_, tr("pipensx/settings/section_network"));
        proxy_ = actionCell(tr("pipensx/settings/proxy"), "",
            [this] { editProxy(); });
        content_->addView(proxy_);
        refreshProxyDetail();
        addNote(content_, tr("pipensx/settings/proxy_note"));

        addSection(content_, tr("pipensx/diag/title"));
        internet_ = addHealthRow(tr("pipensx/diag/internet"));
        dht_ = addHealthRow(tr("pipensx/diag/dht"));
        peers_ = addHealthRow(tr("pipensx/diag/peers"));
        torbox_ = addHealthRow(tr("pipensx/diag/torbox"));
        torrserver_ = addHealthRow(tr("pipensx/diag/torrserver"));
        realdebrid_ = addHealthRow(tr("pipensx/diag/realdebrid"));
        proxyHealth_ = addHealthRow(tr("pipensx/diag/proxy"));
        catalog_ = addHealthRow(tr("pipensx/diag/catalog"));

        auto* recheck = new brls::Button();
        recheck->setText(tr("pipensx/settings/recheck"));
        recheck->setMarginTop(12);
        recheck->registerClickAction([this](brls::View*) {
            refreshHealth();
            return true;
        });
        content_->addView(recheck);
    }

    void onShown() override {
        // The console may have joined/left Wi-Fi since the last visit, and
        // the health rows are only refreshed on demand — never by a timer.
        refreshHealth();
    }

    void updateWebCells() {
        if (webAddress_)
            webAddress_->setDetailText(webAddressText());
        if (webPin_)
            webPin_->setDetailText(
                settings_->get().webServerPin.empty() ? "——" : "••••");
    }

    void applyValues() override {
        const AppSettingsData& values = settings_->get();
        webToggle_->setOn(values.webServerEnabled, false);
        updateWebCells();
        // A reset clears the proxy, so the environment has to follow it.
        pipensx::applyProxySetting(values.proxyUrl);
        refreshProxyDetail();
        refreshHealth();
    }

private:
    std::string webAddressText() const {
        if (!settings_->get().webServerEnabled)
            return tr("pipensx/settings/web_disabled");
        std::string url = webCompanionUrl(webServer_, true);
        return url.empty() ? tr("pipensx/settings/web_address_none") : url;
    }

    void showWebQr() {
        const std::string url = webAddressText();
        if (url.rfind("http://", 0) != 0) {
            brls::Application::notify(url);
            return;
        }
        showWebQrDialog(url, settings_->get().webServerPin);
    }

    void editWebPin() {
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                if (!pipensx::isValidWebPin(text)) {
                    brls::Application::notify(
                        tr("pipensx/settings/web_pin_invalid"));
                    return;
                }
                AppSettingsData values = settings_->get();
                values.webServerPin = text;
                if (!persistSettings(settings_, values, "web_pin", webServer_))
                    return;
                if (webServer_)
                    webServer_->setPin(settings_->get().webServerPin);
                updateWebCells();
            },
            tr("pipensx/settings/web_pin"),
            tr("pipensx/settings/web_pin_detail"), 8,
            settings_->get().webServerPin, brls::KEYBOARD_DISABLE_NONE);
    }

    void refreshProxyDetail() {
        const std::string& url = settings_->get().proxyUrl;
        proxy_->setDetailText(url.empty()
            ? tr("pipensx/settings/proxy_direct") : url);
    }

    void editProxy() {
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                if (!pipensx::isValidProxyUrl(text)) {
                    brls::Application::notify(
                        tr("pipensx/settings/proxy_invalid"));
                    return;
                }
                AppSettingsData values = settings_->get();
                values.proxyUrl = text;
                if (!persistSettings(settings_, values, "proxy_url", webServer_))
                    return;
                // Takes effect on the next request, not the next launch.
                pipensx::applyProxySetting(text);
                refreshProxyDetail();
            },
            tr("pipensx/settings/proxy"),
            tr("pipensx/settings/proxy_detail"), 128,
            settings_->get().proxyUrl, brls::KEYBOARD_DISABLE_NONE);
    }

    // Health rows: built once, values replaced in place (the old Network
    // Health screen minus its 1-second timer — refresh is on demand).
    brls::Label* addHealthRow(const std::string& label) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setFocusable(false);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setMarginBottom(8);

        auto* name = new brls::Label();
        name->setSingleLine(true);
        name->setAutoAnimate(false);
        name->setGrow(1);
        name->setFontSize(18);
        name->setTextColor(theme::textSecondary());
        name->setText(label);
        row->addView(name);

        auto* value = new brls::Label();
        value->setSingleLine(true);
        value->setAutoAnimate(false);
        value->setFontSize(18);
        value->setTextColor(theme::textPrimary());
        row->addView(value);
        content_->addView(row);
        return value;
    }

    void setHealthValue(brls::Label* label, const std::string& text,
                        NVGcolor color) {
        setTextIfChanged(label, text);
        label->setTextColor(color);
    }

    static std::string catalogAge(uint64_t wallSec) {
        if (wallSec == 0)
            return tr("pipensx/diag/never_refreshed");
        int64_t age = now_sec() - static_cast<int64_t>(wallSec);
        if (age < 0) age = 0;
        if (age < 60)
            return tr("pipensx/diag/just_now");
        if (age < 3600)
            return tr("pipensx/diag/updated_m", age / 60);
        if (age < 86400)
            return tr("pipensx/diag/updated_h", age / 3600);
        return tr("pipensx/diag/updated_d", age / 86400);
    }

    void refreshHealth() {
        const bool online = !ipAddress_.empty() && ipAddress_ != "-";
        setHealthValue(internet_,
                       online ? tr("pipensx/diag/connected")
                              : tr("pipensx/diag/offline"),
                       online ? theme::success() : theme::error());

        int dhtGood = 0;
        int dhtDubious = 0;
        const bool dhtOn = dht_shared_running();
        if (dhtOn)
            dht_shared_nodes(&dhtGood, &dhtDubious);
        if (!dhtOn) {
            setHealthValue(dht_, tr("pipensx/diag/dht_off"),
                           theme::textSecondary());
        } else if (dhtGood > 0) {
            setHealthValue(dht_,
                           tr("pipensx/diag/dht_nodes", dhtGood, dhtDubious),
                           theme::success());
        } else if (dhtDubious > 0) {
            setHealthValue(dht_,
                           tr("pipensx/diag/dht_nodes", dhtGood, dhtDubious),
                           theme::warning());
        } else {
            setHealthValue(dht_, tr("pipensx/diag/dht_bootstrapping"),
                           theme::warning());
        }

        uint32_t peers = 0;
        if (manager_) {
            for (const DownloadTask& task : manager_->snapshot())
                if (task.status == DownloadStatus::Downloading)
                    peers += task.peers;
        }
        setHealthValue(peers_, tr("pipensx/diag/peers_n", peers),
                       peers > 0 ? theme::success() : theme::textSecondary());

        const AppSettingsData values = settings_->get();
        setHealthValue(torbox_,
                       values.torboxApiKey.empty() ? tr("pipensx/diag/not_linked")
                                                   : tr("pipensx/diag/linked"),
                       values.torboxApiKey.empty() ? theme::textSecondary()
                                                   : theme::success());
        setHealthValue(torrserver_,
                       values.torrserverUrl.empty()
                           ? tr("pipensx/diag/not_configured")
                           : values.torrserverUrl,
                       values.torrserverUrl.empty() ? theme::textSecondary()
                                                    : theme::textPrimary());
        setHealthValue(realdebrid_,
                       values.realdebridApiKey.empty()
                           ? tr("pipensx/diag/not_linked")
                           : tr("pipensx/diag/linked"),
                       values.realdebridApiKey.empty() ? theme::textSecondary()
                                                       : theme::success());
        setHealthValue(proxyHealth_,
                       values.proxyUrl.empty() ? tr("pipensx/diag/disabled")
                                               : tr("pipensx/diag/enabled"),
                       values.proxyUrl.empty() ? theme::textSecondary()
                                               : theme::accent());
        setHealthValue(catalog_, catalogAge(values.lastCatalogRefreshWallSec),
                       theme::textSecondary());
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    WebServer* webServer_;
    std::string ipAddress_;
    brls::BooleanCell* webToggle_ = nullptr;
    brls::DetailCell* webAddress_ = nullptr;
    brls::DetailCell* webPin_ = nullptr;
    brls::DetailCell* proxy_ = nullptr;
    brls::Label* internet_ = nullptr;
    brls::Label* dht_ = nullptr;
    brls::Label* peers_ = nullptr;
    brls::Label* torbox_ = nullptr;
    brls::Label* torrserver_ = nullptr;
    brls::Label* realdebrid_ = nullptr;
    brls::Label* proxyHealth_ = nullptr;
    brls::Label* catalog_ = nullptr;
};

// --- Catalog: refresh behaviour, source URL, manual refresh ----------------

class CatalogPanel : public SettingsPanel {
public:
    CatalogPanel(AppSettings* settings, CatalogService* catalog,
                 GameMetadataService* metadata,
                 std::shared_ptr<std::atomic<bool>> alive, WebServer* webServer,
                 std::function<void()> onMetadataRefreshed)
        : settings_(settings), catalog_(catalog), metadata_(metadata),
          alive_(std::move(alive)), webServer_(webServer),
          onMetadataRefreshed_(std::move(onMetadataRefreshed)) {
        addSection(content_, tr("pipensx/settings/section_catalog"));
        refreshCatalog_ = new brls::BooleanCell();
        refreshCatalog_->init(tr("pipensx/settings/auto_refresh"),
            settings_->get().refreshCatalogOnLaunch,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.refreshCatalogOnLaunch;
                values.refreshCatalogOnLaunch = enabled;
                if (!persistSettings(settings_, values, "catalog_refresh", webServer_))
                    refreshCatalog_->setOn(previous, false);
            });
        content_->addView(refreshCatalog_);

        catalogSource_ = actionCell(tr("pipensx/settings/catalog_source"), "",
            [this] { editCatalogSource(); });
        content_->addView(catalogSource_);
        refreshCatalogSourceDetail();

        content_->addView(actionCell(tr("pipensx/settings/update_now"),
            tr("pipensx/settings/update_now_detail"),
            [this] { updateAllNow(); }));
    }

    void applyValues() override {
        const AppSettingsData& values = settings_->get();
        refreshCatalog_->setOn(values.refreshCatalogOnLaunch, false);
        refreshCatalogSourceDetail();
    }

private:
    // The manual "Update now" action chains catalog then artwork.
    void updateAllNow() {
        if (refreshInFlight_)
            return;
        refreshCatalogNow([this] { refreshMetadataNow(); });
    }

    void recordRefreshTime(bool catalog, bool metadata) {
        AppSettingsData values = settings_->get();
        const uint64_t now = now_ms();
        if (catalog) {
            values.lastCatalogRefreshMs = now;
            values.lastCatalogRefreshWallSec =
                static_cast<uint64_t>(time(nullptr));
        }
        if (metadata)
            values.lastMetadataRefreshMs = now;
        persistSettings(settings_, values,
                        catalog ? "catalog_refresh_time"
                                : "metadata_refresh_time",
                        webServer_);
    }

    void refreshCatalogNow(std::function<void()> onDone = {}) {
        if (refreshInFlight_)
            return;
        refreshInFlight_ = true;
        brls::Application::notify(tr("pipensx/catalog/updating_catalog"));
        auto alive = alive_;
        CatalogService* catalog = catalog_;
        const std::string catalogSourceUrl =
            effectiveCatalogSourceUrl(settings_->get().catalogSourceUrl);
        brls::async([this, alive, catalog, catalogSourceUrl,
                     onDone = std::move(onDone)]() mutable {
            std::vector<CatalogEntry> entries;
            std::string error;
            bool ok = catalog->fetchLatest(entries, error, catalogSourceUrl);
            brls::sync([this, alive, ok, entries = std::move(entries),
                        error = std::move(error), catalogSourceUrl,
                        onDone = std::move(onDone)]() mutable {
                if (!alive->load())
                    return;
                refreshInFlight_ = false;
                if (!ok) {
                    diagnostic_error("catalog", "settings_refresh", "error=%s",
                                     error.c_str());
                    brls::Application::notify(error);
                    return;
                }
                catalog_->adopt(std::move(entries), catalogSourceUrl);
                recordRefreshTime(true, false);
                brls::Application::notify(
                    tr("pipensx/catalog/updated_catalog",
                       catalog_->entries().size()));
                if (onDone)
                    onDone();
            });
        });
    }

    void refreshMetadataNow(std::function<void()> onDone = {}) {
        if (refreshInFlight_ || !metadata_)
            return;
        refreshInFlight_ = true;
        brls::Application::notify(tr("pipensx/catalog/updating_artwork"));
        auto alive = alive_;
        GameMetadataService* metadata = metadata_;
        brls::async([this, alive, metadata, onDone = std::move(onDone)]()
                        mutable {
            MetadataSnapshot snapshot;
            std::string error;
            bool ok = metadata->fetchLatest(snapshot, error);
            brls::sync([this, alive, ok, snapshot = std::move(snapshot),
                        error = std::move(error),
                        onDone = std::move(onDone)]() mutable {
                if (!alive->load())
                    return;
                refreshInFlight_ = false;
                if (!ok) {
                    diagnostic_error("metadata", "settings_refresh",
                                     "error=%s", error.c_str());
                    brls::Application::notify(error);
                    return;
                }
                metadata_->adopt(std::move(snapshot));
                metadata_->dropMemoryImageCache();
                recordRefreshTime(false, true);
                brls::Application::notify(
                    tr("pipensx/catalog/updated_artwork", metadata_->size()));
                if (onMetadataRefreshed_)
                    onMetadataRefreshed_();
                if (onDone)
                    onDone();
            });
        });
    }

    void refreshCatalogSourceDetail() {
        const std::string& url = settings_->get().catalogSourceUrl;
        catalogSource_->setDetailText(
            url.empty() ? tr("pipensx/settings/catalog_source_default") : url);
    }

    void editCatalogSource() {
        brls::Application::getImeManager()->openForText(
            [this](std::string text) {
                if (!pipensx::isValidCatalogSourceUrl(text)) {
                    brls::Application::notify(
                        tr("pipensx/settings/catalog_source_invalid"));
                    return;
                }
                AppSettingsData values = settings_->get();
                values.catalogSourceUrl = text;
                if (!persistSettings(settings_, values, "catalog_source_url", webServer_))
                    return;
                refreshCatalogSourceDetail();
            },
            tr("pipensx/settings/catalog_source"),
            tr("pipensx/settings/catalog_source_detail"), 512,
            settings_->get().catalogSourceUrl, brls::KEYBOARD_DISABLE_NONE);
    }

    AppSettings* settings_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    std::shared_ptr<std::atomic<bool>> alive_;
    WebServer* webServer_;
    std::function<void()> onMetadataRefreshed_;
    brls::BooleanCell* refreshCatalog_ = nullptr;
    brls::DetailCell* catalogSource_ = nullptr;
    bool refreshInFlight_ = false;
};

// --- Storage: SD breakdown + cleanup (the former Storage screen) -----------

class StoragePanel : public SettingsPanel {
public:
    StoragePanel(DownloadManager* manager, GameMetadataService* metadata,
                 std::shared_ptr<std::atomic<bool>> alive)
        : manager_(manager), metadata_(metadata),
          alive_(std::move(alive)) {
        meter_ = new StorageMeter();
        meter_->setHeader(tr("pipensx/storage/title_sd_card"));
        content_->addView(meter_);

        addSection(content_, tr("pipensx/storage/breakdown"));
        breakdown_ = new brls::Box(brls::Axis::COLUMN);
        breakdown_->setMarginBottom(10);
        content_->addView(breakdown_);

        addSection(content_, tr("pipensx/storage/cleanup"));
        clearCompleted_ = actionCell(
            tr("pipensx/storage/clear_completed"), "", [this] {
                confirmClearCompleted();
            });
        content_->addView(clearCompleted_);
        clearImages_ = actionCell(
            tr("pipensx/storage/clear_images"), "", [this] {
                confirmClearImages();
            });
        content_->addView(clearImages_);
        clearTorrents_ = actionCell(
            tr("pipensx/storage/clear_torrents"), "", [this] {
                confirmClearTorrents();
            });
        content_->addView(clearTorrents_);
        clearTemporary_ = actionCell(
            tr("pipensx/storage/clear_temporary"), "", [this] {
                confirmClearTemporary();
            });
        content_->addView(clearTemporary_);

        canFree_ = new brls::Label();
        canFree_->setFontSize(17);
        canFree_->setTextColor(theme::success());
        canFree_->setMarginTop(14);
        canFree_->setVisibility(brls::Visibility::GONE);
        content_->addView(canFree_);
    }

    void onShown() override {
        refresh(/*wait=*/!didFirstRefresh_);
        didFirstRefresh_ = true;
    }

private:
    struct ScanPayload {
        StorageBreakdown snapshot;
        uint64_t completedBytes = 0;
        uint64_t orphanBytes = 0;
        bool hasFinished = false;
    };

    ScanPayload collectScan() const {
        ScanPayload payload;
        payload.snapshot = scanStorageBreakdown(manager_->rootPath());
        std::vector<DownloadTask> tasks = manager_->snapshot();
        std::vector<std::string> active;
        active.reserve(tasks.size());
        for (const DownloadTask& task : tasks) {
            active.push_back(task.id);
            if (task.status != DownloadStatus::Completed &&
                task.status != DownloadStatus::Installed)
                continue;
            payload.hasFinished = true;
            uint64_t size = 0;
            if (directorySize(task.dataPath, size))
                payload.completedBytes =
                    size > UINT64_MAX - payload.completedBytes
                        ? UINT64_MAX
                        : payload.completedBytes + size;
        }
        payload.orphanBytes =
            pipensx::orphanTorrentBytes(manager_->torrentRoot(), active);
        return payload;
    }

    void applyScan(const ScanPayload& payload) {
        snapshot_ = payload.snapshot;
        completedBytes_ = payload.completedBytes;
        orphanBytes_ = payload.orphanBytes;
        hasFinished_ = payload.hasFinished;

        if (meter_) {
            if (snapshot_.available)
                meter_->setStorage(snapshot_.totalBytes, snapshot_.freeBytes);
            else
                meter_->setUnavailable();
        }

        rebuildRows();

        if (clearCompleted_)
            clearCompleted_->setDetailText(completedDownloadsDetail());
        if (clearImages_)
            clearImages_->setDetailText(
                recoverableDetail(snapshot_.imageCacheBytes));
        if (clearTorrents_)
            clearTorrents_->setDetailText(recoverableDetail(orphanBytes_));
        if (clearTemporary_)
            clearTemporary_->setDetailText(
                recoverableDetail(snapshot_.temporaryBytes));

        if (canFree_) {
            uint64_t total = orphanBytes_;
            const uint64_t parts[3] = {
                hasFinished_ ? completedBytes_ : 0,
                snapshot_.imageCacheBytes, snapshot_.temporaryBytes};
            for (uint64_t part : parts)
                total = part > UINT64_MAX - total ? UINT64_MAX
                                                  : total + part;
            if (total > 0) {
                canFree_->setText(
                    tr("pipensx/storage/can_free", formatBytes(total)));
                canFree_->setVisibility(brls::Visibility::VISIBLE);
            } else {
                canFree_->setVisibility(brls::Visibility::GONE);
            }
        }
    }

    // First paint is synchronous so golden (and the opening frame) see real
    // numbers. Later refreshes walk the tree off the UI thread.
    void refresh(bool wait = false) {
        if (!manager_)
            return;
        if (wait) {
            applyScan(collectScan());
            return;
        }
        if (refreshInFlight_) {
            refreshPending_ = true;
            return;
        }
        refreshInFlight_ = true;
        auto alive = alive_;
        brls::async([this, alive] {
            ScanPayload payload = collectScan();
            brls::sync([this, alive, payload = std::move(payload)] {
                if (!alive->load())
                    return;
                refreshInFlight_ = false;
                applyScan(payload);
                if (refreshPending_) {
                    refreshPending_ = false;
                    refresh(false);
                }
            });
        });
    }

    void rebuildRows() {
        if (!breakdown_)
            return;
        breakdown_->clearViews();
        addRow(tr("pipensx/storage/cat_downloads"), snapshot_.downloadsBytes);
        addRow(tr("pipensx/storage/cat_torrents"), snapshot_.torrentBytes);
        addRow(tr("pipensx/storage/cat_images"), snapshot_.imageCacheBytes);
        addRow(tr("pipensx/storage/cat_metadata"),
               snapshot_.metadataCacheBytes);
        addRow(tr("pipensx/storage/cat_temporary"),
               snapshot_.temporaryBytes);
        addRow(tr("pipensx/storage/cat_icons"), snapshot_.iconsBytes);
        addRow(tr("pipensx/storage/cat_other"), snapshot_.otherBytes);
    }

    void addRow(const std::string& label, uint64_t bytes) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setFocusable(false);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setMarginBottom(4);

        auto* name = new brls::Label();
        name->setSingleLine(true);
        name->setGrow(1);
        name->setFontSize(17);
        name->setTextColor(theme::textSecondary());
        name->setText(label);
        row->addView(name);

        auto* value = new brls::Label();
        value->setSingleLine(true);
        value->setFontSize(17);
        value->setTextColor(theme::textPrimary());
        value->setText(formatBytes(bytes));
        row->addView(value);

        breakdown_->addView(row);
    }

    std::string recoverableDetail(uint64_t bytes) {
        return bytes == 0 ? tr("pipensx/storage/nothing_to_recover")
                          : tr("pipensx/storage/recoverable",
                               formatBytes(bytes));
    }

    std::string completedDownloadsDetail() {
        if (!hasFinished_)
            return tr("pipensx/storage/nothing_to_recover");
        return tr("pipensx/storage/recoverable",
                  formatBytes(completedBytes_));
    }

    void confirmClearCompleted() {
        if (!hasFinished_) {
            brls::Application::notify(
                tr("pipensx/storage/nothing_to_recover"));
            return;
        }
        confirm(tr("pipensx/storage/clear_completed_confirm",
                   formatBytes(completedBytes_)),
                [this] { clearCompleted(); });
    }

    void confirmClearImages() {
        confirmAction(snapshot_.imageCacheBytes, [this] { clearImages(); });
    }

    void confirmClearTorrents() {
        confirmAction(orphanBytes_, [this] { clearTorrents(); });
    }

    void confirmClearTemporary() {
        if (manager_->hasActiveTransfer()) {
            brls::Application::notify(tr("pipensx/storage/busy_transfer"));
            return;
        }
        confirmAction(snapshot_.temporaryBytes, [this] { clearTemporary(); });
    }

    void confirmAction(uint64_t bytes, const std::function<void()>& action) {
        if (bytes == 0) {
            brls::Application::notify(
                tr("pipensx/storage/nothing_to_recover"));
            return;
        }
        confirm(tr("pipensx/storage/confirm_recover", formatBytes(bytes)),
                action);
    }

    void confirm(const std::string& message,
                 const std::function<void()>& action) {
        auto* dialog = new brls::Dialog(message);
        dialog->addButton(tr("pipensx/common/clear"), [action] { action(); });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    void clearCompleted() {
        std::string error;
        if (!manager_->clearCompleted(true, error)) {
            diagnostic_error("storage", "completed", "error=%s",
                             error.c_str());
            if (!error.empty())
                brls::Application::notify(error);
            return;
        }
        brls::Application::notify(tr("pipensx/storage/cleared"));
        refresh();
    }

    void clearImages() {
        std::string error;
        if (!metadata_->clearImageCache(error)) {
            diagnostic_error("storage", "images", "error=%s", error.c_str());
            brls::Application::notify(error);
            return;
        }
        brls::Application::notify(tr("pipensx/storage/cleared"));
        refresh();
    }

    void clearTorrents() {
        std::vector<DownloadTask> tasks = manager_->snapshot();
        std::vector<std::string> active;
        active.reserve(tasks.size());
        for (const DownloadTask& task : tasks)
            active.push_back(task.id);
        std::string error;
        uint64_t recovered = 0;
        if (!clearOrphanTorrents(manager_->torrentRoot(), active, error,
                                 recovered)) {
            diagnostic_error("storage", "torrents", "error=%s", error.c_str());
            brls::Application::notify(error);
            return;
        }
        brls::Application::notify(tr("pipensx/storage/cleared"));
        refresh();
    }

    void clearTemporary() {
        if (manager_->hasActiveTransfer()) {
            brls::Application::notify(tr("pipensx/storage/busy_transfer"));
            return;
        }
        std::string error;
        uint64_t recovered = 0;
        if (!clearTemporaryFiles(manager_->rootPath(), error, recovered)) {
            diagnostic_error("storage", "temporary", "error=%s",
                             error.c_str());
            brls::Application::notify(error);
            return;
        }
        brls::Application::notify(tr("pipensx/storage/cleared"));
        refresh();
    }

    DownloadManager* manager_;
    GameMetadataService* metadata_;
    std::shared_ptr<std::atomic<bool>> alive_;
    StorageMeter* meter_ = nullptr;
    brls::Box* breakdown_ = nullptr;
    brls::DetailCell* clearCompleted_ = nullptr;
    brls::DetailCell* clearImages_ = nullptr;
    brls::DetailCell* clearTorrents_ = nullptr;
    brls::DetailCell* clearTemporary_ = nullptr;
    brls::Label* canFree_ = nullptr;
    StorageBreakdown snapshot_;
    uint64_t completedBytes_ = 0;
    uint64_t orphanBytes_ = 0;
    bool hasFinished_ = false;
    bool didFirstRefresh_ = false;
    bool refreshInFlight_ = false;
    bool refreshPending_ = false;
};

// --- System: update check, diagnostics, factory reset ----------------------

class SystemPanel : public SettingsPanel {
public:
    SystemPanel(AppSettings* settings, DownloadManager* manager,
                CatalogService* catalog, GameMetadataService* metadata,
                InstalledTitleService* installed, UpdateService* updater,
                std::shared_ptr<std::atomic<bool>> alive,
                std::function<void()> onReset, WebServer* webServer)
        : settings_(settings), manager_(manager), catalog_(catalog),
          metadata_(metadata), installed_(installed), updater_(updater),
          alive_(std::move(alive)), onReset_(std::move(onReset)),
          webServer_(webServer) {
        addSection(content_, tr("pipensx/settings/section_updates"));
        updateAction_ = actionCell(tr("pipensx/settings/check_update_now"),
            tr("pipensx/settings/check_update_detail", PIPENSX_VERSION),
            [this] { checkForUpdateNow(); });
        if (updater_ && updater_->checkCompleted())
            markUpdateChecked();
        content_->addView(updateAction_);

        addSection(content_, tr("pipensx/settings/section_diagnostics"));
        addNote(content_, tr("pipensx/settings/diagnostics_note"));
        extendedTelemetry_ = new brls::BooleanCell();
        extendedTelemetry_->init(tr("pipensx/settings/extended_telemetry"),
            settings_->get().extendedTelemetry,
            [this](bool enabled) {
                AppSettingsData values = settings_->get();
                bool previous = values.extendedTelemetry;
                values.extendedTelemetry = enabled;
                if (!persistSettings(settings_, values,
                                     "extended_telemetry", webServer_)) {
                    extendedTelemetry_->setOn(previous, false);
                    return;
                }
                telemetry_set_enabled(enabled ? 1 : 0);
                brls::Application::notify(enabled
                    ? tr("pipensx/settings/telemetry_on")
                    : tr("pipensx/settings/telemetry_off"));
            });
        content_->addView(extendedTelemetry_);

        content_->addView(actionCell(tr("pipensx/settings/capture_snapshot"),
            tr("pipensx/settings/capture_snapshot_detail"),
            [this] { captureSnapshot(); }));
        content_->addView(actionCell(tr("pipensx/settings/clear_log"),
            tr("pipensx/settings/clear_log_detail"),
            [this] { confirmClearLog(); }));

        auto* path = new brls::Label();
        path->setText(tr("pipensx/settings/log_path", LogPath));
        path->setFontSize(15);
        path->setTextColor(theme::textTertiary());
        path->setMarginTop(18);
        content_->addView(path);

        addSection(content_, tr("pipensx/settings/section_reset"));
        content_->addView(actionCell(tr("pipensx/settings/reset"),
            tr("pipensx/settings/reset_detail"),
            [this] { confirmReset(); }));
    }

    void applyValues() override {
        const AppSettingsData& values = settings_->get();
        extendedTelemetry_->setOn(values.extendedTelemetry, false);
        telemetry_set_enabled(values.extendedTelemetry ? 1 : 0);
    }

private:
    void checkForUpdateNow() {
        if (updateInFlight_ || !updater_)
            return;
        updateInFlight_ = true;
        updateAction_->setDetailText(tr("pipensx/settings/checking"));
        auto alive = alive_;
        UpdateService* updater = updater_;
        updater->checkAsync([this, alive](UpdateCheckResult result) {
            brls::sync([this, alive, result = std::move(result)]() mutable {
                if (!alive->load())
                    return;
                updateInFlight_ = false;
                markUpdateChecked();
                if (!result.ok) {
                    updateAction_->setDetailText(
                        tr("pipensx/settings/check_failed"));
                    diagnostic_error("update", "check", "error=%s",
                                     result.error.c_str());
                    brls::Application::notify(result.error);
                    return;
                }
                if (!result.updateAvailable) {
                    updateAction_->setDetailText(
                        tr("pipensx/settings/up_to_date"));
                    brls::Application::notify(
                        tr("pipensx/settings/up_to_date_notify"));
                    return;
                }
                updateAction_->setDetailText(
                    tr("pipensx/settings/version_detail",
                       result.release.version));
                confirmInstallUpdate(std::move(result.release));
            });
        });
    }

    void confirmInstallUpdate(ReleaseInfo release) {
        auto* dialog = new brls::Dialog(
            tr("pipensx/settings/update_available", release.version));
        dialog->addButton(tr("pipensx/settings/install_and_restart"),
                          [this, release = std::move(release)] {
            installUpdate(release);
        });
        dialog->addButton(tr("pipensx/common/later"), [] {});
        dialog->open();
    }

    void markUpdateChecked() {
        updateAction_->setTextColor(theme::accent());
        updateAction_->setDetailTextColor(theme::accent());
    }

    void installUpdate(const ReleaseInfo& release) {
        if (updateInFlight_ || !updater_)
            return;
        updateInFlight_ = true;
        updateAction_->setDetailText(tr("pipensx/settings/downloading"));
        auto alive = alive_;
        UpdateService* updater = updater_;
        auto lastPercent = std::make_shared<std::atomic<int>>(-1);
        updater->onInstallProgress(
            [this, alive, lastPercent](uint64_t received, uint64_t total) {
                const int percent =
                    static_cast<int>((received * 100) / total);
                if (lastPercent->exchange(percent) == percent)
                    return;
                brls::sync([this, alive, percent] {
                    if (!alive->load())
                        return;
                    updateAction_->setDetailText(
                        tr("pipensx/settings/downloading_percent", percent));
                });
            });
        updater->installAsync(release, [this, alive](bool installed,
                                                       std::string error) {
            brls::sync([this, alive, installed, error = std::move(error)] {
                if (!alive->load())
                    return;
                updateInFlight_ = false;
                if (!installed) {
                    updateAction_->setDetailText(
                        tr("pipensx/settings/install_failed"));
                    diagnostic_error("update", "install", "error=%s",
                                     error.c_str());
                    brls::Application::notify(error);
                    return;
                }
                updateAction_->setDetailText(
                    tr("pipensx/settings/restart_required"));
#ifdef __SWITCH__
                if (!envHasNextLoad()) {
                    brls::Application::notify(
                        tr("pipensx/settings/update_no_restart"));
                    return;
                }
                const std::string helper = updater_->helperPath();
                const std::string arguments =
                    "\"" + helper + "\" --finish-update";
                const Result result = envSetNextLoad(helper.c_str(),
                                                     arguments.c_str());
                if (R_FAILED(result)) {
                    diagnostic_error("update", "restart", "result=0x%08x",
                                     result);
                    brls::Application::notify(
                        tr("pipensx/settings/update_restart_failed"));
                    return;
                }
#endif
                // The helper swaps the NRO after we exit, then drops to HOME
                // instead of relaunching (an in-session relaunch of the full
                // app crashes). Gate the quit behind an acknowledged dialog so
                // the close reads as intentional rather than a crash.
                auto* dialog = new brls::Dialog(
                    tr("pipensx/settings/update_close_body"));
                dialog->setCancelable(false);
                dialog->addButton(tr("pipensx/settings/update_close_button"),
                                  [] { brls::Application::quit(); });
                dialog->open();
            });
        });
    }

    void captureSnapshot() {
        writeSystemSnapshot(manager_, catalog_, metadata_, installed_,
                            "manual");
        brls::Application::notify(tr("pipensx/settings/snapshot_written"));
    }

    void confirmClearLog() {
        auto* dialog = new brls::Dialog(
            tr("pipensx/settings/clear_log_question"));
        dialog->addButton(tr("pipensx/settings/clear_log"), [] {
            if (!clearApplicationLog())
                brls::Application::notify(
                    tr("pipensx/settings/clear_log_failed"));
            else
                brls::Application::notify(
                    tr("pipensx/settings/clear_log_done"));
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    // Factory reset. Every panel re-syncs through the hub's onReset callback
    // (the old Advanced page's applyOwnValues + SettingsView::applyValues).
    void confirmReset() {
        auto* dialog = new brls::Dialog(
            tr("pipensx/settings/reset_question"));
        dialog->addButton(tr("pipensx/settings/reset_action"), [this] {
            std::string error;
            if (!settings_->reset(error)) {
                diagnostic_error("settings", "reset", "error=%s",
                                 error.c_str());
                brls::Application::notify(error);
                return;
            }
            if (onReset_)
                onReset_();
            brls::Application::notify(tr("pipensx/settings/reset_done"));
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    AppSettings* settings_;
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    InstalledTitleService* installed_;
    UpdateService* updater_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::function<void()> onReset_;
    WebServer* webServer_;
    brls::BooleanCell* extendedTelemetry_ = nullptr;
    brls::DetailCell* updateAction_ = nullptr;
    bool updateInFlight_ = false;
};

}  // namespace pipensx::ui
