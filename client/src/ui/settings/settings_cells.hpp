#pragma once

#include <functional>
#include <string>

#include <borealis.hpp>

#include "app/app_settings.hpp"
#include "app/companion_settings.hpp"
#include "app/web_server.hpp"
#include "ui/common/ui_helpers.hpp"

namespace pipensx::ui {

// Shared row builders for the settings screen and its section panels, so
// every list renders identical section headers and action rows.

inline void addSection(brls::Box* content, const std::string& text) {
    auto* title = new brls::Label();
    title->setText(text);
    title->setFontSize(25);
    title->setMarginTop(14);
    title->setMarginBottom(8);
    content->addView(title);
}

// Small note under a section — same typography on every panel.
inline brls::Label* addNote(brls::Box* content, const std::string& text) {
    auto* note = new brls::Label();
    note->setText(text);
    note->setFontSize(16);
    note->setTextColor(theme::textSecondary());
    note->setMarginBottom(10);
    content->addView(note);
    return note;
}

inline brls::DetailCell* actionCell(const std::string& title,
                                    const std::string& detail,
                                    std::function<void()> callback) {
    auto* cell = new brls::DetailCell();
    cell->setText(title);
    cell->setDetailText(detail);
    cell->registerClickAction([callback = std::move(callback)](brls::View*) {
        callback();
        return true;
    });
    return cell;
}

// Persist through AppSettings, surface the error as a notification, and keep
// the web companion's GET /api/settings snapshot in sync with edits made from
// the console keyboard (not just its own PATCH). Shared by every panel so a
// failed write always looks the same.
inline bool persistSettings(AppSettings* settings, const AppSettingsData& values,
                            const char* tag, WebServer* webServer = nullptr) {
    std::string error;
    if (!settings->update(values, error)) {
        diagnostic_error("settings", tag, "error=%s", error.c_str());
        brls::Application::notify(error);
        return false;
    }
    if (webServer)
        webServer->updateSettingsSnapshot(companionSettingsJson(values));
    return true;
}

}  // namespace pipensx::ui
