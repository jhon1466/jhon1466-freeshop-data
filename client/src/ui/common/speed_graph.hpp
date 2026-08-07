#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <borealis.hpp>

#include "ui/common/ui_helpers.hpp"
#include "ui/theme.hpp"

namespace pipensx::ui {

// Radial speedometer gauges instead of a time-series line chart - a 270°
// arc per active transfer (download always, install only while streaming),
// with a soft glow backdrop and a bright dot riding the arc's tip. Reads
// the CURRENT rate only (no history), matching the dial-not-chart intent.
class SpeedGraphView : public brls::View {
public:
    SpeedGraphView() {
        setHeight(190);
        setMarginBottom(13);
    }

    void setSamples(std::vector<uint64_t> download,
                    std::vector<uint64_t> install) {
        download_ = std::move(download);
        install_ = std::move(install);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override {
        if (width <= 1 || height <= 1)
            return;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, width, height, 14);
        nvgFillColor(vg, theme::graphBg());
        nvgFill(vg);

        const uint64_t downloadSpeed = download_.empty() ? 0 : download_.back();
        const uint64_t installSpeed = install_.empty() ? 0 : install_.back();
        const bool showInstall = !install_.empty();

        // A ceiling that only grows, held for the component's lifetime: an
        // auto-scaling-per-frame max makes the needle jump to full deflection
        // on every small burst, which reads as noise, not speed.
        peakSpeed_ = std::max<uint64_t>(
            {peakSpeed_, downloadSpeed, installSpeed, 256ULL * 1024});

        if (showInstall) {
            const float half = width / 2.0f;
            drawGauge(vg, x, y, half, height, downloadSpeed, peakSpeed_,
                     theme::accent());
            drawGauge(vg, x + half, y, half, height, installSpeed, peakSpeed_,
                     theme::success());
        } else {
            drawGauge(vg, x, y, width, height, downloadSpeed, peakSpeed_,
                     theme::accent());
        }
    }

private:
    static void drawGauge(NVGcontext* vg, float x, float y, float w, float h,
                          uint64_t speed, uint64_t maxSpeed, NVGcolor color) {
        const float cx = x + w / 2.0f;
        const float cy = y + h / 2.0f + h * 0.06f;
        const float radius = std::min(w, h) * 0.36f;
        const float trackWidth = std::max(6.0f, radius * 0.22f);

        // 270° sweep starting at 7:30 and ending at 4:30, leaving the
        // bottom open - the classic speedometer shape.
        const float startAngle = 0.75f * NVG_PI;
        const float endAngle = 2.25f * NVG_PI;

        // Soft radial glow behind the dial, brighter with the needle.
        const double ratio = maxSpeed
            ? std::clamp(static_cast<double>(speed) /
                             static_cast<double>(maxSpeed),
                        0.0, 1.0)
            : 0.0;
        NVGcolor glowInner = color;
        glowInner.a = 0.10f + 0.14f * static_cast<float>(ratio);
        NVGcolor glowOuter = color;
        glowOuter.a = 0.0f;
        NVGpaint glow = nvgRadialGradient(vg, cx, cy, radius * 0.2f,
                                          radius * 1.35f, glowInner, glowOuter);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, radius * 1.35f);
        nvgFillPaint(vg, glow);
        nvgFill(vg);

        // Track (full sweep, dim).
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, radius, startAngle, endAngle, NVG_CW);
        nvgStrokeWidth(vg, trackWidth);
        nvgStrokeColor(vg, theme::track());
        nvgLineCap(vg, NVG_ROUND);
        nvgStroke(vg);

        // Value arc.
        const float valueAngle =
            startAngle + (endAngle - startAngle) * static_cast<float>(ratio);
        if (ratio > 0.003) {
            nvgBeginPath(vg);
            nvgArc(vg, cx, cy, radius, startAngle, valueAngle, NVG_CW);
            nvgStrokeWidth(vg, trackWidth);
            nvgStrokeColor(vg, color);
            nvgLineCap(vg, NVG_ROUND);
            nvgStroke(vg);
        }

        // Bright tip dot riding the end of the value arc - the "needle".
        const float tipAngle = ratio > 0.003 ? valueAngle : startAngle;
        const float tipX = cx + radius * cosf(tipAngle);
        const float tipY = cy + radius * sinf(tipAngle);
        NVGcolor tipGlow = color;
        tipGlow.a = 0.30f;
        nvgBeginPath(vg);
        nvgCircle(vg, tipX, tipY, trackWidth * 0.85f);
        nvgFillColor(vg, tipGlow);
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, tipX, tipY, trackWidth * 0.42f);
        nvgFillColor(vg, nvgRGBA(255, 255, 255, 235));
        nvgFill(vg);
    }

    std::vector<uint64_t> download_;
    std::vector<uint64_t> install_;
    uint64_t peakSpeed_ = 256ULL * 1024;
};

}  // namespace pipensx::ui
