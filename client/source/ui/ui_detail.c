#include "ui_detail.h"

#include <switch.h>
#include <stdio.h>

UiDetailAction ui_show_detail(const AppEntry *entry) {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_A) {
            return UI_DETAIL_INSTALL;
        }
        if (kDown & HidNpadButton_B) {
            return UI_DETAIL_BACK;
        }

        consoleClear();
        printf("%s\n", entry->title);
        printf("by %s - v%s\n\n", entry->author, entry->version);
        printf("%s\n\n", entry->description);
        if (entry->long_description[0] != '\0') {
            printf("%s\n\n", entry->long_description);
        }
        printf("Size: %.2f MB\n\n", entry->file_size / (1024.0 * 1024.0));
        printf("A: install   B: back\n");

        consoleUpdate(NULL);
    }

    return UI_DETAIL_BACK;
}
