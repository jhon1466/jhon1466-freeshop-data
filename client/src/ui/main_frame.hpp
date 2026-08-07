#pragma once

// UI: collapsible navigation frame.
//
// Wraps brls::TabFrame to (1) draw an icon next to every sidebar label and
// (2) fold the sidebar down to a slim icon rail while the user is browsing a
// tab's content, so the catalogue grid gets almost the whole screen. The rail
// re-expands the moment focus returns to the menu (B / left). No extra button:
// the fold is driven purely by focus.
//
// Everything lives here, in-tree — the vendored borealis submodule is left
// untouched. We reach the private sidebar bits we need through the public
// Sidebar::getItem() / Box::getChildren() surface.

#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <borealis.hpp>
#include <borealis/views/widgets/battery.hpp>
#include <borealis/views/widgets/wireless.hpp>

#include "app/download_manager.hpp"
#include "app/install_space.hpp"
#include "app/web_server.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/common/web_qr.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

enum class NavIconType {
    Catalog, Downloads, Installed, Explorer, Saves, Settings, Help, About
};

// Shrinks the stock sidebar so the icon rail + expanded menu both look right.
// Style metrics back a shared global table, so this must run once after
// Application::init() and BEFORE the first TabFrame/Sidebar is constructed
// (both read these at inflate time).
inline void installSidebarStyle() {
    brls::Style style = brls::Application::getStyle();
    style.addMetric("brls/tab_frame/sidebar_width", 248.0f);  // was 410
    style.addMetric("brls/sidebar/padding_left", 22.0f);      // was 80
    style.addMetric("brls/sidebar/padding_right", 16.0f);     // was 40
    style.addMetric("brls/sidebar/padding_top", 28.0f);
}

// Decorative glyph shown to the left of a sidebar label. Non-focusable; its
// colour tracks the owning item's active state so it lights up with the accent
// when its tab is selected — matching the label the sidebar already recolours.
class NavIcon : public brls::View {
public:
    NavIcon(NavIconType type, brls::SidebarItem* owner)
        : type_(type), owner_(owner) {
        this->setWidth(28.0f);
        this->setHeight(28.0f);
        this->setAlignSelf(brls::AlignSelf::CENTER);
        this->setFocusable(false);
        this->setMarginRight(12.0f);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        const NVGcolor c = (owner_ && owner_->isActive())
                               ? theme::accent()
                               : theme::textSecondary();
        nvgStrokeColor(vg, c);
        nvgFillColor(vg, c);
        nvgStrokeWidth(vg, 2.0f);
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);

        const float s = 24.0f;                    // glyph box side
        const float gx = x + (width - s) / 2.0f;  // glyph origin
        const float gy = y + (height - s) / 2.0f;
        switch (type_) {
            case NavIconType::Catalog:   drawCatalog(vg, gx, gy, s); break;
            case NavIconType::Downloads: drawDownloads(vg, gx, gy, s); break;
            case NavIconType::Installed: drawInstalled(vg, gx, gy, s); break;
            case NavIconType::Explorer:    drawExplorer(vg, gx, gy, s); break;
            case NavIconType::Saves:       drawSaves(vg, gx, gy, s); break;
            case NavIconType::Settings:    drawSettings(vg, gx, gy, s); break;
            case NavIconType::Help:        drawPulse(vg, gx, gy, s); break;
            case NavIconType::About:       drawAbout(vg, gx, gy, s); break;
        }
    }

private:
    // 2x2 grid of rounded squares.
    static void drawCatalog(NVGcontext* vg, float gx, float gy, float s) {
        const float cell = 9.0f;
        const float step = s - cell;  // 15 -> 6px gap
        for (int i = 0; i < 4; i++) {
            const float px = gx + (i % 2) * step;
            const float py = gy + (i / 2) * step;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, px, py, cell, cell, 2.0f);
            nvgStroke(vg);
        }
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

    // Folder: a body rectangle with a small tab notched into its top-left.
    static void drawExplorer(NVGcontext* vg, float gx, float gy, float s) {
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 1.0f, gy + 6.0f);
        nvgLineTo(vg, gx + 1.0f, gy + s - 3.0f);
        nvgLineTo(vg, gx + s - 1.0f, gy + s - 3.0f);
        nvgLineTo(vg, gx + s - 1.0f, gy + 9.0f);
        nvgLineTo(vg, gx + s / 2.0f, gy + 9.0f);
        nvgLineTo(vg, gx + s / 2.0f - 3.0f, gy + 6.0f);
        nvgLineTo(vg, gx + 1.0f, gy + 6.0f);
        nvgClosePath(vg);
        nvgStroke(vg);
    }

    // Floppy disk: rounded body, a notched top-right corner, and a shutter
    // line near the bottom - the universal "save" glyph.
    static void drawSaves(NVGcontext* vg, float gx, float gy, float s) {
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 1.0f, gy + 1.0f);
        nvgLineTo(vg, gx + s - 6.0f, gy + 1.0f);
        nvgLineTo(vg, gx + s - 1.0f, gy + 6.0f);
        nvgLineTo(vg, gx + s - 1.0f, gy + s - 1.0f);
        nvgLineTo(vg, gx + 1.0f, gy + s - 1.0f);
        nvgClosePath(vg);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 6.0f, gy + 1.0f);
        nvgLineTo(vg, gx + 6.0f, gy + 9.0f);
        nvgLineTo(vg, gx + s - 6.0f, gy + 9.0f);
        nvgLineTo(vg, gx + s - 6.0f, gy + 1.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 5.0f, gy + s - 8.0f);
        nvgLineTo(vg, gx + s - 5.0f, gy + s - 8.0f);
        nvgStroke(vg);
    }

    // Rounded square with a checkmark.
    static void drawInstalled(NVGcontext* vg, float gx, float gy, float s) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, gx + 1.0f, gy + 1.0f, s - 2.0f, s - 2.0f, 4.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 6.0f, gy + 12.0f);
        nvgLineTo(vg, gx + 10.0f, gy + 16.0f);
        nvgLineTo(vg, gx + 17.0f, gy + 8.0f);
        nvgStroke(vg);
    }

    // Three fader lines with offset knobs.
    static void drawSettings(NVGcontext* vg, float gx, float gy, float s) {
        const float ys[3] = {gy + 5.0f, gy + 12.0f, gy + 19.0f};
        const float knob[3] = {gx + 8.0f, gx + 16.0f, gx + 11.0f};
        for (int i = 0; i < 3; i++) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, gx + 2.0f, ys[i]);
            nvgLineTo(vg, gx + s - 2.0f, ys[i]);
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgCircle(vg, knob[i], ys[i], 2.6f);
            nvgFill(vg);
        }
    }

    // Info circle: dot over a stem.
    static void drawAbout(NVGcontext* vg, float gx, float gy, float s) {
        const float cx = gx + s / 2.0f;
        const float cy = gy + s / 2.0f;
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, s / 2.0f - 1.0f);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, gy + 7.0f, 1.3f);
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, cx, gy + 11.0f);
        nvgLineTo(vg, cx, gy + 17.0f);
        nvgStroke(vg);
    }

    // ECG pulse: a flat trace with one sharp spike — diagnostics.
    static void drawPulse(NVGcontext* vg, float gx, float gy, float s) {
        nvgBeginPath(vg);
        nvgMoveTo(vg, gx + 2.0f, gy + 15.0f);
        nvgLineTo(vg, gx + 9.0f, gy + 15.0f);
        nvgLineTo(vg, gx + 11.0f, gy + 7.0f);
        nvgLineTo(vg, gx + 13.0f, gy + 19.0f);
        nvgLineTo(vg, gx + 15.0f, gy + 15.0f);
        nvgLineTo(vg, gx + s - 2.0f, gy + 15.0f);
        nvgStroke(vg);
    }

    NavIconType type_;
    brls::SidebarItem* owner_;
};

// One line of web-companion status for the sidebar footer: a state dot
// (accent = serving, muted = off) and the reachable address. Non-focusable —
// the QR/action surface is the global Minus hint, this is just the readout.
class WebStatusRow : public brls::Box {
public:
    WebStatusRow() : brls::Box(brls::Axis::ROW) {
        setFocusable(false);
        setAlignItems(brls::AlignItems::CENTER);
        setMarginBottom(8);
        setClipsToBounds(true);
        dot_ = new Dot();
        addView(dot_);
        label_ = new brls::Label();
        label_->setSingleLine(true);
        label_->setFontSize(theme::kFontCaption);
        label_->setTextColor(theme::textSecondary());
        addView(label_);
    }

    void setState(bool running, const std::string& url) {
        dot_->running = running;
        if (!running) {
            setTextIfChanged(label_, tr("pipensx/web/off"));
            label_->setTextColor(theme::textTertiary());
        } else if (url.empty()) {
            setTextIfChanged(label_, tr("pipensx/settings/web_address_none"));
            label_->setTextColor(theme::textTertiary());
        } else {
            // Drop the scheme: the footer column is 216px, every pixel counts.
            setTextIfChanged(label_, url.rfind("http://", 0) == 0
                                         ? url.substr(7)
                                         : url);
            label_->setTextColor(theme::textSecondary());
        }
    }

private:
    class Dot : public brls::View {
    public:
        Dot() {
            setWidth(10);
            setHeight(10);
            setMarginRight(8);
            setFocusable(false);
            setAlignSelf(brls::AlignSelf::CENTER);
        }
        void draw(NVGcontext* vg, float x, float y, float width, float height,
                  brls::Style, brls::FrameContext*) override {
            nvgBeginPath(vg);
            nvgCircle(vg, x + width / 2.0f, y + height / 2.0f, 4.0f);
            nvgFillColor(vg, running ? theme::accent()
                                     : theme::textTertiary());
            nvgFill(vg);
        }
        bool running = false;
    };

    Dot* dot_ = nullptr;
    brls::Label* label_ = nullptr;
};

// Compact free-space readout for the top status row: a short filled pill
// (used vs. free, same semantics as StorageMeter's bar but sized for a
// header strip instead of the wide sidebar column) plus a "N free" label.
// StorageMeter itself is column-shaped with an optional header/legend built
// for that wider space - forcing it into a slim horizontal strip would mean
// fighting its layout rather than reading better, hence a dedicated widget.
class TopStorageIndicator : public brls::Box {
public:
    TopStorageIndicator() : brls::Box(brls::Axis::ROW) {
        setFocusable(false);
        setAlignItems(brls::AlignItems::CENTER);
        bar_ = new MiniBar();
        bar_->setWidth(56);
        bar_->setHeight(8);
        bar_->setMarginRight(8);
        addView(bar_);
        label_ = new brls::Label();
        label_->setSingleLine(true);
        label_->setFontSize(theme::kFontCaption);
        label_->setTextColor(theme::textSecondary());
        addView(label_);
    }

    void setStorage(uint64_t total, uint64_t free) {
        if (total == 0) {
            setUnavailable();
            return;
        }
        const uint64_t used = total >= free ? total - free : 0;
        bar_->setFraction(static_cast<double>(used) /
                          static_cast<double>(total));
        setTextIfChanged(label_,
                         tr("pipensx/storage/free", formatBytesShort(free)));
    }

    void setUnavailable() {
        bar_->setFraction(0.0);
        setTextIfChanged(label_, tr("pipensx/storage/unavailable"));
    }

private:
    class MiniBar : public brls::View {
    public:
        MiniBar() { setFocusable(false); }

        void setFraction(double usedFraction) {
            usedFraction_ = std::max(0.0, std::min(1.0, usedFraction));
        }

        void draw(NVGcontext* vg, float x, float y, float width, float height,
                  brls::Style, brls::FrameContext*) override {
            if (width <= 1.0f || height <= 1.0f)
                return;
            const float radius = height / 2.0f;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, width, height, radius);
            nvgFillColor(vg, theme::meterTrack());
            nvgFill(vg);
            const float usedW = static_cast<float>(width * usedFraction_);
            if (usedW > 1.0f) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, x, y, usedW, height, radius);
                nvgFillColor(vg, theme::accent());
                nvgFill(vg);
            }
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x + 0.5f, y + 0.5f, width - 1.0f,
                           height - 1.0f, radius);
            nvgStrokeWidth(vg, 1.0f);
            nvgStrokeColor(vg, theme::meterBorder());
            nvgStroke(vg);
        }

    private:
        double usedFraction_ = 0.0;
    };

    MiniBar* bar_ = nullptr;
    brls::Label* label_ = nullptr;
};

// Battery/wireless/clock, in the console's own top-right convention rather
// than borealis's stock bottom-right BottomBar (see attachTopStatus() below
// for how this replaces it on the main frame). Built from borealis's own
// public BatteryWidget/WirelessWidget - same widgets BottomBar itself uses -
// so it reads identically, just relocated.
class TopStatusRow : public brls::Box {
public:
    // manager/webServer are optional: the golden-runner/test builds construct
    // this with neither, and it just skips the async refresh cycle.
    TopStatusRow(DownloadManager* manager = nullptr,
                pipensx::WebServer* webServer = nullptr)
        : brls::Box(brls::Axis::ROW), manager_(manager),
          webServer_(webServer) {
        setFocusable(false);
        setAlignItems(brls::AlignItems::CENTER);

        storage_ = new TopStorageIndicator();
        storage_->setMarginRight(18);
        addView(storage_);
        webRow_ = new WebStatusRow();
        webRow_->setMarginBottom(0);
        webRow_->setMarginRight(18);
        addView(webRow_);

        brls::Platform* platform = brls::Application::getPlatform();
        battery_ = new brls::BatteryWidget();
        battery_->setMarginRight(14);
        battery_->setVisibility(platform->canShowBatteryLevel()
                                    ? brls::Visibility::VISIBLE
                                    : brls::Visibility::GONE);
        addView(battery_);
        wireless_ = new brls::WirelessWidget();
        wireless_->setMarginRight(14);
        wireless_->setVisibility(platform->canShowWirelessLevel()
                                     ? brls::Visibility::VISIBLE
                                     : brls::Visibility::GONE);
        addView(wireless_);
        clock_ = new brls::Label();
        clock_->setFontSize(19);
        clock_->setTextColor(theme::textSecondary());
        addView(clock_);

        if (manager_) {
            refreshStorage();
            refreshWebStatus();
            // Same reasoning as MainFrame's old footer timer: nsGetStorageSize
            // and nifmGetCurrentIpAddress are synchronous service IPC, so this
            // runs off a timer + brls::async rather than from draw().
            queryTimer_.setCallback([this] { scheduleRefresh(); });
            queryTimer_.start(2000);
        }
    }

    ~TopStatusRow() override {
        queryTimer_.stop();
        alive_->store(false);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        updateClock();
        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }

private:
    void updateClock() {
        const auto now = std::chrono::system_clock::now();
        const time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm local {};
        localtime_r(&t, &local);
        std::ostringstream ss;
        ss << std::put_time(&local, "%H:%M");
        const std::string text = ss.str();
        if (text != lastText_) {
            lastText_ = text;
            clock_->setText(text);
        }
    }

    void scheduleRefresh() {
        if (!manager_ || queryInFlight_)
            return;
        queryInFlight_ = true;
        auto alive = alive_;
        std::string root = manager_->rootPath();
        pipensx::WebServer* server = webServer_;
        brls::async([this, alive, root, server] {
            const pipensx::StorageSpaceSnapshot storage =
                pipensx::queryStorageSpace(root);
            const bool running = server ? server->running() : true;
            std::string url = running ? webCompanionUrl(server, true) : "";
            brls::sync([this, alive, storage, running, url = std::move(url)] {
                if (!alive->load())
                    return;
                queryInFlight_ = false;
                if (storage.available)
                    storage_->setStorage(storage.totalBytes, storage.freeBytes);
                else
                    storage_->setUnavailable();
                webRow_->setState(running, url);
            });
        });
    }

    void refreshStorage() {
        const pipensx::StorageSpaceSnapshot storage =
            pipensx::queryStorageSpace(manager_->rootPath());
        if (storage.available)
            storage_->setStorage(storage.totalBytes, storage.freeBytes);
        else
            storage_->setUnavailable();
    }

    void refreshWebStatus() {
        // A null server (golden runner) reads as "serving on the fixed fake
        // address" so the baseline row looks like the real thing.
        const bool running = webServer_ ? webServer_->running() : true;
        webRow_->setState(running,
                          running ? webCompanionUrl(webServer_, true) : "");
    }

    TopStorageIndicator* storage_ = nullptr;
    WebStatusRow* webRow_ = nullptr;
    brls::BatteryWidget* battery_ = nullptr;
    brls::WirelessWidget* wireless_ = nullptr;
    brls::Label* clock_ = nullptr;
    std::string lastText_;
    DownloadManager* manager_ = nullptr;
    pipensx::WebServer* webServer_ = nullptr;
    brls::RepeatingTimer queryTimer_;
    bool queryInFlight_ = false;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
};

// TabFrame that carries icons and folds to an icon rail while a tab is focused.
class MainFrame : public brls::TabFrame {
public:
    MainFrame() {
        expandedWidth_ =
            brls::Application::getStyle()["brls/tab_frame/sidebar_width"];
        if (expandedWidth_ < 1.0f)
            expandedWidth_ = 248.0f;
    }

    // Like TabFrame::addTab, but also plants an icon between the active-accent
    // bar and the label, and remembers the label so it can be folded away.
    void addNavTab(const std::string& label, NavIconType icon,
                   brls::TabViewCreator creator) {
        this->addTab(label, std::move(creator));
        const int index = static_cast<int>(this->sidebar->getItemsSize()) - 1;
        brls::SidebarItem* item = this->sidebar->getItem(index);
        if (!item)
            return;

        // Item children start as [accent, label]; capture the label before we
        // splice the icon in at index 1 -> [accent, icon, label].
        std::vector<brls::View*>& kids = item->getChildren();
        brls::View* labelView = kids.size() >= 2 ? kids[1] : nullptr;
        item->addView(new NavIcon(icon, item), 1);
        if (labelView) {
            labels_.push_back(labelView);
            if (collapsed_)
                labelView->setVisibility(brls::Visibility::GONE);
        }
    }

    void setCollapsed(bool collapsed) {
        if (collapsed == collapsed_)
            return;
        collapsed_ = collapsed;
        this->sidebar->setWidth(collapsed ? kCollapsedWidth : expandedWidth_);
        for (brls::View* label : labels_)
            label->setVisibility(collapsed ? brls::Visibility::GONE
                                           : brls::Visibility::VISIBLE);
    }

protected:
    // Focus in the sidebar -> expanded menu; focus in a tab's content -> icon
    // rail. Both subtrees are direct children of this frame, so this fires on
    // every menu<->content crossing.
    void onChildFocusGained(brls::View* directChild,
                            brls::View* focusedView) override {
        brls::TabFrame::onChildFocusGained(directChild, focusedView);
        setCollapsed(!(this->sidebar == directChild));
    }

private:
    // Wide enough for padding + the active-accent bar + the 28px icon.
    static constexpr float kCollapsedWidth = 88.0f;

    bool collapsed_ = false;
    float expandedWidth_ = 248.0f;
    std::vector<brls::View*> labels_;
};

}  // namespace pipensx::ui
