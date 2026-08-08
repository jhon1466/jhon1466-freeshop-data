#pragma once

#include <string>

#include <borealis.hpp>

#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// One row of the feature rundown on the first-run welcome screen: a short
// bold-ish heading plus a one/two-line description. Purely informational -
// not focusable, no action.
class WelcomeFeatureRow : public brls::Box {
public:
    WelcomeFeatureRow(const std::string& title, const std::string& body)
        : brls::Box(brls::Axis::COLUMN) {
        setFocusable(false);
        setMarginBottom(22);

        auto* heading = new brls::Label();
        heading->setText(title);
        heading->setFontSize(theme::kFontBody);
        heading->setTextColor(theme::accent());
        heading->setMarginBottom(4);
        addView(heading);

        auto* description = new brls::Label();
        description->setText(body);
        description->setFontSize(theme::kFontSmall);
        description->setTextColor(theme::textSecondary());
        description->setSingleLine(false);
        addView(description);
    }
};

// First-launch-only welcome screen: a greeting plus a plain-language rundown
// of what the app can do (catalog, downloads, explorer, saves, web
// companion, settings), so a brand new user has some orientation before
// landing on the tab bar. Purely a one-way informational stop - B (the
// AppletFrame's own default back action) and the "Comenzar" button both just
// pop it.
class WelcomeView : public brls::Box {
public:
    WelcomeView() : brls::Box(brls::Axis::COLUMN) {
        setPadding(24, 40, 24, 40);

        auto* greeting = new brls::Label();
        greeting->setText(tr("pipensx/welcome/greeting"));
        greeting->setFontSize(theme::kFontTitle);
        greeting->setTextColor(theme::textPrimary());
        greeting->setMarginBottom(8);
        addView(greeting);

        auto* intro = new brls::Label();
        intro->setText(tr("pipensx/welcome/intro"));
        intro->setFontSize(theme::kFontSmall);
        intro->setTextColor(theme::textSecondary());
        intro->setSingleLine(false);
        intro->setMarginBottom(22);
        addView(intro);

        auto* content = new brls::Box(brls::Axis::COLUMN);
        content->addView(new WelcomeFeatureRow(
            tr("pipensx/welcome/feature_catalog_title"),
            tr("pipensx/welcome/feature_catalog_body")));
        content->addView(new WelcomeFeatureRow(
            tr("pipensx/welcome/feature_downloads_title"),
            tr("pipensx/welcome/feature_downloads_body")));
        content->addView(new WelcomeFeatureRow(
            tr("pipensx/welcome/feature_explorer_title"),
            tr("pipensx/welcome/feature_explorer_body")));
        content->addView(new WelcomeFeatureRow(
            tr("pipensx/welcome/feature_saves_title"),
            tr("pipensx/welcome/feature_saves_body")));
        content->addView(new WelcomeFeatureRow(
            tr("pipensx/welcome/feature_web_title"),
            tr("pipensx/welcome/feature_web_body")));
        content->addView(new WelcomeFeatureRow(
            tr("pipensx/welcome/feature_settings_title"),
            tr("pipensx/welcome/feature_settings_body")));

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(content);
        addView(scroll);

        auto* start = new brls::Button();
        start->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        start->setText(tr("pipensx/welcome/start"));
        start->setHeight(56);
        start->setMarginTop(20);
        start->registerClickAction([](brls::View*) {
            brls::Application::popActivity();
            return true;
        });
        addView(start);
        setDefaultFocusedIndex(3);
    }
};

inline void showWelcomeScreen() {
    auto* frame = new brls::AppletFrame(new WelcomeView());
    frame->setTitle(tr("pipensx/welcome/title"));
    brls::Application::pushActivity(new brls::Activity(frame));
}

}  // namespace pipensx::ui
