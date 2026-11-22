#include "core/TimingEngine.hpp"

#include <immintrin.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace cadence {

namespace {

uint64_t SplitMix64(uint64_t& x) noexcept {
    uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

} // namespace

TimingEngine::TimingEngine() {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    m_qpcFreq = freq.QuadPart > 0 ? freq.QuadPart : 10'000'000;

    m_timer = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS
    );
    if (m_timer) {
        m_highResSupported = true;
    } else {
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        m_highResSupported = false;
    }

    uint64_t seed = static_cast<uint64_t>(NowQpc()) ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
    for (auto& s : m_rng) {
        s = SplitMix64(seed);
    }

    CalibrateSpinGuard();
}

void TimingEngine::CalibrateSpinGuard() noexcept {
    if (!m_timer || !m_highResSupported) {
        m_spinGuardUs = 450.0;
        return;
    }

    // Run 6 short 1ms high-res timer probes (~6-8ms total) to measure real OS overshoot on this CPU/power plan
    LARGE_INTEGER due{};
    due.QuadPart = -10'000; // 1.0 ms in 100ns units

    double maxOvershootUs = 0.0;
    for (int i = 0; i < 6; ++i) {
        const int64_t t0 = NowQpc();
        if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(m_timer, INFINITE);
            const double actualUs = QpcToMicroseconds(NowQpc() - t0);
            if (i >= 1) { // Skip first warm-up sample
                const double overshoot = std::max(0.0, actualUs - 1000.0);
                maxOvershootUs = std::max(maxOvershootUs, overshoot);
            }
        }
    }
    // Keep guard tight enough to avoid CPU waste & quantum preemption, wide enough to absorb overshoot
    m_spinGuardUs = std::clamp(maxOvershootUs + 150.0, 320.0, 620.0);
}

TimingEngine::~TimingEngine() {
    if (m_timer) {
        CloseHandle(m_timer);
        m_timer = nullptr;
    }
}

bool TimingEngine::SetPowerThrottlingDisabled(bool disable) noexcept {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask   = disable ? 0 : PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;

    return SetProcessInformation(
        GetCurrentProcess(),
        ProcessPowerThrottling,
        &state,
        sizeof(state)
    ) != FALSE;
}

int64_t TimingEngine::NowQpc() const noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

int64_t TimingEngine::MicrosecondsToQpc(double us) const noexcept {
    return static_cast<int64_t>((us * static_cast<double>(m_qpcFreq)) / 1'000'000.0);
}

double TimingEngine::QpcToMilliseconds(int64_t deltaQpc) const noexcept {
    return (static_cast<double>(deltaQpc) * 1000.0) / static_cast<double>(m_qpcFreq);
}

double TimingEngine::QpcToMicroseconds(int64_t deltaQpc) const noexcept {
    return (static_cast<double>(deltaQpc) * 1'000'000.0) / static_cast<double>(m_qpcFreq);
}

bool TimingEngine::WaitUntil(int64_t targetQpc,
                             HANDLE cancelEvent,
                             const std::atomic<bool>& abortFlag,
                             bool hybridSpin) const noexcept {
    if (abortFlag.load(std::memory_order_acquire)) {
        return false;
    }

    const int64_t now = NowQpc();
    if (now >= targetQpc) {
        return true;
    }

    const double remainingUs = QpcToMicroseconds(targetQpc - now);
    const double guardUs     = hybridSpin ? m_spinGuardUs : 0.0;

    // 1. Coarse OS wait using High-Resolution Waitable Timer (0% CPU)
    if (m_timer && remainingUs > guardUs + 150.0) {
        const double sleepUs = remainingUs - guardUs;
        // Relative time in 100-nanosecond intervals (negative value)
        LARGE_INTEGER dueTime{};
        dueTime.QuadPart = -static_cast<LONGLONG>(sleepUs * 10.0);

        if (SetWaitableTimer(m_timer, &dueTime, 0, nullptr, nullptr, FALSE)) {
            if (cancelEvent) {
                HANDLE handles[2] = {m_timer, cancelEvent};
                const DWORD res = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (res != WAIT_OBJECT_0 || abortFlag.load(std::memory_order_acquire)) {
                    CancelWaitableTimer(m_timer);
                    return false;
                }
            } else {
                WaitForSingleObject(m_timer, INFINITE);
                if (abortFlag.load(std::memory_order_acquire)) {
                    return false;
                }
            }
        }
    }

    if (!hybridSpin) {
        return !abortFlag.load(std::memory_order_acquire);
    }

    // 2. Bounded sub-millisecond QPC spin with _mm_pause() for microsecond precision
    while (NowQpc() < targetQpc) {
        if (abortFlag.load(std::memory_order_acquire)) {
            return false;
        }
        _mm_pause();
    }

    return !abortFlag.load(std::memory_order_acquire);
}

bool TimingEngine::WaitForMs(double ms,
                             HANDLE cancelEvent,
                             const std::atomic<bool>& abortFlag,
                             bool hybridSpin) const noexcept {
    if (ms <= 0.0) {
        return !abortFlag.load(std::memory_order_acquire);
    }
    const int64_t target = NowQpc() + MicrosecondsToQpc(ms * 1000.0);
    return WaitUntil(target, cancelEvent, abortFlag, hybridSpin);
}

uint64_t TimingEngine::NextRandomU64() noexcept {
    const uint64_t result = std::rotl(m_rng[0] + m_rng[3], 23) + m_rng[0];
    const uint64_t t = m_rng[1] << 17;
    m_rng[2] ^= m_rng[0];
    m_rng[3] ^= m_rng[1];
    m_rng[1] ^= m_rng[2];
    m_rng[0] ^= m_rng[3];
    m_rng[2] ^= t;
    m_rng[3] = std::rotl(m_rng[3], 45);
    return result;
}

double TimingEngine::NextUniform01() noexcept {
    // 53-bit precision uniform in (0, 1]
    return static_cast<double>((NextRandomU64() >> 11) + 1ULL) * (1.0 / 9007199254740992.0);
}

double TimingEngine::SampleJitterMs(double maxJitterMs) noexcept {
    if (maxJitterMs <= 0.0) {
        return 0.0;
    }
    // Box-Muller Gaussian with sigma = maxJitterMs / 2.5, clamped to [-maxJitterMs, +maxJitterMs]
    const double u1 = NextUniform01();
    const double u2 = NextUniform01();
    const double z0 = std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi * u2);
    const double sample = z0 * (maxJitterMs / 2.5);
    return std::clamp(sample, -maxJitterMs, maxJitterMs);
}

} // namespace cadence
