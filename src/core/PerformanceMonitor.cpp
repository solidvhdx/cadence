#include "core/PerformanceMonitor.hpp"

#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace cadence {

namespace {

uint64_t FileTimeToU64(const FILETIME& ft) noexcept {
    ULARGE_INTEGER ui{};
    ui.LowPart  = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return ui.QuadPart;
}

} // namespace

PerformanceMonitor::PerformanceMonitor() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    m_numLogicalCpus = std::max<uint32_t>(1u, si.dwNumberOfProcessors);

    FILETIME creation{}, exit{}, kernel{}, user{}, now{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        m_prevProcTime100ns = FileTimeToU64(kernel) + FileTimeToU64(user);
    }
    GetSystemTimeAsFileTime(&now);
    m_prevWallTime100ns = FileTimeToU64(now);
}

void PerformanceMonitor::RecordEvent(int64_t timestampQpc,
                                     double targetIntervalMs,
                                     double actualIntervalMs) noexcept {
    const uint64_t idx = m_writeIdx.load(std::memory_order_relaxed);
    m_ring[idx & kRingMask] = Sample{timestampQpc, targetIntervalMs, actualIntervalMs};
    m_writeIdx.store(idx + 1, std::memory_order_release);
    m_sessionCount.fetch_add(1, std::memory_order_relaxed);
}

void PerformanceMonitor::ResetSession() noexcept {
    // Never mutate m_writeIdx from outside the worker thread; advance session base lock-free instead
    const uint64_t curWrite = m_writeIdx.load(std::memory_order_acquire);
    m_sessionBaseIdx.store(curWrite, std::memory_order_release);
    m_sessionCount.store(0, std::memory_order_relaxed);
    m_clearHistory.store(true, std::memory_order_release);
}

TelemetrySnapshot PerformanceMonitor::ComputeSnapshot(int64_t nowQpc,
                                                      int64_t qpcFreq,
                                                      bool isRunning) noexcept {
    TelemetrySnapshot snap{};
    snap.sessionCount = m_sessionCount.load(std::memory_order_relaxed);

    // 1. Measure real process CPU % and Working Set RAM (throttled to >= 250ms delta for accuracy)
    FILETIME creation{}, exit{}, kernel{}, user{}, nowFt{};
    GetSystemTimeAsFileTime(&nowFt);
    const uint64_t wallNow = FileTimeToU64(nowFt);
    const uint64_t wallDelta = wallNow - m_prevWallTime100ns;

    if (wallDelta >= 2'500'000ULL) { // >= 250 ms
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
            const uint64_t procNow   = FileTimeToU64(kernel) + FileTimeToU64(user);
            const uint64_t procDelta = procNow - m_prevProcTime100ns;
            const double rawCpu = (static_cast<double>(procDelta) / static_cast<double>(wallDelta)) *
                                  (100.0 / static_cast<double>(m_numLogicalCpus));
            m_lastCpuPercent    = std::clamp(rawCpu, 0.0, 100.0);
            m_prevProcTime100ns = procNow;
            m_prevWallTime100ns = wallNow;
        }

        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                                 sizeof(pmc))) {
            m_lastMemoryMb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        }
    }

    snap.cpuPercent = m_lastCpuPercent;
    snap.memoryMb   = m_lastMemoryMb;

    if (m_clearHistory.exchange(false, std::memory_order_acq_rel)) {
        m_history.fill(0.0f);
        m_historyCount = 0;
    }

    // 2. Analyze recent real QPC timestamps from lock-free ring buffer
    const uint64_t totalWritten = m_writeIdx.load(std::memory_order_acquire);
    const uint64_t sessionBase  = m_sessionBaseIdx.load(std::memory_order_acquire);
    const uint64_t sessionSpan  = (totalWritten > sessionBase) ? (totalWritten - sessionBase) : 0;
    const size_t available = static_cast<size_t>(std::min<uint64_t>(sessionSpan, kRingSize));

    if (available > 0 && qpcFreq > 0) {
        const int64_t window1s = qpcFreq; // 1.0 second in QPC ticks
        uint32_t eventsInLastSec = 0;
        int64_t  newestInWinQpc  = 0;
        int64_t  oldestInWinQpc  = 0;

        // Stack-allocated scratch buffers: 0 heap allocations per telemetry tick
        std::array<double, 200> intervals{};
        std::array<double, 200> absErrors{};
        size_t intervalCount = 0;
        size_t errorCount    = 0;

        for (size_t i = 0; i < available; ++i) {
            const uint64_t idx = totalWritten - 1 - i;
            const Sample s = m_ring[idx & kRingMask];

            if (nowQpc - s.timestampQpc <= window1s) {
                if (eventsInLastSec == 0) newestInWinQpc = s.timestampQpc;
                oldestInWinQpc = s.timestampQpc;
                ++eventsInLastSec;
            }
            if (intervalCount < intervals.size() && s.actualIntervalMs > 0.0) {
                intervals[intervalCount++] = s.actualIntervalMs;
                if (s.targetIntervalMs > 0.0) {
                    absErrors[errorCount++] = std::abs(s.actualIntervalMs - s.targetIntervalMs);
                }
            }
        }

        if (intervalCount > 0) {
            double sum = 0.0;
            for (size_t i = 0; i < intervalCount; ++i) sum += intervals[i];
            const double avg = sum / static_cast<double>(intervalCount);
            snap.avgIntervalMs = avg;

            double sqSum = 0.0;
            for (size_t i = 0; i < intervalCount; ++i) {
                const double d = intervals[i] - avg;
                sqSum += d * d;
            }
            snap.jitterStdDevMs = std::sqrt(sqSum / static_cast<double>(intervalCount));

            if (isRunning && eventsInLastSec > 0) {
                const double rateFromInterval = avg > 0.001 ? (1000.0 / avg) : 0.0;
                if (eventsInLastSec >= 2 && newestInWinQpc > oldestInWinQpc) {
                    const double spanSec = static_cast<double>(newestInWinQpc - oldestInWinQpc) / static_cast<double>(qpcFreq);
                    snap.actualRate = (spanSec > 0.0005)
                        ? (static_cast<double>(eventsInLastSec - 1) / spanSec)
                        : rateFromInterval;
                } else {
                    snap.actualRate = rateFromInterval;
                }
            }
        }

        if (errorCount > 0) {
            std::sort(absErrors.begin(), absErrors.begin() + static_cast<std::ptrdiff_t>(errorCount));
            const size_t p99Idx = std::min(errorCount - 1,
                                           static_cast<size_t>(std::ceil(static_cast<double>(errorCount) * 0.99)) - 1);
            snap.p99ErrorMs = absErrors[p99Idx];
        }
    }

    if (isRunning) {
        if (m_historyCount < m_history.size()) {
            m_history[m_historyCount++] = static_cast<float>(snap.actualRate);
        } else {
            std::move(m_history.begin() + 1, m_history.end(), m_history.begin());
            m_history.back() = static_cast<float>(snap.actualRate);
        }
    }

    snap.history      = m_history;
    snap.historyCount = m_historyCount;
    return snap;
}

} // namespace cadence
