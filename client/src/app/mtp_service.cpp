#include "mtp_service.hpp"

#include <chrono>
#include <thread>

extern "C" {
#include "core/util.h"
}

namespace pipensx {

namespace {
// Sample no more than ~5x/sec - smooths out per-USB-chunk jitter (a chunk
// lands every few ms while the transport is fast) without the reading
// lagging noticeably behind reality.
constexpr uint64_t kSpeedSampleIntervalMs = 200;

// mtp_step() only throttles itself by blocking inside mtp_usb_read() - and
// only once a session is actually open enough to attempt one. Before the
// cable is even connected, mtp_usb_is_connected() is false and mtp_step()
// returns immediately every time, with nothing else in this loop to slow
// it down. That turns this thread into an unthrottled busy-spin (one core
// pegged at 100%, plus constant mutex_ contention with the UI thread's
// snapshot() polling) the instant the MTP tab opens without a cable - which
// starves the rest of the app of CPU and reads as the whole console
// freezing, not just this screen. Sleeping here whenever mtp_step() didn't
// itself block matches the same ~200ms cadence its own read timeout uses.
constexpr auto kIdlePollInterval = std::chrono::milliseconds(200);
} // namespace

MtpService::MtpService(std::string workingRoot)
    : workingRoot_(std::move(workingRoot)) {}

MtpService::~MtpService() {
    stop();
}

bool MtpService::start(install::InstallStorageTarget target, std::string& error) {
    if (running_.load())
        return true;
    if (!mtp::mtp_start(workingRoot_, target, error))
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = MtpSnapshot{};
    }
    stopping_.store(false);
    running_.store(true);
    thread_ = std::thread([this] { threadMain(); });
    return true;
}

void MtpService::stop() {
    if (!running_.load())
        return;
    stopping_.store(true);
    if (thread_.joinable())
        thread_.join();
    mtp::mtp_stop();
    running_.store(false);
}

void MtpService::updateSpeedLocked(uint64_t nowBytes) {
    const uint64_t nowMs = now_ms();
    if (speedBaseAtMs_ == 0 || nowBytes < speedBaseBytes_) {
        // First sample of a transfer, or the byte count went backwards (a
        // new object started under the same still-open progress callback -
        // shouldn't happen, but resetting is cheap and correct either way).
        speedBaseBytes_ = nowBytes;
        speedBaseAtMs_ = nowMs;
        return;
    }
    const uint64_t elapsedMs = nowMs - speedBaseAtMs_;
    if (elapsedMs < kSpeedSampleIntervalMs)
        return;
    const uint64_t deltaBytes = nowBytes - speedBaseBytes_;
    const uint64_t sample = deltaBytes * 1000 / elapsedMs;
    if (snapshot_.speedBytesPerSecond) {
        const uint64_t previous = snapshot_.speedBytesPerSecond;
        snapshot_.speedBytesPerSecond = sample >= previous
            ? previous + (sample - previous) * 3 / 10
            : previous - (previous - sample) * 3 / 10;
    } else {
        snapshot_.speedBytesPerSecond = sample;
    }
    speedBaseBytes_ = nowBytes;
    speedBaseAtMs_ = nowMs;
}

void MtpService::threadMain() {
    mtp::MtpState state;
    while (!stopping_.load(std::memory_order_relaxed)) {
        mtp::MtpProgressCallback progressCb = [this](uint64_t total, uint64_t now) {
            std::lock_guard<std::mutex> lock(mutex_);
            updateSpeedLocked(now);
            snapshot_.progressTotal = total;
            snapshot_.progressNow = now;
            return !stopping_.load(std::memory_order_relaxed);
        };
        mtp::mtp_step(state, progressCb);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.status = state.status;
            snapshot_.currentFile = state.currentFile;
            snapshot_.history = state.history;
            if (state.currentFile.empty()) {
                snapshot_.progressTotal = 0;
                snapshot_.progressNow = 0;
                snapshot_.speedBytesPerSecond = 0;
                speedBaseAtMs_ = 0;
            }
        }

        // Only WaitingForUsb ever returns without mtp_step() having blocked
        // on its own read timeout - every other status either just did (the
        // idle/waiting-for-host poll) or is mid-transfer (no throttling
        // wanted there at all).
        if (state.status == mtp::MtpStatus::WaitingForUsb)
            std::this_thread::sleep_for(kIdlePollInterval);
    }
}

MtpSnapshot MtpService::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

} // namespace pipensx
