#pragma once

#include <memory>
#include <string>

#include <borealis.hpp>

#include "app/catalog_service.hpp"
#include "app/download_manager.hpp"
#include "app/game_metadata_service.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/progress_bar.hpp"
#include "ui/common/speed_graph.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// Download details: a flat, section-divided layout (no boxed panels) built
// around one big progress readout instead of stacked eShop-style cards -
// percentage and bytes sit right against a tall bar, and peers/pieces/ETA
// live as compact chips in one row rather than a vertical stats list.
class DetailsActivity : public brls::Activity {
public:
    DetailsActivity(std::string taskId, DownloadManager* manager,
                    CatalogService* catalog = nullptr,
                    GameMetadataService* metadata = nullptr)
        : taskId_(std::move(taskId)), manager_(manager), catalog_(catalog),
          metadata_(metadata) {
        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->setPadding(24, 40, 24, 40);
        content->setAlignItems(brls::AlignItems::STRETCH);

        auto* hero = new brls::Box(brls::Axis::ROW);
        hero->setAlignItems(brls::AlignItems::CENTER);
        hero->setMarginBottom(18);

        thumb_ = new brls::Box();
        thumb_->setWidth(64);
        thumb_->setHeight(64);
        thumb_->setCornerRadius(8);
        thumb_->setBackgroundColor(theme::surface());
        thumb_->setMarginRight(16);
        thumb_->setAlignItems(brls::AlignItems::CENTER);
        thumb_->setJustifyContent(brls::JustifyContent::CENTER);
        thumbPlaceholder_ = new brls::Label();
        thumbPlaceholder_->setFontSize(26);
        thumbPlaceholder_->setTextColor(theme::textSecondary());
        thumb_->addView(thumbPlaceholder_);
        thumbImage_ = new AsyncRgbaImage();
        thumbImage_->setWidth(64);
        thumbImage_->setHeight(64);
        thumbImage_->setPositionType(brls::PositionType::ABSOLUTE);
        thumbImage_->setPositionTop(0);
        thumbImage_->setPositionLeft(0);
        thumbImage_->setCornerRadius(8);
        thumbImage_->setScalingType(brls::ImageScalingType::FILL);
        thumb_->addView(thumbImage_);
        hero->addView(thumb_);

        status_ = new brls::Label();
        status_->setFontSize(theme::kFontHeading);
        hero->addView(status_);
        content->addView(hero);

        // Action buttons replace the old X/Y hotkeys.
        auto* actions = new brls::Box(brls::Axis::ROW);
        actions->setMarginBottom(28);
        pauseButton_ = addActionButton(actions, tr("pipensx/common/pause"),
                                       &brls::BUTTONSTYLE_PRIMARY);
        verifyButton_ = addActionButton(actions, tr("pipensx/common/verify"),
                                        &brls::BUTTONSTYLE_DEFAULT);
        removeButton_ = addActionButton(actions, tr("pipensx/common/remove"),
                                        &brls::BUTTONSTYLE_DEFAULT);
        content->addView(actions);
        pauseButton_->registerClickAction([this](brls::View*) {
            onPauseResume();
            return true;
        });
        verifyButton_->registerClickAction([this](brls::View*) {
            manager_->verify(taskId_);
            refresh();
            return true;
        });
        removeButton_->registerClickAction([this](brls::View*) {
            openRemoveDialog();
            return true;
        });

        // Hero readout: tall bar, big percentage/byte line right under it -
        // the one thing on this screen that should read at a glance.
        progressBar_ = new ProgressBar();
        progressBar_->setHeight(18);
        progressBar_->setMarginBottom(14);
        content->addView(progressBar_);
        progress_ = addLine(content, theme::kFontHeading);
        progress_->setMarginBottom(4);
        package_ = addLine(content, theme::kFontSmall);
        package_->setTextColor(theme::textSecondary());
        currentPackage_ = addLine(content, theme::kFontSmall);
        currentPackage_->setSingleLine(true);
        currentPackage_->setAutoAnimate(false);
        currentPackage_->setMarginBottom(18);

        // Compact stats: peers/pieces/ETA as pills in one row, instead of a
        // boxed "Network" card of stacked text lines.
        auto* chips = new brls::Box(brls::Axis::ROW);
        chips->setMarginBottom(26);
        eta_ = addChip(chips, &etaChip_);
        peers_ = addChip(chips);
        pieces_ = addChip(chips);
        content->addView(chips);

        addDivider(content);
        addSectionLabel(content, tr("pipensx/downloads/card_speed"));
        auto* speedLegend = new brls::Box(brls::Axis::ROW);
        speedLegend->setAlignItems(brls::AlignItems::CENTER);
        speedLegend->setMarginBottom(10);
        downloadSpeed_ = addSpeedLegend(speedLegend, theme::accent(), nullptr);
        installSpeed_ = addSpeedLegend(speedLegend, theme::success(),
                                       &installSpeedItem_);
        content->addView(speedLegend);
        speedGraph_ = new SpeedGraphView();
        content->addView(speedGraph_);

        error_ = addLine(content, theme::kFontSmall);
        error_->setMarginTop(18);
        error_->setTextColor(theme::error());

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        frame_ = new brls::AppletFrame(scroll);
    }

    brls::View* createContentView() override {
        return frame_;
    }

    void onContentAvailable() override {
        refresh();
        timer_.setCallback([this] { refresh(); });
        timer_.start(500);
        brls::Application::giveFocus(pauseButton_);
    }

    ~DetailsActivity() override {
        timer_.stop();
    }

private:
    static brls::Label* addLine(brls::Box* box, float size) {
        auto* label = new brls::Label();
        label->setWidth(brls::View::AUTO);
        label->setFontSize(size);
        label->setMarginBottom(6);
        box->addView(label);
        return label;
    }

    // Compact pill for a stat line (peers/pieces/ETA) - a row of these reads
    // as a glance-able summary instead of the "Network" card's stacked text.
    // itemOut, when given, captures the pill itself so a caller can hide it
    // (an empty ETA before any rate is known should disappear, not sit
    // there as an empty rounded rectangle).
    static brls::Label* addChip(brls::Box* row, brls::Box** itemOut = nullptr) {
        auto* pill = new brls::Box(brls::Axis::ROW);
        pill->setBackgroundColor(theme::surface());
        pill->setCornerRadius(999.0f);
        pill->setPadding(9, 16, 9, 16);
        pill->setMarginRight(10);
        pill->setMarginBottom(10);
        auto* label = new brls::Label();
        label->setWidth(brls::View::AUTO);
        label->setFontSize(theme::kFontCaption);
        label->setTextColor(theme::textSecondary());
        label->setSingleLine(true);
        pill->addView(label);
        row->addView(pill);
        if (itemOut)
            *itemOut = pill;
        return label;
    }

    // Thin rule instead of a boxed card - separates sections without
    // re-introducing the panel-everywhere look this screen used to have.
    static void addDivider(brls::Box* parent) {
        auto* line = new brls::Box();
        line->setHeight(1);
        line->setBackgroundColor(theme::track());
        line->setMarginBottom(18);
        parent->addView(line);
    }

    static void addSectionLabel(brls::Box* parent, const std::string& text) {
        auto* label = new brls::Label();
        label->setFontSize(theme::kFontCaption);
        label->setTextColor(theme::textSecondary());
        label->setText(text);
        label->setMarginBottom(12);
        parent->addView(label);
    }

    // A big, series-colored number reads as a speedometer next to the
    // graph - the old dot + body-size text sat flat beside a 150px chart.
    static brls::Label* addSpeedLegend(brls::Box* row, NVGcolor color,
                                       brls::Box** itemOut) {
        auto* item = new brls::Box(brls::Axis::ROW);
        item->setAlignItems(brls::AlignItems::CENTER);
        item->setMarginRight(36);

        auto* dot = new brls::Box();
        dot->setWidth(12);
        dot->setHeight(12);
        dot->setCornerRadius(6);
        dot->setBackgroundColor(color);
        dot->setMarginRight(10);
        item->addView(dot);

        auto* label = new brls::Label();
        label->setFontSize(theme::kFontTitle);
        label->setTextColor(color);
        label->setSingleLine(true);
        label->setAutoAnimate(false);
        item->addView(label);
        row->addView(item);
        if (itemOut)
            *itemOut = item;
        return label;
    }

    static brls::Button* addActionButton(brls::Box* row, const std::string& text,
                                         const brls::ButtonStyle* style) {
        auto* button = new brls::Button();
        button->setStyle(style);
        button->setFontSize(theme::kFontSmall);
        button->setHeight(52);
        button->setGrow(1);
        button->setMarginRight(12);
        button->setText(text);
        row->addView(button);
        return button;
    }

    const DownloadTask* currentTask() {
        cache_ = manager_->snapshot();
        for (const auto& task : cache_)
            if (task.id == taskId_)
                return &task;
        return nullptr;
    }

    void onPauseResume() {
        const DownloadTask* task = currentTask();
        if (!task)
            return;
        if (task->status == DownloadStatus::Paused ||
            task->status == DownloadStatus::Error)
            manager_->resume(taskId_);
        else
            manager_->pause(taskId_);
        refresh();
    }

    void refresh() {
        const DownloadTask* task = currentTask();
        if (!task) {
            brls::Application::popActivity();
            return;
        }
        if (frameTitle_ != task->name) {
            frameTitle_ = task->name;
            frame_->setTitle(frameTitle_);
            setTextIfChanged(thumbPlaceholder_, placeholderLetter(task->name));
        }
        setTextIfChanged(status_, tr("pipensx/downloads/status_line",
                                     downloadStatusLabel(task->status)));
        status_->setTextColor(statusColor(task->status));

        // Most torrent-sourced titles have no entry in the (much smaller)
        // metadata-match index — fall back to the catalog's own poster.
        std::string iconUrl;
        if (metadata_) {
            const GameMetadata* found = metadata_->findByInfoHash(task->id);
            if (found)
                iconUrl = found->iconUrl;
        }
        if (iconUrl.empty() && catalog_) {
            const CatalogEntry* entry = catalog_->findByInfoHash(task->id);
            if (entry)
                iconUrl = entry->posterUrl;
        }
        setArtworkUrl(thumbImage_, metadata_, iconUrl, currentIconUrl_,
                      imageState_);

        bool installing = task->status == DownloadStatus::Installing ||
                          task->status == DownloadStatus::Committing;
        float progress = installing ? installProgressOf(*task)
                                    : progressOf(*task);
        progressBar_->setProgress(progress);
        // Installing phases: the bar and the byte line track the same
        // per-package install numbers. Downloading/other: the byte line
        // follows the wanted (selection-aware) range like the bar does.
        const auto wanted = downloadProgressBytes(*task);
        const uint64_t doneBytes =
            installing ? task->installedBytes : wanted.first;
        const uint64_t totalBytes =
            installing ? task->installTotalBytes : wanted.second;
        setTextIfChanged(progress_, tr("pipensx/downloads/progress_line",
                                       percentOf(progress),
                                       formatBytes(doneBytes),
                                       formatBytes(totalBytes)));

        const uint64_t now = now_ms();
        std::string eta;
        if (auto seconds = taskEtaSeconds(*task, now))
            eta = formatEtaSeconds(*seconds);
        // No rate yet (e.g. install just started, 0 B/s) - hide the pill
        // rather than show it empty.
        etaChip_->setVisibility(eta.empty() ? brls::Visibility::GONE
                                            : brls::Visibility::VISIBLE);
        if (!eta.empty())
            setTextIfChanged(eta_, tr("pipensx/downloads/eta_line", eta));

        if (task->mode == TransferMode::StreamInstall && task->packageCount) {
            const bool hasCurrent = !task->currentPackage.empty() &&
                                    task->packagesInstalled < task->packageCount;
            if (hasCurrent) {
                setTextIfChanged(
                    package_,
                    tr("pipensx/downloads/package_of",
                       task->packagesInstalled + 1, task->packageCount));
            } else {
                setTextIfChanged(
                    package_,
                    tr("pipensx/downloads/packages_installed",
                       task->packagesInstalled, task->packageCount));
            }
            setTextIfChanged(currentPackage_, task->currentPackage);
        } else {
            setTextIfChanged(package_, "");
            setTextIfChanged(currentPackage_, "");
        }

        recordSpeedSample(*task, now);
        setTextIfChanged(downloadSpeed_,
                          tr("pipensx/downloads/speed_download",
                             formatSpeed(task->speedBytesPerSecond)));
        const uint64_t installSpeed = currentInstallSpeed(*task, now);
        if (task->mode == TransferMode::StreamInstall) {
            installSpeedItem_->setVisibility(brls::Visibility::VISIBLE);
            setTextIfChanged(installSpeed_,
                             tr("pipensx/downloads/speed_install",
                                formatSpeed(installSpeed)));
        } else {
            installSpeedItem_->setVisibility(brls::Visibility::GONE);
        }
        setTextIfChanged(peers_, tr("pipensx/downloads/peers_line", task->peers,
                                    task->dhtGood, task->dhtDubious));
        setTextIfChanged(pieces_,
                         tr("pipensx/downloads/pieces_line", task->piecesDone,
                            task->piecesTotal, task->piecesVerified));
        setTextIfChanged(error_,
                         task->error.empty()
                             ? std::string()
                             : tr("pipensx/downloads/error_line", task->error));

        updateButtons(*task);
    }

    void updateButtons(const DownloadTask& task) {
        bool paused = task.status == DownloadStatus::Paused ||
                            task.status == DownloadStatus::Error;
        bool active = task.status == DownloadStatus::Queued ||
                      task.status == DownloadStatus::Checking ||
                      task.status == DownloadStatus::Fetching ||
                      task.status == DownloadStatus::Downloading ||
                      task.status == DownloadStatus::Installing ||
                      task.status == DownloadStatus::Committing ||
                      task.status == DownloadStatus::Verifying;
        setTextIfChanged(pauseButton_, paused ? tr("pipensx/common/resume")
                                              : tr("pipensx/common/pause"));
        setButtonAvailable(pauseButton_, paused || active);

        bool canVerify = task.status == DownloadStatus::Paused ||
                         task.status == DownloadStatus::Error ||
                         task.status == DownloadStatus::Completed ||
                         task.status == DownloadStatus::Installed;
        setButtonAvailable(verifyButton_, canVerify);
        setButtonAvailable(removeButton_,
                           task.status != DownloadStatus::Removing);
    }

    static void setButtonAvailable(brls::Button* button, bool available) {
        button->setState(available ? brls::ButtonState::ENABLED
                                   : brls::ButtonState::DISABLED);
        button->setAlpha(available ? 1.0f : 0.32f);
    }

    static void appendSpeedSample(std::vector<uint64_t>& samples,
                                  uint64_t value) {
        constexpr size_t kMaxSpeedSamples = 60;
        if (samples.size() == kMaxSpeedSamples)
            samples.erase(samples.begin());
        samples.push_back(value);
    }

    void recordSpeedSample(const DownloadTask& task, uint64_t now) {
        appendSpeedSample(downloadSpeedSamples_, task.speedBytesPerSecond);

        if (task.mode == TransferMode::StreamInstall) {
            appendSpeedSample(installSpeedSamples_,
                              currentInstallSpeed(task, now));
        } else {
            installSpeedSamples_.clear();
        }

        speedGraph_->setSamples(downloadSpeedSamples_, installSpeedSamples_);
    }

    void openRemoveDialog() {
        auto* dialog = new brls::Dialog(
            tr("pipensx/downloads/remove_question"));
        dialog->addButton(tr("pipensx/downloads/remove_keep"), [this] {
            std::string error;
            if (!manager_->remove(taskId_, false, error))
                brls::Application::notify(error);
            else
                brls::Application::popActivity();
        });
        dialog->addButton(tr("pipensx/downloads/remove_delete"), [this] {
            std::string error;
            if (!manager_->remove(taskId_, true, error))
                brls::Application::notify(error);
            else
                brls::Application::popActivity();
        });
        dialog->addButton(tr("pipensx/common/cancel"), [] {});
        dialog->open();
    }

    std::string taskId_;
    DownloadManager* manager_;
    CatalogService* catalog_;
    GameMetadataService* metadata_;
    std::string frameTitle_;
    brls::AppletFrame* frame_;
    brls::Box* thumb_;
    brls::Label* thumbPlaceholder_;
    AsyncRgbaImage* thumbImage_;
    std::string currentIconUrl_;
    std::shared_ptr<ImageRequestState> imageState_ =
        std::make_shared<ImageRequestState>();
    brls::Label* status_;
    brls::Button* pauseButton_;
    brls::Button* verifyButton_;
    brls::Button* removeButton_;
    ProgressBar* progressBar_;
    brls::Label* progress_;
    brls::Label* package_;
    brls::Label* currentPackage_;
    brls::Label* eta_;
    brls::Box* etaChip_ = nullptr;
    brls::Label* downloadSpeed_;
    brls::Label* installSpeed_;
    brls::Box* installSpeedItem_;
    SpeedGraphView* speedGraph_;
    brls::Label* peers_;
    brls::Label* pieces_;
    brls::Label* error_;
    brls::RepeatingTimer timer_;
    std::vector<DownloadTask> cache_;
    std::vector<uint64_t> downloadSpeedSamples_;
    std::vector<uint64_t> installSpeedSamples_;
};

}  // namespace pipensx::ui
