#pragma once

#include <vector>

#include <borealis.hpp>

#include "app/game_metadata_service.hpp"
#include "app/installed_title_service.hpp"
#include "app/save_data_service.hpp"
#include "ui/common/async_image.hpp"
#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/saves/save_detail_activity.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

class SavesView;

class SavesDataSource : public brls::RecyclerDataSource {
public:
    explicit SavesDataSource(SavesView* owner) : owner_(owner) {}
    int numberOfRows(brls::RecyclerFrame*, int) override;
    brls::RecyclerCell* cellForRow(brls::RecyclerFrame*,
                                   brls::IndexPath) override;
    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath) override;

private:
    SavesView* owner_;
};

class SaveTitleCell : public brls::RecyclerCell {
public:
    SaveTitleCell() {
        setFocusable(true);
        setAxis(brls::Axis::ROW);
        setAlignItems(brls::AlignItems::CENTER);
        setHeight(70);
        setPadding(10, 24, 10, 24);

        cover_ = new brls::Box();
        cover_->setWidth(50);
        cover_->setHeight(50);
        cover_->setCornerRadius(8);
        cover_->setMarginRight(16);
        cover_->setBackgroundColor(theme::surface());
        cover_->setAlignItems(brls::AlignItems::CENTER);
        cover_->setJustifyContent(brls::JustifyContent::CENTER);
        cover_->setClipsToBounds(true);
        placeholder_ = new brls::Label();
        placeholder_->setFontSize(theme::kFontSmall);
        placeholder_->setTextColor(theme::textSecondary());
        cover_->addView(placeholder_);
        image_ = new AsyncRgbaImage();
        image_->setWidth(50);
        image_->setHeight(50);
        image_->setPositionType(brls::PositionType::ABSOLUTE);
        image_->setPositionTop(0);
        image_->setPositionLeft(0);
        image_->setCornerRadius(8);
        image_->setScalingType(brls::ImageScalingType::FILL);
        cover_->addView(image_);
        addView(cover_);

        auto* labels = new brls::Box(brls::Axis::COLUMN);
        name_ = new brls::Label();
        name_->setSingleLine(true);
        name_->setFontSize(20);
        labels->addView(name_);
        publisher_ = new brls::Label();
        publisher_->setSingleLine(true);
        publisher_->setFontSize(14);
        publisher_->setTextColor(theme::textTertiary());
        publisher_->setMarginTop(4);
        labels->addView(publisher_);
        addView(labels);
    }

    void setTitle(const InstalledTitle& title, GameMetadataService* metadata) {
        title_ = title;
        name_->setText(title.name);
        publisher_->setText(title.publisher);
        placeholder_->setText(placeholderLetter(title.name));
        setArtworkUrl(image_, metadata, title.iconPath, currentIconPath_,
                      imageState_);
    }

    const InstalledTitle& title() const { return title_; }

private:
    brls::Box* cover_ = nullptr;
    brls::Label* placeholder_ = nullptr;
    AsyncRgbaImage* image_ = nullptr;
    brls::Label* name_;
    brls::Label* publisher_;
    InstalledTitle title_;
    std::string currentIconPath_;
    std::shared_ptr<ImageRequestState> imageState_ =
        std::make_shared<ImageRequestState>();
};

// Per-title save data backups: pick an installed game, then back up, restore
// or delete its saves on the next screen (save_detail_activity.hpp).
class SavesView : public brls::Box {
public:
    SavesView(InstalledTitleService* installed, GameMetadataService* metadata)
        : brls::Box(brls::Axis::COLUMN), installed_(installed),
          metadata_(metadata) {
        setPadding(0, 34, 0, 34);

        statusLabel_ = new brls::Label();
        statusLabel_->setFontSize(theme::kFontSmall);
        statusLabel_->setTextColor(theme::textSecondary());
        statusLabel_->setMarginTop(24);
        statusLabel_->setMarginBottom(8);
        addView(statusLabel_);

        recycler_ = new brls::RecyclerFrame();
        recycler_->setGrow(1);
        recycler_->setPadding(6, 0, 6, 0);
        recycler_->estimatedRowHeight = 70;
        recycler_->registerCell("SaveTitle", [] { return new SaveTitleCell(); });
        recycler_->setDataSource(new SavesDataSource(this));
        addView(recyclerHost(recycler_));

        registerAction(tr("pipensx/common/refresh"), brls::BUTTON_RB,
                       [this](brls::View*) {
            reload();
            return true;
        });

        reload();
    }

    const std::vector<InstalledTitle>& titles() const { return titles_; }
    GameMetadataService* metadata() const { return metadata_; }

    void select(size_t index) {
        if (index >= titles_.size())
            return;
        brls::Application::pushActivity(
            new SaveDetailActivity(titles_[index]));
    }

private:
    void reload() {
        titles_ = installed_->titles();
        const bool hasAccount = saveDataAccountAvailable();
        statusLabel_->setText(
            hasAccount ? tr("pipensx/saves/pick_title")
                      : tr("pipensx/saves/no_account"));
        recycler_->reloadData();
    }

    InstalledTitleService* installed_;
    GameMetadataService* metadata_;
    std::vector<InstalledTitle> titles_;
    brls::Label* statusLabel_ = nullptr;
    brls::RecyclerFrame* recycler_ = nullptr;

    friend class SavesDataSource;
};

}  // namespace pipensx::ui
