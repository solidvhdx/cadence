#pragma once

#include "core/Types.hpp"
#include "core/Units.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace cadence {

class PerformanceMonitor {
public:
    PerformanceMonitor();

    // Called from worker thread on each real dispatched click / key press (lock-free, zero alloc)
    // targetInterval may be zero when no target applies (e.g. clicks inside a burst)
    void RecordEvent(int64_t timestampQpc, Millis targetInterval, Millis actualInterval) noexcept;

    // Resets session counters and history (e.g. on mode switch or new session)
    void ResetSession() noexcept;

    // Samples real Windows process CPU% and RAM (MB) + computes real CPS/KPS, Jitter, P99
    TelemetrySnapshot ComputeSnapshot(int64_t nowQpc, int64_t qpcFreq, bool isRunning) noexcept;

    [[nodiscard]] uint64_t SessionCount() const noexcept {
        return m_sessionCount.load(std::memory_order_relaxed);
    }

private:
    struct Sample {
        int64_t timestampQpc     = 0;
        double  targetIntervalMs = 0.0;
        double  actualIntervalMs = 0.0;
    };

    static constexpr size_t kRingSize = 1024;
    static constexpr size_t kRingMask = kRingSize - 1;

    std::array<Sample, kRingSize> m_ring{};
    std::atomic<uint64_t>         m_writeIdx{0};
    std::atomic<uint64_t>         m_sessionBaseIdx{0};
    std::atomic<uint64_t>         m_sessionCount{0};
    std::atomic<bool>             m_clearHistory{false};

    // UI-thread-only rolling history & CPU delta tracking
    std::array<float, 40> m_history{};
    size_t                m_historyCount = 0;

    uint64_t m_prevProcTime100ns = 0;
    uint64_t m_prevWallTime100ns = 0;
    uint32_t m_numLogicalCpus    = 1;
    double   m_lastCpuPercent    = 0.0;
    double   m_lastMemoryMb      = 0.0;
};

} // namespace cadence
