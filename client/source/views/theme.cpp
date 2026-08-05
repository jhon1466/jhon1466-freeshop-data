#include "theme.hpp"

#include <borealis.hpp>

namespace freeshop
{

// Every value below is pipensx's own (src/ui/theme.hpp, read from
// https://github.com/i3sey/pipensx). Light and dark variants are both
// registered because Borealis switches between them from the console's own
// system setting, exactly as pipensx does - so the app follows the user's
// Switch theme instead of hardcoding one.
void registerTheme()
{
    brls::Theme& light = brls::Theme::getLightTheme();
    brls::Theme& dark  = brls::Theme::getDarkTheme();

    // ---- Brand & status ----
    // "Joy-Con Neon Blue #00C3E3" - the same accent in both variants.
    light.addColor("pipensx/accent", nvgRGB(0, 195, 227));
    dark.addColor("pipensx/accent", nvgRGB(0, 195, 227));
    light.addColor("pipensx/on_accent", nvgRGB(12, 34, 40));
    dark.addColor("pipensx/on_accent", nvgRGB(12, 34, 40));
    // "Neon Red #FF4554"
    light.addColor("pipensx/error", nvgRGB(255, 69, 84));
    dark.addColor("pipensx/error", nvgRGB(255, 69, 84));
    light.addColor("pipensx/success", nvgRGB(40, 170, 90));
    dark.addColor("pipensx/success", nvgRGB(96, 220, 130));
    light.addColor("pipensx/warning", nvgRGB(196, 110, 22));
    dark.addColor("pipensx/warning", nvgRGB(230, 150, 80));

    // ---- Text levels ----
    light.addColor("pipensx/text_primary", nvgRGB(45, 45, 45));
    dark.addColor("pipensx/text_primary", nvgRGB(245, 245, 250));
    light.addColor("pipensx/text_secondary", nvgRGB(90, 90, 95));
    dark.addColor("pipensx/text_secondary", nvgRGB(185, 185, 195));
    light.addColor("pipensx/text_tertiary", nvgRGB(125, 125, 130));
    dark.addColor("pipensx/text_tertiary", nvgRGB(150, 150, 160));
    light.addColor("pipensx/text_disabled", nvgRGB(175, 175, 180));
    dark.addColor("pipensx/text_disabled", nvgRGB(115, 115, 125));

    // ---- Surfaces ----
    light.addColor("pipensx/surface", nvgRGB(225, 225, 230));
    dark.addColor("pipensx/surface", nvgRGB(58, 58, 66));
    light.addColor("pipensx/overlay", nvgRGBA(240, 240, 244, 235));
    dark.addColor("pipensx/overlay", nvgRGBA(35, 35, 40, 235));
    light.addColor("pipensx/panel", nvgRGBA(228, 228, 234, 180));
    dark.addColor("pipensx/panel", nvgRGBA(45, 45, 50, 180));
    light.addColor("pipensx/track", nvgRGBA(128, 128, 128, 70));
    dark.addColor("pipensx/track", nvgRGBA(128, 128, 128, 70));

    // ---- Storage meter ----
    light.addColor("pipensx/meter_track", nvgRGB(205, 207, 213));
    dark.addColor("pipensx/meter_track", nvgRGB(24, 24, 27));
    light.addColor("pipensx/meter_used", nvgRGB(0, 195, 227));
    dark.addColor("pipensx/meter_used", nvgRGB(0, 195, 227));
    light.addColor("pipensx/meter_border", nvgRGBA(0, 0, 0, 50));
    dark.addColor("pipensx/meter_border", nvgRGBA(255, 255, 255, 55));

    // ---- Speed graph ----
    light.addColor("pipensx/graph_bg", nvgRGBA(208, 210, 216, 120));
    dark.addColor("pipensx/graph_bg", nvgRGBA(30, 31, 36, 120));
    light.addColor("pipensx/graph_grid", nvgRGBA(60, 60, 70, 35));
    dark.addColor("pipensx/graph_grid", nvgRGBA(180, 180, 190, 35));

    // ---- Borealis's own chrome, retinted to match ----
    //
    // Without these the framework's built-in accents (the focus highlight,
    // the sidebar's active-item marker) would keep Borealis's stock teal
    // while everything else is pipensx's neon blue.
    light.addColor("brls/accent", nvgRGB(0, 195, 227));
    dark.addColor("brls/accent", nvgRGB(0, 195, 227));
    light.addColor("brls/sidebar/active_item", nvgRGB(0, 195, 227));
    dark.addColor("brls/sidebar/active_item", nvgRGB(0, 195, 227));

    // ---- Metrics ----
    //
    // pipensx's corner radii (kRadiusSmall/Medium/Large) and type scale
    // (kFontTitle/Heading/Body/Small/Caption), so views can ask for them by
    // name instead of repeating literals.
    brls::Style style = brls::getStyle();

    // ---- Borealis layout metrics, overridden to pipensx's own ----
    //
    // These are pipensx's installSidebarStyle() values (src/ui/main_frame.hpp),
    // not guesses. The sidebar width matters most: Borealis defaults to 410,
    // which leaves only 870px of content area on a 1280px screen - not enough
    // for the 5-column, 964px-wide catalog grid below, so the grid overflowed
    // and the whole catalog screen looked broken. At 248 the content area is
    // 1032px and the grid fits with ~34px of breathing room each side.
    style.addMetric("brls/tab_frame/sidebar_width", 248.0f);
    style.addMetric("brls/sidebar/padding_left", 22.0f);  // Borealis default: 80
    style.addMetric("brls/sidebar/padding_right", 16.0f); // Borealis default: 40
    style.addMetric("brls/sidebar/padding_top", 28.0f);

    style.addMetric("pipensx/radius_small", 6.0f);
    style.addMetric("pipensx/radius_medium", 8.0f);
    style.addMetric("pipensx/radius_large", 12.0f);
    style.addMetric("pipensx/spacing", 8.0f);
    style.addMetric("pipensx/font_title", 32.0f);
    style.addMetric("pipensx/font_heading", 25.0f);
    style.addMetric("pipensx/font_body", 21.0f);
    style.addMetric("pipensx/font_small", 17.0f);
    style.addMetric("pipensx/font_caption", 15.0f);
}

} // namespace freeshop
