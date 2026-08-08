#pragma once

#include "install/install_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pipensx::mtp {

// PTP/MTP protocol responder, built on mtp_usb.h's transport. Presents one
// storage with an always-empty root (no file browsing - see mtp_step's
// design note on why) that accepts drops: a host (Windows Explorer, or any
// other MTP client) opening a session and sending an object (SendObjectInfo/
// SendObjectPropList + SendObject, or the Android BeginEditObject/
// SendPartialObject/.../EndEditObject sequence for anything over 4GB)
// installs a .nsp/.nsz/.xci/.xcz straight from the transfer via
// install::PackageStream + install::InstallBackend (the same streaming
// pipeline torrent installs use for PFS0; PackageStream also understands
// XCI/XCZ's nested HFS0 partitions), without ever staging a copy on the SD
// card; anything else is staged to
// sdmc:/switch/freeshop-client/mtp_incoming/ and left there, uninstalled,
// with a message explaining why.
//
// A host dragging several files sends them the same way it always has -
// one complete object at a time, over the same session - so this already
// installs them one after another with no extra work; MtpState::history is
// just this responder's own record of that, for a caller to show as a
// queue-style list.
enum class MtpStatus {
    WaitingForUsb,  // cable not connected / USB not configured yet
    WaitingForHost, // USB connected, no PTP session open yet
    Idle,           // session open, nothing in flight
    Installing,     // transfer done, committing it (blocks a few seconds, no USB serviced)
};

enum class MtpHistoryStatus {
    Installed,
    Failed,
};

// One finished (installed or failed) transfer this session. MTP never tells
// a responder how many objects a drag-and-drop batch actually contains -
// each one only becomes known as its own SendObjectInfo arrives - so this
// is filled in incrementally, one entry per completion.
struct MtpHistoryItem {
    std::string filename;
    MtpHistoryStatus status = MtpHistoryStatus::Installed;
    std::string error; // only meaningful when status == Failed
};

// How many recent completions mtp_step remembers - well past anything a
// real drag-and-drop batch is likely to contain; once full, the oldest
// entry is dropped to make room, so this stays a record of what's *recent*
// rather than a hard cap on how many files a session can process.
inline constexpr size_t kMtpHistoryMax = 32;

struct MtpState {
    MtpStatus status = MtpStatus::WaitingForUsb;
    std::string currentFile; // file being received/installed right now, empty when idle
    std::vector<MtpHistoryItem> history; // oldest first, capped at kMtpHistoryMax
};

// (total, now) -> false cancels the in-flight receive/install. `total` is 0
// when the total size genuinely isn't known yet (a >4GB object whose host
// never stated a size - see receive_data_phase_unbounded in mtp_ptp.cpp).
using MtpProgressCallback = std::function<bool(uint64_t total, uint64_t now)>;

// Brings up the USB transport and remembers where/how a received .nsp/.nsz
// should install. Returns false with a reason in `error` if that fails -
// see mtp_usb.h.
bool mtp_start(std::string workingRoot, install::InstallStorageTarget target,
               std::string& error);
void mtp_stop();

// Services USB/PTP traffic. Most calls are a short (~200ms) non-blocking
// poll - meant to be called in a loop on a dedicated worker thread, never
// the UI thread: the one exception is that once a host actually starts
// sending a file (SendObject), this blocks for however long that transfer
// (and, if it's a .nsp/.nsz, the install after it) takes. `progressCb` is
// called periodically during both so the caller can publish progress and
// check for a cancel; returning false from it aborts the in-flight
// receive/install. May be empty to just block silently. `state` is updated
// with the latest status/current file/history on every call.
void mtp_step(MtpState& state, const MtpProgressCallback& progressCb);

} // namespace pipensx::mtp
