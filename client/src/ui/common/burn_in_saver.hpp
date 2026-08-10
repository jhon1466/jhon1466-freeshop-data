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
            // Clock + battery drift together as one unit (phone-AOD style):
            // absolute + a translation animated in draw() below, since a
            // plain flex child cannot drift freely around the screen the way
            // a screensaver needs to.
            drifter_ = new brls::Box();
            drifter_->setAxis(brls::Axis::COLUMN);
            drifter_->setAlignItems(brls::AlignItems::CENTER);
            drifter_->setFocusable(false);
            drifter_->setPositionType(brls::PositionType::ABSOLUTE);
            drifter_->setPositionTopPercentage(50.f);
            drifter_->setPositionLeftPercentage(50.f);

            clock_ = new brls::Label();
            clock_->setFontSize(48.f);
            clock_->setTextColor(theme::burnInClock());
            clock_->setSingleLine(true);
            clock_->setFocusable(false);
            drifter_->addView(clock_);

            if (brls::Application::getPlatform()->canShowBatteryLevel()) {
                battery_ = new brls::Label();
                battery_->setFontSize(20.f);
                battery_->setTextColor(theme::burnInClock());
                battery_->setSingleLine(true);
                battery_->setFocusable(false);
                battery_->setMarginTop(6.f);
                drifter_->addView(battery_);
            }

            addView(drifter_);
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

        if (showClock_ && drifter_) {
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

            if (battery_)
                updateBattery();

            // Drifts around the center of the screen; the anchor above puts
            // the box's top-left at 50%/50%, so this offset also serves as
            // the (rough) centering — good enough for a screensaver.
            const float t = static_cast<float>(brls::getCPUTimeUsec()) * 1e-6f;
            const float ampX = width * 0.32f;
            const float ampY = height * 0.32f;
            drifter_->setTranslationX(ampX * std::sin(t * 0.05f));
            drifter_->setTranslationY(ampY * std::cos(t * 0.037f));
        }

        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }

    brls::View* getDefaultFocus() override { return this; }

private:
    // Refreshes the cached battery level/charging state off the main thread
    // every 5s - same throttled, fire-and-forget pattern as borealis's own
    // BatteryWidget/WirelessWidget, since psmGetBatteryChargePercentage is an
    // IPC call with no business running on the render thread every frame.
    // The label itself is only touched here, on the main thread.
    void updateBattery() {
        const brls::Time now = brls::getCPUTimeUsec();
        if (lastBatteryPollUsec_ == 0 ||
            now - lastBatteryPollUsec_ > 5'000'000) {
            lastBatteryPollUsec_ = now;
            ASYNC_RETAIN
            brls::async([ASYNC_TOKEN]() {
                ASYNC_RELEASE
                batteryLevel_ =
                    brls::Application::getPlatform()->getBatteryLevel();
                batteryCharging_ =
                    brls::Application::getPlatform()->isBatteryCharging();
            });
        }

        char buf[16];
        std::snprintf(buf, sizeof(buf), batteryCharging_ ? "%d%% +" : "%d%%",
                      batteryLevel_);
        if (lastBatteryText_ != buf) {
            lastBatteryText_ = buf;
            battery_->setText(lastBatteryText_);
        }
    }

    bool showClock_;
    brls::Box* drifter_ = nullptr;
    brls::Label* clock_ = nullptr;
    brls::Label* battery_ = nullptr;
    std::string lastText_;
    std::string lastBatteryText_;
    int batteryLevel_ = 0;
    bool batteryCharging_ = false;
    brls::Time lastBatteryPollUsec_ = 0;
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
