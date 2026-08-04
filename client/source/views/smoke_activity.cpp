#include "smoke_activity.hpp"

extern "C" {
#include "config.h"
}

namespace freeshop
{

brls::View* SmokeActivity::createContentView()
{
    brls::Box* content = new brls::Box(brls::Axis::COLUMN);
    content->setJustifyContent(brls::JustifyContent::CENTER);
    content->setAlignItems(brls::AlignItems::CENTER);
    content->setGrow(1.0f);

    brls::Theme theme = brls::Application::getTheme();
    brls::Style style = brls::Application::getStyle();

    brls::Label* headline = new brls::Label();
    headline->setText("Borealis funcionando");
    headline->setFontSize(style["pipensx/font_title"]);
    // Reading the accent back out of the theme (rather than hardcoding the
    // color here) is what actually proves registerTheme() landed - if the
    // token were missing this would fall back to Borealis's own color and
    // the text would not be pipensx's neon blue.
    headline->setTextColor(theme["pipensx/accent"]);
    content->addView(headline);

    brls::Label* detail = new brls::Label();
    detail->setText("FreeShop " CLIENT_VERSION "  -  interfaz pipensx (prueba de arranque)");
    detail->setFontSize(style["pipensx/font_body"]);
    detail->setTextColor(theme["pipensx/text_secondary"]);
    content->addView(detail);

    brls::Label* hint = new brls::Label();
    hint->setText("Si ves esto con el titulo en azul, el framework y el tema cargaron bien.\n"
                  "Prueba tambien: el boton B deberia salir de la app.");
    hint->setFontSize(style["pipensx/font_small"]);
    hint->setTextColor(theme["pipensx/text_tertiary"]);
    hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    content->addView(hint);

    // AppletFrame gives the real Switch chrome for free - the 88px header
    // with its separator rule, and the footer band with button hints. That
    // it renders correctly is half of what this screen is checking. The
    // content view goes through the constructor because setContentView is
    // protected on AppletFrame.
    brls::AppletFrame* frame = new brls::AppletFrame(content);
    frame->setTitle("FreeShop");
    return frame;
}

} // namespace freeshop
