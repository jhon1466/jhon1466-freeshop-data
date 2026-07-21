#include "ui_list.h"

#include <switch.h>
#include <stdio.h>

#define VISIBLE_ROWS 15

int ui_show_list(const AppEntry *entries, int count) {
    int selected = 0;
    int scroll_offset = 0;

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Down) {
            if (selected < count - 1) selected++;
        }
        if (kDown & HidNpadButton_Up) {
            if (selected > 0) selected--;
        }
        if (kDown & HidNpadButton_A) {
            if (count > 0) return selected;
        }
        if ((kDown & HidNpadButton_B) || (kDown & HidNpadButton_Plus)) {
            return -1;
        }

        if (selected < scroll_offset) scroll_offset = selected;
        if (selected >= scroll_offset + VISIBLE_ROWS) scroll_offset = selected - VISIBLE_ROWS + 1;

        consoleClear();
        printf("FreeShop - Homebrew Catalog\n");
        printf("D-Pad: navigate   A: select   B/+: exit\n\n");

        if (count == 0) {
            printf("  (catalog is empty)\n");
        }

        for (int i = scroll_offset; i < count && i < scroll_offset + VISIBLE_ROWS; i++) {
            const char *cursor = (i == selected) ? ">" : " ";
            printf("%s %-40.40s [%s]\n", cursor, entries[i].title, entries[i].category);
        }

        consoleUpdate(NULL);
    }

    return -1;
}
