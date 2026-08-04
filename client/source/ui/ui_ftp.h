#pragma once

// FTP server screen (see ../ftp/ftp_server.h) - shows the address to type
// into a PC's FTP client ("ftp://<ip>:5000"), connection status, and while a
// file is actively being transferred/installed, a progress bar (B cancels).
// B when idle backs out and shuts the server down. Blocks for as long as
// the screen is open - the whole point is to sit here while the user's PC
// talks to it.
void ui_show_ftp(void);
