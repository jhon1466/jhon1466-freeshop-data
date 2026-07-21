#include <switch.h>
#include <stdbool.h>
#include <stdio.h>

#include "config.h"
#include "catalog/catalog.h"
#include "ui/ui_list.h"
#include "ui/ui_detail.h"
#include "install/install.h"

static void wait_for_plus_with_message(const char *msg) {
    consoleClear();
    printf("%s\n\nPress + to exit.\n", msg);
    consoleUpdate(NULL);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
}

static void wait_for_a(void) {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_A) break;
        consoleUpdate(NULL);
    }
}

static void install_progress_cb(long total, long now, void *userdata) {
    (void)userdata;
    consoleClear();
    printf("Installing...\n\n");
    if (total > 0) {
        int pct = (int)((now * 100) / total);
        printf("%d%% (%ld / %ld bytes)\n", pct, now, total);
    } else {
        printf("%ld bytes downloaded\n", now);
    }
    consoleUpdate(NULL);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    consoleInit(NULL);

    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        wait_for_plus_with_message("Could not initialize networking.");
        consoleExit(NULL);
        return 1;
    }

    AppEntry *entries = NULL;
    int count = 0;
    char err_buf[256];

    CatalogResult cres = catalog_fetch(CATALOG_BASE_URL, &entries, &count, err_buf, sizeof(err_buf));
    if (cres != CATALOG_OK) {
        char msg[640];
        snprintf(msg, sizeof(msg), "Could not load catalog from\n%s%s:\n%s",
                 CATALOG_BASE_URL, CATALOG_API_PATH, err_buf);
        wait_for_plus_with_message(msg);
        socketExit();
        consoleExit(NULL);
        return 1;
    }

    bool running = true;
    while (running && appletMainLoop()) {
        int selected = ui_show_list(entries, count);
        if (selected < 0) {
            running = false;
            break;
        }

        UiDetailAction action = ui_show_detail(&entries[selected]);
        if (action != UI_DETAIL_INSTALL) {
            continue;
        }

        InstallResult ires = install_app(&entries[selected], CATALOG_BASE_URL,
                                          install_progress_cb, NULL, err_buf, sizeof(err_buf));

        consoleClear();
        if (ires == INSTALL_OK) {
            printf("Installed \"%s\" successfully.\n\nReturn to hbmenu to launch it.\n\nPress A to continue.\n",
                   entries[selected].title);
        } else {
            printf("Install failed: %s\n\nPress A to continue.\n", err_buf);
        }
        consoleUpdate(NULL);
        wait_for_a();
    }

    catalog_free(entries);
    socketExit();
    consoleExit(NULL);
    return 0;
}
