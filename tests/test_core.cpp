#include "core/ConfigManager.hpp"
#include "core/PerformanceMonitor.hpp"
#include "core/ProfileManager.hpp"
#include "core/TimingEngine.hpp"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

#define CADENCE_ASSERT(cond, msg)                                                         \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::cerr << "[FAIL] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

int main() {
    std::cout << "=== Cadence - Core Verification Tests ===\n";

    // 1. Config JSON Serialization & Independent Mode Persistence Round-trip
    {
        cadence::AppConfig orig{};
        orig.activeMode            = cadence::OperatingMode::KPS;
        orig.cps.rateCps           = 17.5;
        orig.cps.button            = cadence::MouseButton::Right;
        orig.cps.pattern           = cadence::ClickPattern::CustomBurst;
        orig.cps.burstCount        = 4;
        orig.cps.cursorMode        = cadence::CursorMode::LockedPoint;
        orig.cps.lockX             = 1234;
        orig.cps.lockY             = 567;
        orig.cps.hotkey            = {cadence::HotkeySource::Mouse, VK_XBUTTON2};

        orig.kps.rateKps           = 22.0;
        orig.kps.targetVk          = 'Q';
        orig.kps.modCtrl           = true;
        orig.kps.modShift          = true;
        orig.kps.inputMode         = cadence::KeyInputMode::Sequence;
        orig.kps.sequenceVks       = {'1', '2', '3', '4', 'E'};
        orig.kps.pressDurationMs   = 45;
        orig.kps.dispatchMethod    = cadence::KeyDispatchMethod::VirtualKey;
        orig.kps.hotkey            = {cadence::HotkeySource::Keyboard, VK_F8};

        orig.settings.stopOnSelfFocus   = true;
        orig.settings.hybridQpcTimer    = true;
        orig.settings.targetWindowLock  = true;
        orig.settings.targetProcessName = "RobloxPlayerBeta.exe";
        orig.cps.lockedMicroWiggle      = false;

        cadence::ProfileManager pm(orig);
        pm.CreateFromCurrent("Unit Test Preset");

        const auto tempPath = std::filesystem::temp_directory_path() / L"cadence_unit_test_config.json";
        CADENCE_ASSERT(cadence::ConfigManager::SaveToFile(tempPath, orig), "Atomic save config to disk");

        cadence::AppConfig loaded{};
        CADENCE_ASSERT(cadence::ConfigManager::LoadFromFile(tempPath, loaded), "Load config from disk");
        std::filesystem::remove(tempPath);

        CADENCE_ASSERT(loaded.activeMode == cadence::OperatingMode::KPS, "Active mode preserved");
        CADENCE_ASSERT(std::abs(loaded.cps.rateCps - 17.5) < 0.01, "CPS rate preserved independently");
        CADENCE_ASSERT(loaded.cps.button == cadence::MouseButton::Right, "CPS mouse button preserved");
        CADENCE_ASSERT(loaded.cps.pattern == cadence::ClickPattern::CustomBurst, "CPS pattern preserved");
        CADENCE_ASSERT(loaded.cps.lockX == 1234 && loaded.cps.lockY == 567, "CPS locked coords preserved");
        CADENCE_ASSERT(!loaded.cps.lockedMicroWiggle, "CPS micro-wiggle flag preserved");
        CADENCE_ASSERT(loaded.cps.hotkey == orig.cps.hotkey, "CPS mouse X2 hotkey preserved");
        CADENCE_ASSERT(std::abs(loaded.kps.rateKps - 22.0) < 0.01, "KPS rate preserved independently");
        CADENCE_ASSERT(loaded.kps.targetVk == 'Q' && loaded.kps.modCtrl && loaded.kps.modShift, "KPS chord preserved");
        CADENCE_ASSERT(loaded.kps.sequenceVks.size() == 5 && loaded.kps.sequenceVks[4] == 'E', "KPS sequence preserved");
        CADENCE_ASSERT(loaded.settings.stopOnSelfFocus, "Self-focus safety setting preserved");
        CADENCE_ASSERT(loaded.settings.targetWindowLock && loaded.settings.targetProcessName == "RobloxPlayerBeta.exe", "Target window lock preserved");
        CADENCE_ASSERT(loaded.profiles.size() == orig.profiles.size(), "All profiles preserved");

        // Verify RFC-8259 \uXXXX Unicode escape decoding
        cadence::Profile unicodeProf{};
        CADENCE_ASSERT(cadence::ConfigManager::DeserializeProfile(R"({"id":"u1","name":"Pr\u00e4zision","mode":0})", unicodeProf), "Unicode JSON parse");
        CADENCE_ASSERT(unicodeProf.name == "Pr\xC3\xA4zision", "Unicode \\u00e4 decoded to UTF-8 'ä'");
        std::cout << "[PASS] ConfigManager atomic JSON persistence, Unicode escapes & mode isolation\n";
    }

    // 2. ProfileManager Mode Switching & Non-Destructive Preservation
    {
        cadence::AppConfig cfg{};
        cfg.cps.rateCps = 19.5;
        cfg.kps.rateKps = 13.0;
        cadence::ProfileManager pm(cfg);

        // Switching to a KPS profile must NOT wipe the user's current CPS rate
        CADENCE_ASSERT(pm.ActivateProfile("skill-spam-e"), "Activate built-in KPS profile");
        CADENCE_ASSERT(cfg.activeMode == cadence::OperatingMode::KPS, "Switched to KPS mode");
        CADENCE_ASSERT(std::abs(cfg.cps.rateCps - 19.5) < 0.01, "CPS config preserved when activating KPS profile");

        // Deleting last profile must be prevented
        while (cfg.profiles.size() > 1) {
            CADENCE_ASSERT(pm.DeleteProfile(cfg.profiles.back().id), "Delete extra profile");
        }
        CADENCE_ASSERT(!pm.DeleteProfile(cfg.profiles.front().id), "Prevent deleting final remaining profile");
        std::cout << "[PASS] ProfileManager activation, isolation, and delete guard\n";
    }

    // 3. TimingEngine Precision, Cancellation & Gaussian Jitter Bounds
    {
        cadence::TimingEngine timing;
        CADENCE_ASSERT(timing.IsHighResTimerAvailable(), "Windows 11 High-Res WaitableTimer available");
        CADENCE_ASSERT(cadence::TimingEngine::SetPowerThrottlingDisabled(true), "Win11 Power Throttling override succeeds");

        // Verify jitter bounds over 2000 samples
        double sum = 0.0;
        for (int i = 0; i < 2000; ++i) {
            const double j = timing.SampleJitterMs(5.0);
            CADENCE_ASSERT(j >= -5.0 && j <= 5.0, "Jitter strictly clamped within [-max, +max]");
            sum += j;
        }
        const double mean = sum / 2000.0;
        CADENCE_ASSERT(std::abs(mean) < 0.4, "Gaussian jitter centered near 0");

        // Verify immediate wait cancellation (must wake in < 10ms even on a 5000ms wait!)
        std::atomic<bool> abort{false};
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const int64_t t0 = timing.NowQpc();
        abort.store(true);
        SetEvent(ev);
        const bool completed = timing.WaitForMs(5000.0, ev, abort, true);
        const double cancelMs = timing.QpcToMilliseconds(timing.NowQpc() - t0);
        CloseHandle(ev);
        CADENCE_ASSERT(!completed, "Cancelled wait returns false");
        CADENCE_ASSERT(cancelMs < 10.0, "Cancellation wakes immediately");

        // Verify real 20ms precision wait within +-2.0ms
        abort.store(false);
        HANDLE ev2 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const int64_t s0 = timing.NowQpc();
        CADENCE_ASSERT(timing.WaitForMs(20.0, ev2, abort, true), "20ms hybrid wait completes");
        const double actual20 = timing.QpcToMilliseconds(timing.NowQpc() - s0);
        CloseHandle(ev2);
        CADENCE_ASSERT(actual20 >= 19.0 && actual20 <= 22.0, "20ms hybrid wait accuracy within tight bounds");
        std::cout << "[PASS] TimingEngine precision (" << actual20 << " ms for 20.0 ms target), jitter & instant cancel\n";
    }

    // 4. PerformanceMonitor Real Rate & Statistics Verification
    {
        cadence::TimingEngine timing;
        cadence::PerformanceMonitor pm;
        const int64_t freq = timing.QpcFrequency();
        int64_t t = timing.NowQpc() - freq; // simulate 1 second of 20 CPS events (50ms each)
        for (int i = 0; i < 20; ++i) {
            t += freq / 20;
            pm.RecordEvent(t, 50.0, 50.0);
        }
        const auto snap = pm.ComputeSnapshot(t, freq, true);
        CADENCE_ASSERT(snap.sessionCount == 20, "Session count matches exact recorded events");
        CADENCE_ASSERT(std::abs(snap.avgIntervalMs - 50.0) < 0.01, "Avg interval exact");
        CADENCE_ASSERT(snap.actualRate >= 19.0 && snap.actualRate <= 21.0, "Measured CPS matches 20 CPS");
        std::cout << "[PASS] PerformanceMonitor real rate & stats verification (" << snap.actualRate << " CPS)\n";
    }

    std::cout << "\nALL TESTS PASSED!\n";
    return 0;
}
