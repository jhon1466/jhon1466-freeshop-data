#pragma once

namespace pipensx {

class SwitchPerformanceController {
public:
    SwitchPerformanceController() = default;
    ~SwitchPerformanceController();

    SwitchPerformanceController(const SwitchPerformanceController&) = delete;
    SwitchPerformanceController& operator=(
        const SwitchPerformanceController&) = delete;

    // CPU boost mode: only worth the extra heat/battery while a transfer is
    // actually moving bytes, so this stays tied to hasActiveTransfer().
    void setCpuBoostActive(bool active);

    // Auto-sleep/screen-off: set once for the whole run. A download or
    // install left mid-transfer when the console sleeps is exactly the
    // failure this exists to prevent, and there is no reliable moment to
    // safely re-enable it while the app might still be moving bytes in the
    // background (a paused task can be resumed, a queued one can start).
    void setKeepAwake(bool enabled);

private:
    void applyCpuBoost();
    void revertCpuBoost();
    void applyKeepAwake();
    void revertKeepAwake();

    bool cpuBoostActive_ = false;
    bool cpuBoostApplied_ = false;
    bool keepAwakeEnabled_ = false;
    bool autoSleepChanged_ = false;
    bool previousAutoSleepDisabled_ = false;
};

} // namespace pipensx
