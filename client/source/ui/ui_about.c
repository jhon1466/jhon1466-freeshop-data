#include "ui_about.h"
#include "ui_app.h"
#include "ui_icons.h"
#include "ui_prefs.h"
#include "ui_sound.h"
#include "ui_fx.h"
#include "../config.h"
#include "../i18n.h"

#include <switch.h>
#include <stdio.h>

#define SCREEN_W 1280
#define SCREEN_H 720
#define LEFT_EDGE 20
#define RIGHT_EDGE (SCREEN_W - 20)
#define HEADER_Y 40
#define FOOTER_Y (SCREEN_H - 46)

#define PANEL_TOP 130
#define PANEL_BOTTOM (FOOTER_Y - 20)
#define CONTENT_X (LEFT_EDGE + 40)
#define CONTENT_RIGHT (RIGHT_EDGE - 40)

#define QR_SIZE 220

void ui_show_about(void) {
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    padUpdate(&pad);

    // Loaded once per visit rather than cached across the whole app's
    // lifetime - this screen isn't opened often enough for a decode-once-
    // per-launch texture to be worth the extra global state to track.
    SDL_Texture *qr = ui_icons_load_local("romfs:/qr.jpg");

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        if ((kDown & HidNpadButton_B) || (kDown & HidNpadButton_Plus)) {
            ui_sound_play(UI_SOUND_BACK);
            break;
        }

        // Effects/sound live here rather than on a settings screen of their
        // own - this is the only screen that isn't task-oriented, and both
        // are one-off "set it and forget it" toggles. Persisted through the
        // same prefs file the list screen uses, read-modify-write so
        // toggling one never drops the view/sort/category the user had.
        if (kDown & (HidNpadButton_X | HidNpadButton_Y)) {
            if (kDown & HidNpadButton_X) ui_fx_set_enabled(!ui_fx_enabled());
            if (kDown & HidNpadButton_Y) ui_sound_set_enabled(!ui_sound_enabled());

            UiListPrefs prefs;
            ui_prefs_load(&prefs);
            prefs.effects_disabled = !ui_fx_enabled();
            prefs.sound_disabled = !ui_sound_enabled();
            ui_prefs_save(&prefs);

            // Played after the toggle so turning sound back on is audible
            // immediately (and turning it off makes no noise, as expected).
            ui_sound_play(UI_SOUND_NAVIGATE);
        }

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);
        ui_fx_draw_background();

        ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, "FreeShop");
        ui_draw_text(g_font_body, 210, HEADER_Y + 4, COLOR_TEXT_DIM, tr(STR_ABOUT_HEADER));

        ui_draw_rect(LEFT_EDGE, PANEL_TOP, RIGHT_EDGE - LEFT_EDGE, PANEL_BOTTOM - PANEL_TOP, COLOR_PANEL);

        // Everything below is laid out top-down, each block starting where
        // the previous one actually ended (ui_draw_text_wrapped's return
        // value) rather than at a fixed offset - the description's wrapped
        // line count isn't fixed, so a hardcoded Y for the QR/donations
        // below it either overlapped the description or left a gap,
        // depending on how many lines it happened to wrap to.
        int y = PANEL_TOP + 24;
        char version_line[64];
        snprintf(version_line, sizeof(version_line), tr(STR_ABOUT_VERSION_TEMPLATE), CLIENT_VERSION);
        ui_draw_text(g_font_body, CONTENT_X, y, COLOR_TEXT, version_line);
        y += 34;
        y = ui_draw_text_wrapped(g_font_small, CONTENT_X, y, CONTENT_RIGHT - CONTENT_X, 22, COLOR_TEXT_DIM,
                                  tr(STR_ABOUT_DESCRIPTION));
        y += 36;

        // Donations
        int qr_x = CONTENT_X;
        int qr_y = y;
        ui_draw_rect(qr_x, qr_y, QR_SIZE, QR_SIZE, COLOR_BG);
        if (qr) {
            SDL_Rect dst = { qr_x, qr_y, QR_SIZE, QR_SIZE };
            SDL_RenderCopy(g_renderer, qr, NULL, &dst);
        }

        int donate_x = qr_x + QR_SIZE + 50;
        int donate_w = CONTENT_RIGHT - donate_x;
        int ty = qr_y;
        ui_draw_text(g_font_body, donate_x, ty, COLOR_TEXT, tr(STR_ABOUT_DONATE_QUESTION));
        ty += 34;
        ty = ui_draw_text_wrapped(g_font_small, donate_x, ty, donate_w, 24, COLOR_TEXT_DIM,
                                   tr(STR_ABOUT_DONATE_TEXT));
        ty += 18;
        ui_draw_text(g_font_small, donate_x, ty, COLOR_ACCENT, tr(STR_ABOUT_DONATE_SCAN));
        ty += 28;
        ui_draw_text(g_font_body, donate_x, ty, COLOR_TEXT, "mastergarden1112@gmail.com");

        ui_draw_rect(LEFT_EDGE, FOOTER_Y - 10, RIGHT_EDGE - LEFT_EDGE, 1, COLOR_PANEL);
        char footer[160];
        snprintf(footer, sizeof(footer), tr(STR_ABOUT_FOOTER_TOGGLES),
                 tr(ui_fx_enabled() ? STR_ON : STR_OFF),
                 tr(ui_sound_enabled() ? STR_ON : STR_OFF));
        ui_draw_text(g_font_small, LEFT_EDGE, FOOTER_Y, COLOR_TEXT_DIM, footer);

        SDL_RenderPresent(g_renderer);
    }

    if (qr) SDL_DestroyTexture(qr);
}
