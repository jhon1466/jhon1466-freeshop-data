#include "ui_about.h"
#include "ui_app.h"
#include "ui_icons.h"
#include "../config.h"

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

#define QR_SIZE 220
#define QR_X (LEFT_EDGE + 40)
#define QR_Y (PANEL_TOP + 60)

#define DONATE_TEXT_X (QR_X + QR_SIZE + 50)
#define DONATE_TEXT_W (RIGHT_EDGE - 40 - DONATE_TEXT_X)

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
        if ((kDown & HidNpadButton_B) || (kDown & HidNpadButton_Plus)) break;

        SDL_SetRenderDrawColor(g_renderer, COLOR_BG.r, COLOR_BG.g, COLOR_BG.b, COLOR_BG.a);
        SDL_RenderClear(g_renderer);

        ui_draw_text(g_font_title, LEFT_EDGE, HEADER_Y, COLOR_TEXT, "FreeShop");
        ui_draw_text(g_font_body, 210, HEADER_Y + 4, COLOR_TEXT_DIM, "- Acerca de");

        ui_draw_rect(LEFT_EDGE, PANEL_TOP, RIGHT_EDGE - LEFT_EDGE, PANEL_BOTTOM - PANEL_TOP, COLOR_PANEL);

        int y = PANEL_TOP + 24;
        char version_line[64];
        snprintf(version_line, sizeof(version_line), "Versión %s", CLIENT_VERSION);
        ui_draw_text(g_font_body, LEFT_EDGE + 40, y, COLOR_TEXT, version_line);
        y += 34;
        ui_draw_text_wrapped(g_font_small, LEFT_EDGE + 40, y, RIGHT_EDGE - LEFT_EDGE - 80, 22, COLOR_TEXT_DIM,
                              "Catálogo de homebrew para Nintendo Switch - lista, descarga e instala "
                              "juegos, ports, DLC/actualizaciones y updates de la propia app, todo desde "
                              "aquí mismo.");

        // Donations
        ui_draw_rect(QR_X, QR_Y, QR_SIZE, QR_SIZE, COLOR_BG);
        if (qr) {
            SDL_Rect dst = { QR_X, QR_Y, QR_SIZE, QR_SIZE };
            SDL_RenderCopy(g_renderer, qr, NULL, &dst);
        }

        int ty = QR_Y - 6;
        ui_draw_text(g_font_body, DONATE_TEXT_X, ty, COLOR_TEXT, "¿Te sirvió FreeShop?");
        ty += 34;
        ty = ui_draw_text_wrapped(g_font_small, DONATE_TEXT_X, ty, DONATE_TEXT_W, 24, COLOR_TEXT_DIM,
                                   "Este proyecto lo mantengo en mi tiempo libre, sin nada a cambio - si te "
                                   "ha gustado y quieres ayudarme a seguir dedicándole horas (y café), "
                                   "cualquier aporte por PayPal se agradece muchísimo. No es obligatorio, "
                                   "pero sí que se siente. ¡Gracias por usar la app!");
        ty += 14;
        ui_draw_text(g_font_small, DONATE_TEXT_X, ty, COLOR_ACCENT, "Escanea el QR o dona por PayPal a:");
        ty += 28;
        ui_draw_text(g_font_body, DONATE_TEXT_X, ty, COLOR_TEXT, "mastergarden1112@gmail.com");

        ui_draw_rect(LEFT_EDGE, FOOTER_Y - 10, RIGHT_EDGE - LEFT_EDGE, 1, COLOR_PANEL);
        ui_draw_text(g_font_small, LEFT_EDGE, FOOTER_Y, COLOR_TEXT_DIM, "B/+: volver");

        SDL_RenderPresent(g_renderer);
    }

    if (qr) SDL_DestroyTexture(qr);
}
