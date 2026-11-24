#pragma once

#include "core/Units.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace cadence {

class TimingEngine {
public:
    TimingEngine();
    ~TimingEngine();

    TimingEngine(const TimingEngine&)            = delete;
    TimingEngine& operator=(const TimingEngine&) = delete;

    // Windows 11 Power Throttling override (PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION)
    static bool SetPowerThrottlingDisabled(bool disable) noexcept;

    // Raw QueryPerformanceCounter ticks and their conversions (the only place that knows the frequency)
    [[nodiscard]] int64_t NowQpc() const noexcept;
    [[nodiscard]] int64_t QpcFrequency() const noexcept { return m_qpcFreq; }
    [[nodiscard]] int64_t ToQpcTicks(Micros duration) const noexcept;
    [[nodiscard]] Millis  QpcToMillis(int64_t deltaQpc) const noexcept;
    [[nodiscard]] Micros  QpcToMicros(int64_t deltaQpc) const noexcept;

    // Interruptible precision wait until targetQpc.
    // Returns true if target reached, false if cancelled via abortFlag or cancelEvent.
    bool WaitUntil(int64_t targetQpc,
                   HANDLE cancelEvent,
                   const std::atomic<bool>& abortFlag,
                   bool hybridSpin) const noexcept;

    // Convenience wait for a relative duration.
    bool WaitFor(Millis duration,
                 HANDLE cancelEvent,
                 const std::atomic<bool>& abortFlag,
                 bool hybridSpin) const noexcept;

    // Clamped Gaussian jitter within [-maxJitter, +maxJitter] (sigma = maxJitter / 2.5)
    [[nodiscard]] Millis SampleJitter(Millis maxJitter) noexcept;

    // Measures actual Win11 waitable timer overshoot (~10ms total) and tunes spin guard dynamically
    void CalibrateSpinGuard() noexcept;

    void SetSpinGuard(Micros guard) noexcept { m_spinGuard = guard; }
    [[nodiscard]] Micros SpinGuard() const noexcept { return m_spinGuard; }
    [[nodiscard]] bool IsHighResTimerAvailable() const noexcept { return m_highResSupported; }

private:
    uint64_t NextRandomU64() noexcept;
    double   NextUniform01() noexcept;

    HANDLE  m_timer            = nullptr;
    int64_t m_qpcFreq          = 10'000'000;
    Micros  m_spinGuard{450.0}; // Default spin guard tuned for Windows 11 high-res timer
    bool    m_highResSupported = false;

    // xoshiro256++ PRNG state (zero heap allocation, ultra-fast)
    uint64_t m_rng[4]{};
};

} // namespace cadence
