#pragma once

#include "install/install_backend.hpp"
#include "mtp/mtp_ptp.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pipensx {

// Snapshot of the MTP responder's state, published by MtpService's worker
// thread and polled by the UI (brls::RepeatingTimer). Never call into
// mtp::mtp_step() from the UI thread directly - it blocks for as long as an
// actual USB file transfer takes, which would freeze the whole app.
struct MtpSnapshot {
    mtp::MtpStatus status = mtp::MtpStatus::WaitingForUsb;
    std::string currentFile;
    std::vector<mtp::MtpHistoryItem> history;
    // Only meaningful while currentFile is non-empty (a receive/install is
    // in flight). total == 0 means the total size isn't known yet.
    uint64_t progressTotal = 0;
    uint64_t progressNow = 0;
    // Smoothed transfer rate, 0 until enough samples have landed for a
    // reading (or once the transfer stops). See MtpService's own doc
    // comment on the sampling/smoothing used - same shape as
    // DownloadManager's install-speed tracking.
    uint64_t speedBytesPerSecond = 0;
};

// Owns the MTP USB responder's lifecycle and worker thread. start()/stop()
// are UI-thread calls (matching the app's other services); the worker
// thread is the only thing that ever touches mtp::mtp_step()/mtp_ptp.cpp's
// internal state.
class MtpService {
public:
    explicit MtpService(std::string workingRoot);
    ~MtpService();

    MtpService(const MtpService&) = delete;
    MtpService& operator=(const MtpService&) = delete;

    // Brings up the USB transport synchronously (so a failure - e.g. usb:ds
    // already claimed by another sysmodule - can be reported immediately)
    // then starts the worker thread that services it. Safe to call again
    // while already running (no-op, returns true).
    bool start(install::InstallStorageTarget target, std::string& error);
    // Stops the worker thread (joining it - if a transfer is in flight,
    // this waits for its current chunk read to time out and unwind) then
    // tears down the USB transport. Safe to call when not running.
    void stop();
    bool running() const { return running_.load(); }

    MtpSnapshot snapshot() const;

private:
    void threadMain();
    // Updates snapshot_.speedBytesPerSecond from how far `nowBytes` has
    // moved since the last sample, throttled to a fixed sampling interval
    // and exponentially smoothed - the same shape DownloadManager uses for
    // its own install-speed reading (see download_manager.cpp's
    // updateInstallSpeed), just local to this one in-flight transfer
    // instead of per-task. Caller holds mutex_.
    void updateSpeedLocked(uint64_t nowBytes);

    std::string workingRoot_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    MtpSnapshot snapshot_;
    uint64_t speedBaseBytes_ = 0;
    uint64_t speedBaseAtMs_ = 0;
};

} // namespace pipensx
