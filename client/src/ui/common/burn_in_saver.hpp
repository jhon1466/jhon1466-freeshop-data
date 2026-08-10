#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include <borealis.hpp>

#include "ui/theme.hpp"

namespace pipensx::ui {

// Full-screen OLED burn-in guard: pure black, optionally with a dim clock
// that slowly drifts so nothing sits on the same pixels for hours. Dismissal
// is owned by main_switch's idle loop — any controller button or touch there
// pops this activity — so D-pad and touch work the same as face buttons
// without racing double-pops.
class BurnInSaverView : public brls::Box {
public:
    explicit BurnInSaverView(bool showClock) : showClock_(showClock) {
        setFocusable(true);
        setGrow(1.f);
        setBackgroundColor(theme::burnInBackdrop());
        if (showClock_) {
            clock_ = new brls::Label();
            clock_->setFontSize(48.f);
            clock_->setTextColor(theme::burnInClock());
            clock_->setSingleLine(true);
            clock_->setFocusable(false);
            // Absolute + a translation animated in draw() below: a plain
            // flex child cannot drift freely around the screen the way a
            // screensaver needs to.
            clock_->setPositionType(brls::PositionType::ABSOLUTE);
            clock_->setPositionTopPercentage(50.f);
            clock_->setPositionLeftPercentage(50.f);
            addView(clock_);
        }
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override {
        // Explicit fill instead of relying on the Box's own background
        // paint: guarantees a flat, fully opaque black covering the exact
        // frame regardless of any theme/background-style interaction.
        // Neither overdrawing nor insetting the fill changed the stray
        // corner pixels (tried both) - back to the exact frame. Whatever
        // shows through at the corners is below this draw call, not a
        // coverage gap in it.
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillColor(vg, nvgRGB(0, 0, 0));
        nvgFill(vg);

        if (showClock_ && clock_) {
            const time_t now = time(nullptr);
            struct tm local {};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour,
                          local.tm_min);
            if (lastText_ != buf) {
                lastText_ = buf;
                clock_->setText(lastText_);
            }
            // Drifts around the center of the screen; the anchor above puts
            // the label's top-left at 50%/50%, so this offset also serves
            // as the (rough) centering — good enough for a screensaver.
            const float t = static_cast<float>(brls::getCPUTimeUsec()) * 1e-6f;
            const float ampX = width * 0.32f;
            const float ampY = height * 0.32f;
            clock_->setTranslationX(ampX * std::sin(t * 0.05f));
            clock_->setTranslationY(ampY * std::cos(t * 0.037f));
        }

        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }

    brls::View* getDefaultFocus() override { return this; }

private:
    bool showClock_;
    brls::Label* clock_ = nullptr;
    std::string lastText_;
};

class BurnInSaverActivity : public brls::Activity {
public:
    explicit BurnInSaverActivity(bool showClock) : showClock_(showClock) {}

    brls::View* createContentView() override {
        return new BurnInSaverView(showClock_);
    }

private:
    bool showClock_;
};

inline bool controllerHasButtonDown(const brls::ControllerState& state) {
    for (int i = 0; i < brls::_BUTTON_MAX; ++i) {
        if (state.buttons[i])
            return true;
    }
    return false;
}

inline bool burnInSaverIsTop() {
    const auto stack = brls::Application::getActivitiesStack();
    return !stack.empty() &&
           dynamic_cast<BurnInSaverActivity*>(stack.back()) != nullptr;
}

} // namespace pipensx::ui
