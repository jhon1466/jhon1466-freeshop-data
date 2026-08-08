#pragma once

#include <string>

#include <borealis.hpp>

#include "ui/i18n.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

namespace detail {

// The release body comes from GitHub's Markdown release notes. This isn't a
// Markdown renderer - just enough cleanup that "## Heading" and "**bold**"
// read as plain prose instead of showing their literal markers, since the
// notes are otherwise short, hand-written changelog text (see
// scripts/update-release-notes.js), never arbitrary user content.
inline std::string stripLightMarkdown(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = text.size();
        size_t begin = lineStart;
        while (begin < lineEnd && (text[begin] == '#' || text[begin] == ' '))
            ++begin;
        out.append(text, begin, lineEnd - begin);
        if (lineEnd < text.size())
            out.push_back('\n');
        lineStart = lineEnd + 1;
    }
    std::string result;
    result.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '*')
            continue;
        result.push_back(out[i]);
    }
    return result;
}

}  // namespace detail

// Shown once, right after an update finishes installing and the app
// confirms the swap on its first relaunch - mirrors WelcomeView's role for a
// brand new user, but for "what changed" instead of "what this app does".
class UpdateNotesView : public brls::Box {
public:
    UpdateNotesView(const std::string& version, const std::string& notes)
        : brls::Box(brls::Axis::COLUMN) {
        setPadding(24, 40, 24, 40);

        auto* heading = new brls::Label();
        heading->setText(tr("pipensx/update_notes/heading", version));
        heading->setFontSize(theme::kFontTitle);
        heading->setTextColor(theme::textPrimary());
        heading->setMarginBottom(18);
        heading->setSingleLine(false);
        addView(heading);

        auto* body = new brls::Label();
        body->setText(detail::stripLightMarkdown(notes));
        body->setFontSize(theme::kFontSmall);
        body->setTextColor(theme::textSecondary());
        body->setSingleLine(false);

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1);
        scroll->setContentView(body);
        addView(scroll);

        auto* continueButton = new brls::Button();
        continueButton->setStyle(&brls::BUTTONSTYLE_PRIMARY);
        continueButton->setText(tr("pipensx/update_notes/continue"));
        continueButton->setHeight(56);
        continueButton->setMarginTop(20);
        continueButton->registerClickAction([](brls::View*) {
            brls::Application::popActivity();
            return true;
        });
        addView(continueButton);
        setDefaultFocusedIndex(2);
    }
};

inline void showUpdateNotesScreen(const std::string& version,
                                  const std::string& notes) {
    auto* frame = new brls::AppletFrame(new UpdateNotesView(version, notes));
    frame->setTitle(tr("pipensx/update_notes/title"));
    brls::Application::pushActivity(new brls::Activity(frame));
}

}  // namespace pipensx::ui
