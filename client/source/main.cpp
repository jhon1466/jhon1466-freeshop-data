// FreeShop's entry point, now a Borealis application.
//
// The whole UI layer is being moved onto Borealis - the same framework
// pipensx renders with (see ../THIRD_PARTY_NOTICES.md) - because pipensx's
// look isn't a palette that can be copied onto a hand-drawn SDL2 renderer:
// it *is* Borealis's widget set, layout engine and focus model. The old
// SDL2 screens under source/ui/ are being replaced one at a time by
// Borealis views under source/views/; source/ui/ is deliberately excluded
// from the CMake build while that lands.
//
// Everything below the UI - the torrent engine, installers, catalog
// fetching, MTP/FTP/save tooling - stays exactly as it is, plain C, and is
// reached from here through extern "C" headers.

#include <borealis.hpp>

#include <switch.h>

extern "C" {
#include "config.h"
#include "i18n.h"
}

#include "views/theme.hpp"

int main(int argc, char* argv[])
{
    // hbmenu passes the running .nro's own sdmc path as argv[0] - the
    // self-update path needs it to know what file to replace. Captured
    // before Borealis takes over argv handling.
    const char* self_path = (argc > 0 && argv && argv[0] && argv[0][0] != '\0') ? argv[0] : nullptr;
    (void)self_path; // wired back up when the update flow moves to Borealis

    // Picks the language every tr() call returns - see i18n.h. Cheap (one
    // service call) and every screen needs it, so it happens first.
    i18n_init();

    brls::Logger::setLogLevel(brls::LogLevel::LOG_WARNING);

    if (!brls::Application::init())
    {
        brls::Logger::error("Could not initialise Borealis");
        return EXIT_FAILURE;
    }

    brls::Application::createWindow("FreeShop");

    // Registers pipensx's own design tokens (its src/ui/theme.hpp values)
    // as Borealis theme/style entries, so every stock Borealis widget picks
    // them up without per-widget styling.
    freeshop::registerTheme();

    // Without this the console dims on its own inactivity timer regardless
    // of what's happening - including mid-download, where there's no
    // controller input for however long the transfer takes.
    appletSetAutoSleepDisabled(true);

    // Mounts the .nro's embedded RomFS (client/romfs/) as romfs:/ - static
    // assets that must work with no network. Best-effort.
    romfsInit();

    brls::Application::pushActivity(new brls::Activity());

    while (brls::Application::mainLoop())
        ;

    romfsExit();
    return EXIT_SUCCESS;
}
