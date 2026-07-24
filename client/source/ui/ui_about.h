#pragma once

// Blocks until the user backs out (B or +). Shows the app name/version and
// a donations panel (QR + PayPal email) - the QR is bundled in the .nro via
// RomFS (client/romfs/qr.jpg, see ui_icons_load_local), so it's always
// available regardless of network.
void ui_show_about(void);
