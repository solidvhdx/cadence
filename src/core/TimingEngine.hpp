#pragma once

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

    [[nodiscard]] int64_t NowQpc() const noexcept;
    [[nodiscard]] int64_t QpcFrequency() const noexcept { return m_qpcFreq; }
    [[nodiscard]] int64_t MicrosecondsToQpc(double us) const noexcept;
    [[nodiscard]] double  QpcToMilliseconds(int64_t deltaQpc) const noexcept;
    [[nodiscard]] double  QpcToMicroseconds(int64_t deltaQpc) const noexcept;

    // Interruptible precision wait until targetQpc.
    // Returns true if target reached, false if cancelled via abortFlag or cancelEvent.
    bool WaitUntil(int64_t targetQpc,
                   HANDLE cancelEvent,
                   const std::atomic<bool>& abortFlag,
                   bool hybridSpin) const noexcept;

    // Convenience wait for relative milliseconds.
    bool WaitForMs(double ms,
                   HANDLE cancelEvent,
                   const std::atomic<bool>& abortFlag,
                   bool hybridSpin) const noexcept;

    // Clamped Gaussian jitter in microseconds within [-maxJitterMs, +maxJitterMs]
    double SampleJitterMs(double maxJitterMs) noexcept;

    // Measures actual Win11 waitable timer overshoot (~10ms total) and tunes spin guard dynamically
    void CalibrateSpinGuard() noexcept;

    void SetSpinGuardUs(double us) noexcept { m_spinGuardUs = us; }
    [[nodiscard]] double SpinGuardUs() const noexcept { return m_spinGuardUs; }
    [[nodiscard]] bool IsHighResTimerAvailable() const noexcept { return m_highResSupported; }

private:
    uint64_t NextRandomU64() noexcept;
    double   NextUniform01() noexcept;

    HANDLE  m_timer            = nullptr;
    int64_t m_qpcFreq          = 10'000'000;
    double  m_spinGuardUs      = 450.0; // Default spin guard tuned for Windows 11 high-res timer
    bool    m_highResSupported = false;

    // xoshiro256++ PRNG state (zero heap allocation, ultra-fast)
    uint64_t m_rng[4]{};
};

} // namespace cadence
