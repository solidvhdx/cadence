#pragma once

#include "core/KeyboardInputEngine.hpp"
#include "core/MouseInputEngine.hpp"
#include "core/PerformanceMonitor.hpp"
#include "core/TimingEngine.hpp"
#include "core/Types.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace cadence {

class EngineController {
public:
    EngineController();
    ~EngineController();

    EngineController(const EngineController&)            = delete;
    EngineController& operator=(const EngineController&) = delete;

    void SetAppWindow(HWND hwnd) noexcept { m_appHwnd.store(hwnd, std::memory_order_release); }
    void SetStateChangedCallback(std::function<void(EngineState)> cb);

    // Thread-safe config synchronization from UI thread
    void UpdateConfig(OperatingMode mode,
                      const CpsConfig& cps,
                      const KpsConfig& kps,
                      const GlobalSettings& settings);

    // Transport controls
    void Start(bool fromUiButton = false);
    void Stop();
    void PanicStop();
    void TogglePause();

    // Hotkey entry points (handles both Toggle and Hold modes)
    void HandleHotkeyPress(OperatingMode hotkeyMode);
    void HandleHotkeyRelease(OperatingMode hotkeyMode);

    // Called from MainWindow on WM_ACTIVATE / WM_SETFOCUS / WM_KILLFOCUS
    void NotifyAppFocusChanged(bool isFocused);

    [[nodiscard]] EngineState State() const noexcept {
        return m_state.load(std::memory_order_acquire);
    }

    [[nodiscard]] TelemetrySnapshot GetTelemetry();
    [[nodiscard]] TimingEngine& Timing() noexcept { return m_timing; }

    // Helper: extracts executable filename (UTF-8, e.g. "RobloxPlayerBeta.exe") for an HWND
    static std::string QueryWindowProcessName(HWND hwnd);

private:
    void WorkerLoop(std::stop_token st);
    void RunCpsCycle(const CpsConfig& cfg, bool hybridSpin, bool stopOnSelfFocus,
                     int64_t& nextTargetQpc, int64_t& lastEventQpc, uint64_t& sessionEvents);
    void RunKpsCycle(const KpsConfig& cfg, bool hybridSpin, bool stopOnSelfFocus,
                     int64_t& nextTargetQpc, int64_t& lastEventQpc, uint64_t& sessionEvents, size_t& seqIndex);

    [[nodiscard]] bool IsSelfWindowBlocking(bool checkCursorForMouse, CursorMode cursorMode) const noexcept;
    [[nodiscard]] bool MatchesTargetWindowLock(const GlobalSettings& gs, HWND& cachedHwnd, bool& cachedMatch) const noexcept;
    void TransitionState(EngineState newState);

    TimingEngine        m_timing;
    MouseInputEngine    m_mouse;
    KeyboardInputEngine m_keyboard;
    PerformanceMonitor  m_perf;

    std::atomic<HWND>        m_appHwnd{nullptr};
    std::atomic<EngineState> m_state{EngineState::Idle};
    std::atomic<bool>        m_abortWait{false};
    std::atomic<bool>        m_holdKeyActive{false};
    std::atomic<bool>        m_appFocused{true};

    HANDLE m_wakeEvent   = nullptr;
    HANDLE m_cancelEvent = nullptr;

    mutable std::mutex m_cfgMutex;
    OperatingMode      m_mode = OperatingMode::CPS;
    CpsConfig          m_cps{};
    KpsConfig          m_kps{};
    GlobalSettings     m_settings{};

    std::mutex                       m_cbMutex;
    std::function<void(EngineState)> m_stateCallback;

    std::jthread m_worker;
};

} // namespace cadence
