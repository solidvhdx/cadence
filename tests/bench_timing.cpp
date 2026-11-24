#include "core/PerformanceMonitor.hpp"
#include "core/TimingEngine.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <numeric>
#include <vector>

namespace {

struct BenchResult {
    double targetMs   = 0.0;
    double meanMs     = 0.0;
    double stdDevUs   = 0.0;
    double p99ErrUs   = 0.0;
    double maxErrUs   = 0.0;
    double cpuPercent = 0.0;
    double memoryMb   = 0.0;
};

BenchResult RunCase(cadence::TimingEngine& timing,
                    bool disableThrottle,
                    bool hybridSpin,
                    double spinGuardUs,
                    double targetRateHz,
                    int iterations) {
    cadence::TimingEngine::SetPowerThrottlingDisabled(disableThrottle);
    timing.SetSpinGuard(cadence::Micros{spinGuardUs});

    cadence::PerformanceMonitor perf;
    std::atomic<bool> abort{false};
    HANDLE cancelEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    const double targetMs = 1000.0 / targetRateHz;
    const int64_t stepQpc = timing.ToQpcTicks(cadence::Millis{targetMs});

    // Warm-up
    int64_t nextQpc = timing.NowQpc() + stepQpc;
    for (int i = 0; i < 5; ++i) {
        timing.WaitUntil(nextQpc, cancelEv, abort, hybridSpin);
        nextQpc += stepQpc;
    }

    std::vector<double> actualMs;
    std::vector<double> absErrUs;
    actualMs.reserve(iterations);
    absErrUs.reserve(iterations);

    int64_t prevQpc = timing.NowQpc();
    nextQpc = prevQpc + stepQpc;

    for (int i = 0; i < iterations; ++i) {
        timing.WaitUntil(nextQpc, cancelEv, abort, hybridSpin);
        const int64_t now = timing.NowQpc();
        const double dtMs = timing.QpcToMillis(now - prevQpc).count();
        const double errUs = std::abs(dtMs - targetMs) * 1000.0;
        actualMs.push_back(dtMs);
        absErrUs.push_back(errUs);
        perf.RecordEvent(now, cadence::Millis{targetMs}, cadence::Millis{dtMs});
        prevQpc = now;
        nextQpc += stepQpc;
    }

    const auto snap = perf.ComputeSnapshot(timing.NowQpc(), timing.QpcFrequency(), true);
    CloseHandle(cancelEv);

    BenchResult r{};
    r.targetMs = targetMs;
    r.meanMs = std::accumulate(actualMs.begin(), actualMs.end(), 0.0) / static_cast<double>(actualMs.size());

    double sqSum = 0.0;
    for (double v : actualMs) {
        const double d = v - r.meanMs;
        sqSum += d * d;
    }
    r.stdDevUs = std::sqrt(sqSum / static_cast<double>(actualMs.size())) * 1000.0;

    std::sort(absErrUs.begin(), absErrUs.end());
    const size_t p99 = std::min(absErrUs.size() - 1, static_cast<size_t>(std::ceil(absErrUs.size() * 0.99)) - 1);
    r.p99ErrUs   = absErrUs[p99];
    r.maxErrUs   = absErrUs.back();
    r.cpuPercent = snap.cpuPercent;
    r.memoryMb   = snap.memoryMb;
    return r;
}

void PrintResult(const char* label, const BenchResult& r) {
    std::cout << label << "\n"
              << "  Target: " << r.targetMs << " ms | Mean: " << r.meanMs << " ms"
              << " | StdDev: " << r.stdDevUs << " us"
              << " | P99 Err: " << r.p99ErrUs << " us"
              << " | Max Err: " << r.maxErrUs << " us"
              << " | CPU: " << r.cpuPercent << "%"
              << " | RAM: " << r.memoryMb << " MB\n";
}

} // namespace

int main() {
    cadence::TimingEngine timing;
    std::cout << "=== Cadence - Windows 11 Timing Benchmark ===\n";
    std::cout << "High-Res WaitableTimer supported: " << (timing.IsHighResTimerAvailable() ? "YES" : "NO") << "\n";
    std::cout << "QPC Frequency: " << timing.QpcFrequency() << " Hz\n\n";

    // Compare Pure WaitableTimer (Eco) vs Hybrid QPC Spin with varying guard windows at 20 CPS (50ms) and 50 CPS (20ms)
    PrintResult("[20 CPS / 50ms] Pure High-Res WaitableTimer (Eco, 0us spin)",
                RunCase(timing, true, false, 0.0, 20.0, 30));

    PrintResult("[20 CPS / 50ms] Hybrid QPC Spin (guard 450us)",
                RunCase(timing, true, true, 450.0, 20.0, 30));

    PrintResult("[20 CPS / 50ms] Hybrid QPC Spin (guard 750us)",
                RunCase(timing, true, true, 750.0, 20.0, 30));

    PrintResult("[50 CPS / 20ms] Pure High-Res WaitableTimer (Eco, 0us spin)",
                RunCase(timing, true, false, 0.0, 50.0, 60));

    PrintResult("[50 CPS / 20ms] Hybrid QPC Spin (guard 450us)",
                RunCase(timing, true, true, 450.0, 50.0, 60));

    PrintResult("[50 CPS / 20ms] Hybrid QPC Spin (guard 750us)",
                RunCase(timing, true, true, 750.0, 50.0, 60));

    return 0;
}
