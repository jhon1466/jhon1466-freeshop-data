#pragma once

#include <string>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/mtp_service.hpp"
#include "ui/common/progress_bar.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// Small status dot, same look language main_frame.hpp's WebStatusRow uses:
// accent while ready, muted otherwise.
class MtpStatusDot : public brls::View {
public:
    MtpStatusDot() {
        setWidth(14);
        setHeight(14);
        setMarginRight(14);
        setFocusable(false);
    }

    void setActive(bool active) { active_ = active; }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        nvgBeginPath(vg);
        nvgCircle(vg, x + width / 2.0f, y + height / 2.0f, width / 2.0f);
        nvgFillColor(vg, active_ ? theme::success() : theme::textTertiary());
        nvgFill(vg);
    }

private:
    bool active_ = false;
};

// One row of the session's transfer history - filename on the left, outcome
// on the right, in the same colour language the rest of the app uses for
// install/download status.
class MtpHistoryRow : public brls::Box {
public:
    explicit MtpHistoryRow(const mtp::MtpHistoryItem& item)
        : brls::Box(brls::Axis::ROW) {
        setFocusable(false);
        setHeight(36);
        setAlignItems(brls::AlignItems::CENTER);

        name_ = new brls::Label();
        name_->setFontSize(theme::kFontSmall);
        name_->setTextColor(theme::textPrimary());
        name_->setSingleLine(true);
        name_->setGrow(1);
        name_->setText(item.filename);
        addView(name_);

        const bool installed = item.status == mtp::MtpHistoryStatus::Installed;
        status_ = new brls::Label();
        status_->setFontSize(theme::kFontSmall);
        status_->setText(installed ? tr("pipensx/mtp/status_installed")
                                   : tr("pipensx/mtp/status_error"));
        status_->setTextColor(installed ? theme::success() : theme::error());
        addView(status_);
    }

private:
    brls::Label* name_;
    brls::Label* status_;
};

inline brls::Box* mtpDivider() {
    auto* divider = new brls::Box();
    divider->setHeight(1);
    divider->setFocusable(false);
    divider->setBackgroundColor(theme::panel());
    divider->setMarginTop(18);
    divider->setMarginBottom(18);
    return divider;
}

// MTP tab: presents the console as a PTP/MTP USB device (see
// app/mtp_service.hpp, mtp/mtp_ptp.hpp) so a PC can drag-and-drop a .nsp/
// .nsz straight onto it for a direct install, no SD card removal or cable
// swap needed. Only active while this tab is actually visible - willAppear/
// willDisappear start and stop the USB responder, matching the old SDL2
// client's screen-scoped lifecycle (holding the USB device role for the
// whole app session rather than just this screen wasn't the original
// design and isn't worth changing).
class MtpView : public brls::Box {
public:
    explicit MtpView(AppSettings* settings)
        : brls::Box(brls::Axis::COLUMN), settings_(settings),
          service_("sdmc:/switch/freeshop-client") {
        setPadding(24, 34, 24, 34);

        buildIdleSection();
        buildBusySection();
        busy_->setVisibility(brls::Visibility::GONE);

        timer_.setCallback([this] { refresh(); });
    }

    ~MtpView() override {
        timer_.stop();
    }

    void willAppear(bool resetState) override {
        brls::Box::willAppear(resetState);
        if (!service_.running()) {
            std::string error;
            install::InstallStorageTarget target =
                installTargetFor(settings_->get().installLocation);
            if (!service_.start(target, error))
                brls::Application::notify(tr("pipensx/mtp/start_failed", error));
        }
        lastHistoryCount_ = static_cast<size_t>(-1); // force a rebuild
        timer_.start(400);
        refresh();
    }

    void willDisappear(bool resetState) override {
        timer_.stop();
        service_.stop();
        brls::Box::willDisappear(resetState);
    }

private:
    void buildIdleSection() {
        idle_ = new brls::Box(brls::Axis::COLUMN);
        idle_->setGrow(1);

        auto* statusRow = new brls::Box(brls::Axis::ROW);
        statusRow->setFocusable(false);
        statusRow->setAlignItems(brls::AlignItems::CENTER);
        statusRow->setMarginBottom(14);
        dot_ = new MtpStatusDot();
        statusRow->addView(dot_);
        statusLabel_ = new brls::Label();
        statusLabel_->setFontSize(theme::kFontHeading);
        statusLabel_->setTextColor(theme::textPrimary());
        statusRow->addView(statusLabel_);
        idle_->addView(statusRow);

        helpLabel_ = new brls::Label();
        helpLabel_->setFontSize(theme::kFontSmall);
        helpLabel_->setTextColor(theme::textSecondary());
        helpLabel_->setSingleLine(false);
        helpLabel_->setMarginBottom(6);
        idle_->addView(helpLabel_);

        formatsLabel_ = new brls::Label();
        formatsLabel_->setFontSize(theme::kFontCaption);
        formatsLabel_->setTextColor(theme::textTertiary());
        formatsLabel_->setText(tr("pipensx/mtp/formats"));
        idle_->addView(formatsLabel_);

        idle_->addView(mtpDivider());

        historyTitle_ = new brls::Label();
        historyTitle_->setFontSize(theme::kFontSmall);
        historyTitle_->setTextColor(theme::textSecondary());
        historyTitle_->setMarginBottom(12);
        idle_->addView(historyTitle_);

        emptyLabel_ = new brls::Label();
        emptyLabel_->setFontSize(theme::kFontSmall);
        emptyLabel_->setTextColor(theme::textTertiary());
        emptyLabel_->setText(tr("pipensx/mtp/queue_empty"));
        idle_->addView(emptyLabel_);

        historyList_ = new brls::Box(brls::Axis::COLUMN);
        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(historyList_);
        idle_->addView(scroll);

        addView(idle_);
    }

    void buildBusySection() {
        busy_ = new brls::Box(brls::Axis::COLUMN);
        busy_->setGrow(1);

        busyTitle_ = new brls::Label();
        busyTitle_->setFontSize(theme::kFontSmall);
        busyTitle_->setTextColor(theme::textSecondary());
        busyTitle_->setMarginBottom(10);
        busy_->addView(busyTitle_);

        busyFile_ = new brls::Label();
        busyFile_->setFontSize(theme::kFontHeading);
        busyFile_->setTextColor(theme::textPrimary());
        busyFile_->setSingleLine(true);
        busyFile_->setMarginBottom(18);
        busy_->addView(busyFile_);

        busyDetail_ = new brls::Label();
        busyDetail_->setFontSize(theme::kFontSmall);
        busyDetail_->setTextColor(theme::textSecondary());
        busyDetail_->setMarginBottom(10);
        busy_->addView(busyDetail_);

        progressBar_ = new ProgressBar();
        busy_->addView(progressBar_);

        addView(busy_);
    }

    void refresh() {
        const MtpSnapshot snap = service_.snapshot();
        const bool busy = !snap.currentFile.empty();
        idle_->setVisibility(busy ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        busy_->setVisibility(busy ? brls::Visibility::VISIBLE : brls::Visibility::GONE);

        if (busy) {
            const bool installing = snap.status == mtp::MtpStatus::Installing;
            setTextIfChanged(busyTitle_, installing
                                             ? tr("pipensx/mtp/installing_now")
                                             : tr("pipensx/mtp/receiving"));
            setTextIfChanged(busyFile_, snap.currentFile);
            if (installing) {
                progressBar_->setVisibility(brls::Visibility::GONE);
                setTextIfChanged(busyDetail_, "");
            } else {
                progressBar_->setVisibility(brls::Visibility::VISIBLE);
                std::string detail;
                if (snap.progressTotal > 0) {
                    const float pct = static_cast<float>(snap.progressNow) /
                                      static_cast<float>(snap.progressTotal);
                    progressBar_->setProgress(pct);
                    detail = formatBytes(snap.progressNow) + " / " +
                             formatBytes(snap.progressTotal) + "  (" +
                             std::to_string(static_cast<int>(pct * 100)) + "%)";
                } else {
                    progressBar_->setProgress(0.0f);
                    detail = formatBytes(snap.progressNow);
                }
                if (snap.speedBytesPerSecond > 0) {
                    detail += "   " + formatSpeed(snap.speedBytesPerSecond);
                    if (snap.progressTotal > snap.progressNow) {
                        const uint64_t remaining =
                            snap.progressTotal - snap.progressNow;
                        detail += tr("pipensx/downloads/cell_eta",
                                     formatEtaSeconds(remaining /
                                                      snap.speedBytesPerSecond));
                    }
                }
                setTextIfChanged(busyDetail_, detail);
            }
            return;
        }

        const bool ready = snap.status == mtp::MtpStatus::Idle;
        dot_->setActive(ready);
        switch (snap.status) {
            case mtp::MtpStatus::WaitingForUsb:
                setTextIfChanged(statusLabel_, tr("pipensx/mtp/waiting_usb"));
                setTextIfChanged(helpLabel_, tr("pipensx/mtp/waiting_usb_help"));
                break;
            case mtp::MtpStatus::WaitingForHost:
                setTextIfChanged(statusLabel_, tr("pipensx/mtp/waiting_host"));
                setTextIfChanged(helpLabel_, tr("pipensx/mtp/waiting_host_help"));
                break;
            default:
                setTextIfChanged(statusLabel_, tr("pipensx/mtp/ready"));
                setTextIfChanged(helpLabel_, tr("pipensx/mtp/help"));
                break;
        }
        formatsLabel_->setVisibility(ready ? brls::Visibility::VISIBLE
                                           : brls::Visibility::GONE);

        const std::string title = snap.history.empty()
            ? tr("pipensx/mtp/queue_title")
            : tr("pipensx/mtp/queue_title") + " (" +
                  std::to_string(snap.history.size()) + ")";
        setTextIfChanged(historyTitle_, title);
        emptyLabel_->setVisibility(snap.history.empty() ? brls::Visibility::VISIBLE
                                                        : brls::Visibility::GONE);

        if (snap.history.size() != lastHistoryCount_) {
            lastHistoryCount_ = snap.history.size();
            historyList_->clearViews();
            // Newest first - what just happened matters more than the
            // first file of a long batch.
            for (auto it = snap.history.rbegin(); it != snap.history.rend(); ++it)
                historyList_->addView(new MtpHistoryRow(*it));
        }
    }

    AppSettings* settings_;
    MtpService service_;
    brls::RepeatingTimer timer_;
    size_t lastHistoryCount_ = static_cast<size_t>(-1);

    brls::Box* idle_ = nullptr;
    MtpStatusDot* dot_ = nullptr;
    brls::Label* statusLabel_ = nullptr;
    brls::Label* helpLabel_ = nullptr;
    brls::Label* formatsLabel_ = nullptr;
    brls::Label* historyTitle_ = nullptr;
    brls::Label* emptyLabel_ = nullptr;
    brls::Box* historyList_ = nullptr;

    brls::Box* busy_ = nullptr;
    brls::Label* busyTitle_ = nullptr;
    brls::Label* busyFile_ = nullptr;
    brls::Label* busyDetail_ = nullptr;
    ProgressBar* progressBar_ = nullptr;
};

} // namespace pipensx::ui
