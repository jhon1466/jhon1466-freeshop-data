#pragma once
#include <switch.h> // u16/u32
#include "../install/install.h" // InstallProgressCallback - reused as-is, see mtp_step's doc comment
#include <stdbool.h>
#include <stddef.h>

// PTP/MTP protocol responder, built on mtp_usb.h's transport. Presents one
// storage with an always-empty root (no file browsing - see mtp_step's
// design note on why) that accepts drops: a host (Windows Explorer, or any
// other MTP client) opening a session and sending an object (SendObjectInfo/
// SendObjectPropList + SendObject, or the Android BeginEditObject/
// SendPartialObject/.../EndEditObject sequence for anything over 4GB)
// installs .nsp/.nsz/.xci/.xcz straight from the transfer via
// install_stream.h, without ever staging a copy on the SD card; anything
// else is staged to sdmc:/switch/freeshop/mtp_incoming/ and left there,
// uninstalled, with a message explaining why.
//
// A host dragging several files sends them the same way it always has -
// one complete object at a time, over the same session - so this already
// installs them one after another with no extra work; MTP_STATE_HISTORY is
// just this responder's own record of that, for a caller to show as a
// queue-style list (ui_mtp.c does).
typedef enum {
    MTP_STATUS_WAITING_FOR_USB,  // cable not connected / USB not configured yet
    MTP_STATUS_WAITING_FOR_HOST, // USB connected, no PTP session open yet
    MTP_STATUS_IDLE,             // session open, nothing in flight
    MTP_STATUS_INSTALLING,       // transfer done, committing it (blocks a few seconds, no USB serviced)
} MtpStatus;

typedef enum {
    MTP_HISTORY_INSTALLED,
    MTP_HISTORY_FAILED,
} MtpHistoryStatus;

// One finished (installed or failed) transfer this session. MTP never tells
// a responder how many objects a drag-and-drop batch actually contains -
// each one only becomes known as its own SendObjectInfo arrives - so this
// is filled in incrementally, one entry per completion, rather than
// pre-populated like a download queue's.
typedef struct {
    char filename[256];
    MtpHistoryStatus status;
    char error[220]; // only meaningful when status == MTP_HISTORY_FAILED
} MtpHistoryItem;

// How many recent completions mtp_step remembers - well past anything a
// real drag-and-drop batch is likely to contain; once full, the oldest
// entry is dropped to make room, so this stays a record of what's *recent*
// rather than a hard cap on how many files a session can process.
#define MTP_HISTORY_MAX 32

typedef struct {
    MtpStatus status;
    char current_file[256]; // file being received/installed right now, "" when idle

    MtpHistoryItem history[MTP_HISTORY_MAX];
    int history_count; // <= MTP_HISTORY_MAX; index 0 is the oldest still remembered
} MtpState;

// Brings up the USB transport. Returns false with a reason in err_buf if
// that fails - see mtp_usb.h.
bool mtp_start(char *err_buf, size_t err_buf_size);
void mtp_stop(void);

// Services USB/PTP traffic. Most calls are a short (~200ms) non-blocking
// poll - safe to call once per rendered frame, matching every other
// screen's input-then-render loop. The one exception: once a host actually
// starts sending a file (SendObject), this blocks for however long that
// takes (and, if it's a .nsp/.xci, the install after it) - `progress_cb`
// (same shape/throttling contract as install.h's InstallProgressCallback,
// reused directly rather than a near-duplicate typedef, since it's passed
// straight through to install_nsp_from_local_file/install_xci_from_local_file
// unchanged) is called periodically during both so the caller can redraw
// and check for a cancel button, exactly like every install screen already
// does - `total`/`now` are the current file's expected/received bytes
// during the USB receive, or whatever install.h's own installers report
// during the install phase after it. May be NULL to just block silently.
// `out` is updated with the latest status/last-installed-or-errored file
// name on every call.
void mtp_step(MtpState *out, InstallProgressCallback progress_cb, void *userdata);
