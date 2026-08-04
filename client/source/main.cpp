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

#include "views/smoke_activity.hpp"
#include "views/theme.hpp"

// Appends one line to sdmc:/switch/freeshop/boot.log. The crash this is
// here to chase reported nx-hbmenu as the only loaded module, i.e. our code
// may never have been mapped at all - and "did main() even run, and how far
// did it get" is not something a crash report from *hbmenu* can answer.
// Opened and closed per call so a hard crash can't lose buffered output.
static void boot_log(const char* stage)
{
    FILE* fp = fopen("sdmc:/switch/freeshop/boot.log", "a");
    if (!fp)
        return;
    fprintf(fp, "%s\n", stage);
    fclose(fp);
}

int main(int argc, char* argv[])
{
    boot_log("--- launch ---");
    // hbmenu passes the running .nro's own sdmc path as argv[0] - the
    // self-update path needs it to know what file to replace. Captured
    // before Borealis takes over argv handling.
    const char* self_path = (argc > 0 && argv && argv[0] && argv[0][0] != '\0') ? argv[0] : nullptr;
    (void)self_path; // wired back up when the update flow moves to Borealis

    brls::Logger::setLogLevel(brls::LogLevel::LOG_WARNING);

    boot_log("before Application::init");
    if (!brls::Application::init())
    {
        boot_log("Application::init FAILED");
        brls::Logger::error("Could not initialise Borealis");
        return EXIT_FAILURE;
    }
    boot_log("after Application::init");

    brls::Application::createWindow("FreeShop");
    boot_log("after createWindow");

    // After Application::init(), not before: Borealis's own libnx entry
    // hook (its lib/platforms/switch/switch_wrapper.c userAppInit) is what
    // brings up romfs, the socket stack, pl/set/psm/nifm - the very
    // services i18n_init and the rest of the C backend expect to already be
    // running. Calling into them ahead of that is asking uninitialised
    // services for answers.
    i18n_init();
    boot_log("after i18n_init");

    // Registers pipensx's own design tokens (its src/ui/theme.hpp values)
    // as Borealis theme/style entries, so every stock Borealis widget picks
    // them up without per-widget styling.
    freeshop::registerTheme();
    boot_log("after registerTheme");

    // Without this the console dims on its own inactivity timer regardless
    // of what's happening - including mid-download, where there's no
    // controller input for however long the transfer takes.
    appletSetAutoSleepDisabled(true);

    // No romfsInit/romfsExit here on purpose - Borealis's switch_wrapper
    // already mounted romfs before main() ran, and unmounting it from here
    // would pull its font and XML layouts out from under it.
    boot_log("before pushActivity");
    brls::Application::pushActivity(new freeshop::SmokeActivity());
    boot_log("entering mainLoop");

    while (brls::Application::mainLoop())
        ;

    boot_log("clean exit");
    return EXIT_SUCCESS;
}
