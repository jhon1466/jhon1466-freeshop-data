#pragma once
#include "../install/install.h" // InstallProgressCallback - reused as-is, same contract as mtp_ptp.h's
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A single-client FTP server exposing the whole SD card (root "/" maps to
// sdmc:/), for a PC's own FTP client (FileZilla, WinSCP, Windows Explorer's
// ftp:// support) over the local network - no cable, no companion app.
// Standard file management (browse, upload, download, delete, rename,
// create/remove folders) works against real files, exactly like any other
// homebrew FTP server; the one deliberate difference is that uploading an
// .nsp/.nsz/.xci/.xcz installs it straight from the transfer via
// install_stream.h, the same as mtp_ptp.c's responder does - no staging
// copy on the SD, no separate install pass after.
//
// PASV (passive) data connections only - a client offering only active/PORT
// mode will get a clean "not supported" rather than a connection that never
// works. Every modern client (FileZilla, WinSCP, Windows' own ftp client)
// either defaults to PASV or falls back to it automatically.
//
// One client at a time, matching mtp_ptp.c's own single-active-transfer
// design - a second control connection just waits (unaccepted) until the
// first disconnects. FileZilla's default of several simultaneous transfer
// connections will make its 2nd+ queued transfer look stuck for that
// reason; setting its "maximum simultaneous transfers" to 1 avoids it.
typedef enum {
    FTP_STATUS_WAITING_FOR_NETWORK, // no local IP yet - not connected to Wi-Fi/Ethernet
    FTP_STATUS_LISTENING,           // ready, no client connected
    FTP_STATUS_CONNECTED,           // a client is connected, no transfer in flight
    FTP_STATUS_TRANSFERRING,        // sending/receiving one file
    FTP_STATUS_INSTALLING,          // transfer done, committing it (blocks briefly, no control traffic serviced)
} FtpStatus;

typedef enum {
    FTP_HISTORY_INSTALLED,
    FTP_HISTORY_UPLOADED,   // STOR of something other than an installable format - written to the SD as-is
    FTP_HISTORY_DOWNLOADED, // RETR completed
    FTP_HISTORY_FAILED,
} FtpHistoryStatus;

// One finished transfer this session - see MtpHistoryItem in mtp_ptp.h,
// which this mirrors.
typedef struct {
    char filename[256];
    FtpHistoryStatus status;
    char error[220]; // only meaningful when status == FTP_HISTORY_FAILED
} FtpHistoryItem;

#define FTP_HISTORY_MAX 32

typedef struct {
    FtpStatus status;
    char local_ip[24];       // "192.168.1.5" - what the user types into their FTP client, "" until known
    char client_ip[48];      // connected client's address, "" when none is
    char current_file[256];  // file being transferred/installed right now, "" otherwise

    FtpHistoryItem history[FTP_HISTORY_MAX];
    int history_count;
} FtpState;

// Brings up networking (nifm for the local IP, a listening control socket
// on FTP_SERVER_PORT). Returns false with a reason in err_buf on failure.
#define FTP_SERVER_PORT 5000
bool ftp_start(char *err_buf, size_t err_buf_size);
void ftp_stop(void);

// Services one control connection's worth of FTP traffic. Safe to call once
// per rendered frame like mtp_step - most calls are a short non-blocking
// poll (accepting a new connection, or checking for a command line), except
// while a data transfer (LIST/RETR/STOR) is actually in flight, which blocks
// for however long that takes. `progress_cb`/`userdata` have the exact same
// contract as mtp_step's own (see mtp_ptp.h) - called periodically during
// both the transfer and, for an installable upload, the commit phase after,
// so the caller can redraw and check for a cancel button. `out` is updated
// with the latest status/history on every call.
void ftp_step(FtpState *out, InstallProgressCallback progress_cb, void *userdata);
