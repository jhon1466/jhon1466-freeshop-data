#pragma once

// Native MTP responder screen (see ../mtp/mtp_ptp.h) - shows connection
// status ("esperando cable" / "esperando conexión" / "listo"), and while a
// file is actively being received/installed, a progress bar (B cancels).
// B when idle backs out and tears down the USB responder. Blocks for as
// long as the screen is open - the whole point is to sit here while the
// user drags files onto the console from a PC.
void ui_show_mtp(void);
