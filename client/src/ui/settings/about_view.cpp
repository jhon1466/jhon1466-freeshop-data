#include "ui/settings/about_view.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include "ui/common/ui_helpers.hpp"
#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

namespace {

constexpr float kCardGap = theme::kSpacingUnit * 2.0f;
constexpr float kIconFrameSize = 140.0f;
constexpr float kIconSize = 112.0f;

brls::Box* makeCard(brls::Axis axis = brls::Axis::COLUMN) {
    auto* card = new brls::Box(axis);
    card->setBackgroundColor(theme::panel());
    card->setBorderColor(theme::track());
    card->setBorderThickness(1.0f);
    card->setCornerRadius(theme::kRadiusLarge);
    card->setPadding(24, 28, 24, 28);
    return card;
}

brls::Label* addLabel(brls::Box* parent, const std::string& text, float size,
                      NVGcolor color) {
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(size);
    label->setTextColor(color);
    parent->addView(label);
    return label;
}

constexpr float kQrSize = 176.0f;

} // namespace

AboutView::AboutView() : brls::Box(brls::Axis::COLUMN) {
    auto* content = new brls::Box(brls::Axis::COLUMN);
    content->setPadding(24, 34, 24, 34);

    auto* hero = makeCard(brls::Axis::ROW);
    hero->setAlignItems(brls::AlignItems::CENTER);
    hero->setMarginBottom(kCardGap);

    auto* iconFrame = new brls::Box(brls::Axis::ROW);
    iconFrame->setWidth(kIconFrameSize);
    iconFrame->setHeight(kIconFrameSize);
    iconFrame->setBackgroundColor(theme::surface());
    iconFrame->setCornerRadius(theme::kRadiusLarge);
    iconFrame->setJustifyContent(brls::JustifyContent::CENTER);
    iconFrame->setAlignItems(brls::AlignItems::CENTER);
    iconFrame->setMarginRight(24);
    auto* icon = new brls::Image();
    icon->setWidth(kIconSize);
    icon->setHeight(kIconSize);
    icon->setScalingType(brls::ImageScalingType::FIT);
    icon->setImageFromRes("icon.png");
    iconFrame->addView(icon);
    hero->addView(iconFrame);

    auto* summary = new brls::Box(brls::Axis::COLUMN);
    summary->setGrow(1);
    auto* title =
        addLabel(summary, "FreeShop", theme::kFontTitle, theme::textPrimary());
    title->setMarginBottom(10);
    auto* version = addLabel(summary,
                             tr("pipensx/about/version", PIPENSX_VERSION),
                             theme::kFontSmall, theme::accent());
    version->setMarginBottom(4);
    auto* build = addLabel(summary,
                           tr("pipensx/about/built", __DATE__, __TIME__),
                           theme::kFontCaption, theme::textTertiary());
    build->setMarginBottom(14);
    auto* description = addLabel(
        summary, tr("pipensx/about/description"),
        theme::kFontSmall, theme::textSecondary());
    description->setMarginBottom(12);
    hero->addView(summary);
    content->addView(hero);

    auto* support = makeCard(brls::Axis::ROW);
    support->setMarginBottom(kCardGap);

    auto* qrFrame = new brls::Box(brls::Axis::ROW);
    qrFrame->setWidth(kQrSize);
    qrFrame->setHeight(kQrSize);
    qrFrame->setMarginRight(24);
    auto* qr = new brls::Image();
    qr->setWidth(kQrSize);
    qr->setHeight(kQrSize);
    qr->setScalingType(brls::ImageScalingType::FIT);
    qr->setImageFromRes("qr_paypal.jpg");
    qrFrame->addView(qr);
    support->addView(qrFrame);

    auto* donateText = new brls::Box(brls::Axis::COLUMN);
    donateText->setGrow(1);
    auto* donateQuestion = addLabel(donateText,
        tr("pipensx/about/donate_question"), theme::kFontBody,
        theme::textPrimary());
    donateQuestion->setMarginBottom(10);
    auto* donateBody = addLabel(donateText,
        tr("pipensx/about/donate_text"), theme::kFontCaption,
        theme::textSecondary());
    donateBody->setMarginBottom(14);
    auto* donateScan = addLabel(donateText,
        tr("pipensx/about/donate_scan"), theme::kFontSmall, theme::accent());
    donateScan->setMarginBottom(4);
    addLabel(donateText, "mastergarden1112@gmail.com", theme::kFontBody,
             theme::textPrimary());
    support->addView(donateText);
    content->addView(support);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1);
    scroll->setContentView(content);
    addView(scroll);
}

} // namespace pipensx::ui
