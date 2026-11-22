#include "core/EngineController.hpp"

#include <avrt.h>

#include <algorithm>
#include <cmath>

namespace cadence {

namespace {

class MmcssGuard {
public:
    explicit MmcssGuard(bool enable) {
        if (enable) {
            DWORD taskIdx = 0;
            m_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
        }
    }
    ~MmcssGuard() {
        if (m_handle) {
            AvRevertMmThreadCharacteristics(m_handle);
        }
    }
    MmcssGuard(const MmcssGuard&)            = delete;
    MmcssGuard& operator=(const MmcssGuard&) = delete;

private:
    HANDLE m_handle = nullptr;
};

} // namespace

EngineController::EngineController() {
    m_wakeEvent   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    TimingEngine::SetPowerThrottlingDisabled(true);

    m_worker = std::jthread([this](std::stop_token st) {
        WorkerLoop(st);
    });
}

EngineController::~EngineController() {
    Stop();
    m_worker.request_stop();
    m_abortWait.store(true, std::memory_order_release);
    if (m_cancelEvent) SetEvent(m_cancelEvent);
    if (m_wakeEvent)   SetEvent(m_wakeEvent);
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_mouse.ReleaseAll();
    m_keyboard.ReleaseAll();

    if (m_wakeEvent)   CloseHandle(m_wakeEvent);
    if (m_cancelEvent) CloseHandle(m_cancelEvent);
}

void EngineController::SetStateChangedCallback(std::function<void(EngineState)> cb) {
    std::scoped_lock lock(m_cbMutex);
    m_stateCallback = std::move(cb);
}

void EngineController::TransitionState(EngineState newState) {
    const EngineState prev = m_state.exchange(newState, std::memory_order_acq_rel);
    if (prev != newState) {
        std::function<void(EngineState)> cb;
        {
            std::scoped_lock lock(m_cbMutex);
            cb = m_stateCallback;
        }
        if (cb) {
            cb(newState);
        }
    }
}

void EngineController::UpdateConfig(OperatingMode mode,
                                    const CpsConfig& cps,
                                    const KpsConfig& kps,
                                    const GlobalSettings& settings) {
    bool modeChanged = false;
    {
        std::scoped_lock lock(m_cfgMutex);
        modeChanged = (m_mode != mode);
        m_mode      = mode;
        m_cps       = cps;
        m_kps       = kps;
        m_settings  = settings;
    }

    TimingEngine::SetPowerThrottlingDisabled(settings.disablePowerThrottle);

    if (modeChanged) {
        // Safely stop active engine & release any held inputs when switching modes
        if (State() != EngineState::Idle) {
            Stop();
        }
        m_perf.ResetSession();
    }
}

std::string EngineController::QueryWindowProcessName(HWND hwnd) {
    if (!hwnd) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return {};

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};

    wchar_t pathBuf[MAX_PATH]{};
    DWORD len = MAX_PATH;
    std::string result;
    if (QueryFullProcessImageNameW(proc, 0, pathBuf, &len) && len > 0) {
        std::wstring_view sv(pathBuf, len);
        const size_t slash = sv.find_last_of(L"\\/");
        if (slash != std::wstring_view::npos) {
            sv.remove_prefix(slash + 1);
        }
        const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0) {
            result.resize(static_cast<size_t>(utf8Len));
            WideCharToMultiByte(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), result.data(), utf8Len, nullptr, nullptr);
        }
    }
    CloseHandle(proc);
    return result;
}

bool EngineController::MatchesTargetWindowLock(const GlobalSettings& gs, HWND& cachedHwnd, bool& cachedMatch) const noexcept {
    if (!gs.targetWindowLock || gs.targetProcessName.empty()) {
        return true;
    }
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == cachedHwnd) {
        return cachedMatch;
    }
    cachedHwnd = fg;
    const std::string procName = QueryWindowProcessName(fg);
    cachedMatch = (_stricmp(procName.c_str(), gs.targetProcessName.c_str()) == 0);
    return cachedMatch;
}

bool EngineController::IsSelfWindowBlocking(bool checkCursorForMouse, CursorMode cursorMode) const noexcept {
    const HWND appHwnd = m_appHwnd.load(std::memory_order_acquire);
    if (!appHwnd || !IsWindowVisible(appHwnd) || IsIconic(appHwnd)) {
        return false;
    }

    // 1. If Cadence window is foreground/focused, never click or press into it
    if (GetForegroundWindow() == appHwnd) {
        return true;
    }

    // 2. For mouse follow-cursor clicks, also protect if cursor hovers directly over Cadence window
    if (checkCursorForMouse && cursorMode == CursorMode::FollowCursor) {
        POINT pt{};
        if (GetCursorPos(&pt)) {
            HWND under = WindowFromPoint(pt);
            if (under && (under == appHwnd || GetAncestor(under, GA_ROOT) == appHwnd)) {
                return true;
            }
        }
    }
    return false;
}

void EngineController::Start(bool fromUiButton) {
    bool stopOnSelfFocus = true;
    OperatingMode mode{};
    CpsConfig cps{};
    {
        std::scoped_lock lock(m_cfgMutex);
        stopOnSelfFocus = m_settings.stopOnSelfFocus;
        mode            = m_mode;
        cps             = m_cps;
    }

    m_perf.ResetSession();
    m_abortWait.store(false, std::memory_order_release);
    if (m_cancelEvent) ResetEvent(m_cancelEvent);

    const bool selfFocused = m_appFocused.load(std::memory_order_acquire) ||
                             IsSelfWindowBlocking(mode == OperatingMode::CPS, cps.cursorMode);

    if (stopOnSelfFocus && selfFocused) {
        if (fromUiButton) {
            // User clicked "Start" inside Cadence: arm engine so it activates as soon as they switch to target window
            TransitionState(EngineState::Armed);
        } else {
            // Hotkey pressed while Cadence window is open/focused: do not click inside Cadence!
            Stop();
        }
        return;
    }

    TransitionState(EngineState::Active);
    if (m_wakeEvent) SetEvent(m_wakeEvent);
}

void EngineController::Stop() {
    m_holdKeyActive.store(false, std::memory_order_release);
    m_abortWait.store(true, std::memory_order_release);
    if (m_cancelEvent) SetEvent(m_cancelEvent);
    if (m_wakeEvent)   ResetEvent(m_wakeEvent);

    TransitionState(EngineState::Idle);
    m_mouse.ReleaseAll();
    m_keyboard.ReleaseAll();
}

void EngineController::PanicStop() {
    Stop();
}

void EngineController::TogglePause() {
    const EngineState st = State();
    if (st == EngineState::Active) {
        m_abortWait.store(true, std::memory_order_release);
        if (m_cancelEvent) SetEvent(m_cancelEvent);
        if (m_wakeEvent)   ResetEvent(m_wakeEvent);
        m_mouse.ReleaseAll();
        m_keyboard.ReleaseAll();
        TransitionState(EngineState::Paused);
    } else if (st == EngineState::Paused) {
        bool stopOnSelfFocus = true;
        {
            std::scoped_lock lock(m_cfgMutex);
            stopOnSelfFocus = m_settings.stopOnSelfFocus;
        }
        if (stopOnSelfFocus && m_appFocused.load(std::memory_order_acquire)) {
            TransitionState(EngineState::Armed);
            return;
        }
        m_abortWait.store(false, std::memory_order_release);
        if (m_cancelEvent) ResetEvent(m_cancelEvent);
        TransitionState(EngineState::Active);
        if (m_wakeEvent) SetEvent(m_wakeEvent);
    }
}

void EngineController::HandleHotkeyPress(OperatingMode hotkeyMode) {
    OperatingMode activeMode{};
    ActivationMode activation{};
    {
        std::scoped_lock lock(m_cfgMutex);
        activeMode = m_mode;
        activation = (activeMode == OperatingMode::CPS) ? m_cps.activation : m_kps.activation;
    }

    // Only react to the currently selected operating mode's hotkey
    if (hotkeyMode != activeMode) {
        return;
    }

    if (activation == ActivationMode::Toggle) {
        if (State() == EngineState::Idle) {
            Start(false);
        } else {
            Stop();
        }
    } else {
        // Hold mode: start while pressed
        m_holdKeyActive.store(true, std::memory_order_release);
        if (State() == EngineState::Idle) {
            Start(false);
        }
    }
}

void EngineController::HandleHotkeyRelease(OperatingMode hotkeyMode) {
    OperatingMode activeMode{};
    ActivationMode activation{};
    {
        std::scoped_lock lock(m_cfgMutex);
        activeMode = m_mode;
        activation = (activeMode == OperatingMode::CPS) ? m_cps.activation : m_kps.activation;
    }

    if (hotkeyMode != activeMode) {
        return;
    }

    if (activation == ActivationMode::Hold) {
        m_holdKeyActive.store(false, std::memory_order_release);
        if (State() != EngineState::Idle) {
            Stop();
        }
    }
}

void EngineController::NotifyAppFocusChanged(bool isFocused) {
    m_appFocused.store(isFocused, std::memory_order_release);

    bool stopOnSelfFocus = true;
    {
        std::scoped_lock lock(m_cfgMutex);
        stopOnSelfFocus = m_settings.stopOnSelfFocus;
    }
    if (!stopOnSelfFocus) {
        return;
    }

    const EngineState st = State();
    if (isFocused) {
        // User explicitly focused / opened the Cadence window -> STOP immediately!
        if (st == EngineState::Active || st == EngineState::Paused) {
            Stop();
        }
    } else {
        // User switched away from Cadence window -> if Armed, start CPS/KPS!
        if (st == EngineState::Armed) {
            m_abortWait.store(false, std::memory_order_release);
            if (m_cancelEvent) ResetEvent(m_cancelEvent);
            TransitionState(EngineState::Active);
            if (m_wakeEvent) SetEvent(m_wakeEvent);
        }
    }
}

TelemetrySnapshot EngineController::GetTelemetry() {
    return m_perf.ComputeSnapshot(
        m_timing.NowQpc(),
        m_timing.QpcFrequency(),
        State() == EngineState::Active
    );
}

void EngineController::WorkerLoop(std::stop_token st) {
    while (!st.stop_requested()) {
        // Completely parked when idle/paused/armed -> 0.00% CPU
        if (State() != EngineState::Active) {
            WaitForSingleObject(m_wakeEvent, INFINITE);
            if (st.stop_requested()) break;
            if (State() != EngineState::Active) continue;
        }

        // Snapshot session settings upon activation
        GlobalSettings settingsSnapshot{};
        {
            std::scoped_lock lock(m_cfgMutex);
            settingsSnapshot = m_settings;
        }

        // Elevate thread via MMCSS ONLY for the duration of active clicking/pressing
        MmcssGuard mmcss(settingsSnapshot.mmcssPriority);

        int64_t  sessionStartQpc    = m_timing.NowQpc();
        int64_t  nextTargetQpc      = sessionStartQpc;
        int64_t  lastEventQpc       = 0;
        double   expectedIntervalMs = 0.0;
        uint64_t sessionEvents      = 0;
        size_t   seqIndex           = 0;
        HWND     cachedTargetHwnd   = nullptr;
        bool     cachedTargetMatch  = false;

        while (!st.stop_requested() && State() == EngineState::Active && !m_abortWait.load(std::memory_order_acquire)) {
            OperatingMode mode{};
            CpsConfig cps{};
            KpsConfig kps{};
            GlobalSettings gs{};
            {
                std::scoped_lock lock(m_cfgMutex);
                mode = m_mode;
                cps  = m_cps;
                kps  = m_kps;
                gs   = m_settings;
            }

            // Self-click protection: stop immediately if Cadence window is focused or hovered
            if (gs.stopOnSelfFocus && IsSelfWindowBlocking(mode == OperatingMode::CPS, cps.cursorMode)) {
                Stop();
                break;
            }

            // Target-Window Lock (AutoHotkey #HotIf WinActive style): hold quietly while non-target app is focused
            if (!MatchesTargetWindowLock(gs, cachedTargetHwnd, cachedTargetMatch)) {
                m_mouse.ReleaseAll();
                m_keyboard.ReleaseAll();
                if (!m_timing.WaitForMs(25.0, m_cancelEvent, m_abortWait, false)) {
                    break;
                }
                nextTargetQpc      = m_timing.NowQpc();
                lastEventQpc       = 0;
                expectedIntervalMs = 0.0;
                continue;
            }

            // Repeat limit check (Count or Duration)
            const RepeatLimitMode limMode = (mode == OperatingMode::CPS) ? cps.limitMode : kps.limitMode;
            const uint32_t limVal         = (mode == OperatingMode::CPS) ? cps.limitValue : kps.limitValue;
            if (limMode == RepeatLimitMode::Count && sessionEvents >= limVal) {
                Stop();
                break;
            }
            if (limMode == RepeatLimitMode::Duration) {
                const double elapsedSec = m_timing.QpcToMilliseconds(m_timing.NowQpc() - sessionStartQpc) / 1000.0;
                if (elapsedSec >= static_cast<double>(limVal)) {
                    Stop();
                    break;
                }
            }

            if (mode == OperatingMode::CPS) {
                RunCpsCycle(cps, gs.hybridQpcTimer, gs.stopOnSelfFocus, nextTargetQpc, lastEventQpc, expectedIntervalMs, sessionEvents);
            } else {
                RunKpsCycle(kps, gs.hybridQpcTimer, gs.stopOnSelfFocus, nextTargetQpc, lastEventQpc, expectedIntervalMs, sessionEvents, seqIndex);
            }
        }

        // Guarantee clean state on exiting active loop
        m_mouse.ReleaseAll();
        m_keyboard.ReleaseAll();
    }
}

void EngineController::RunCpsCycle(const CpsConfig& cfg,
                                   bool hybridSpin,
                                   bool stopOnSelfFocus,
                                   int64_t& nextTargetQpc,
                                   int64_t& lastEventQpc,
                                   double& expectedIntervalMs,
                                   uint64_t& sessionEvents) {
    const double baseIntervalMs = 1000.0 / std::clamp(cfg.rateCps, 0.5, 500.0);
    const double jitter         = cfg.jitterEnabled ? m_timing.SampleJitterMs(cfg.jitterMs) : 0.0;
    const double nextIntervalMs = std::max(0.5, baseIntervalMs + jitter);

    if (!m_timing.WaitUntil(nextTargetQpc, m_cancelEvent, m_abortWait, hybridSpin)) {
        return;
    }

    // Re-check self-click protection right at dispatch moment
    if (stopOnSelfFocus && IsSelfWindowBlocking(true, cfg.cursorMode)) {
        Stop();
        return;
    }

    const int64_t dispatchQpc = m_timing.NowQpc();
    const double actualIntervalMs = (lastEventQpc > 0)
        ? m_timing.QpcToMilliseconds(dispatchQpc - lastEventQpc)
        : 0.0;
    lastEventQpc = dispatchQpc;

    uint32_t clicksInPattern = 1;
    if (cfg.pattern == ClickPattern::Double)           clicksInPattern = 2;
    else if (cfg.pattern == ClickPattern::TripleBurst) clicksInPattern = 3;
    else if (cfg.pattern == ClickPattern::CustomBurst) clicksInPattern = std::clamp(cfg.burstCount, 2u, 20u);

    const double maxHoldMs = std::max(0.2, (nextIntervalMs / static_cast<double>(clicksInPattern)) * 0.45);
    const double holdMs    = std::min(static_cast<double>(cfg.holdDurationMs), maxHoldMs);

    for (uint32_t i = 0; i < clicksInPattern; ++i) {
        if (m_abortWait.load(std::memory_order_acquire)) break;
        if (stopOnSelfFocus && IsSelfWindowBlocking(true, cfg.cursorMode)) {
            Stop();
            return;
        }

        m_mouse.SendDown(cfg.button, cfg.cursorMode, cfg.lockX, cfg.lockY, cfg.lockedMicroWiggle);
        if (holdMs >= 0.25) {
            m_timing.WaitForMs(holdMs, m_cancelEvent, m_abortWait, hybridSpin);
        }
        m_mouse.SendUp(cfg.button, cfg.cursorMode, cfg.lockX, cfg.lockY);

        ++sessionEvents;
        if (i == 0) {
            m_perf.RecordEvent(dispatchQpc, expectedIntervalMs, actualIntervalMs);
        } else {
            const int64_t subQpc = m_timing.NowQpc();
            const double subDtMs = m_timing.QpcToMilliseconds(subQpc - lastEventQpc);
            lastEventQpc = subQpc;
            m_perf.RecordEvent(subQpc, 0.0, subDtMs);
        }

        if (i + 1 < clicksInPattern) {
            const double intraGapMs = std::min(22.0, maxHoldMs);
            if (!m_timing.WaitForMs(intraGapMs, m_cancelEvent, m_abortWait, hybridSpin)) {
                return;
            }
        }
    }

    const int64_t nowAfter = m_timing.NowQpc();
    if (cfg.pattern == ClickPattern::CustomBurst) {
        // Measure burst cooldown strictly after the burst finishes
        const double cooldownMs = std::max(0.5, static_cast<double>(cfg.burstCooldownMs) + jitter);
        nextTargetQpc      = nowAfter + m_timing.MicrosecondsToQpc(cooldownMs * 1000.0);
        expectedIntervalMs = m_timing.QpcToMilliseconds(nextTargetQpc - lastEventQpc);
    } else {
        // Drift-compensated next deadline (clamped if OS stalled to avoid burst catch-up)
        const int64_t stepQpc = m_timing.MicrosecondsToQpc(nextIntervalMs * 1000.0);
        nextTargetQpc += stepQpc;
        if (nextTargetQpc < nowAfter - stepQpc) {
            nextTargetQpc = nowAfter + stepQpc;
        }
        expectedIntervalMs = (clicksInPattern == 1)
            ? nextIntervalMs
            : m_timing.QpcToMilliseconds(std::max<int64_t>(0, nextTargetQpc - lastEventQpc));
    }
}

void EngineController::RunKpsCycle(const KpsConfig& cfg,
                                   bool hybridSpin,
                                   bool stopOnSelfFocus,
                                   int64_t& nextTargetQpc,
                                   int64_t& lastEventQpc,
                                   double& expectedIntervalMs,
                                   uint64_t& sessionEvents,
                                   size_t& seqIndex) {
    const double baseIntervalMs = 1000.0 / std::clamp(cfg.rateKps, 0.5, 500.0);
    const double jitter         = cfg.jitterEnabled ? m_timing.SampleJitterMs(cfg.jitterMs) : 0.0;
    const double nextIntervalMs = std::max(0.5, baseIntervalMs + jitter);

    if (!m_timing.WaitUntil(nextTargetQpc, m_cancelEvent, m_abortWait, hybridSpin)) {
        return;
    }

    if (stopOnSelfFocus && IsSelfWindowBlocking(false, CursorMode::FollowCursor)) {
        Stop();
        return;
    }

    const int64_t dispatchQpc = m_timing.NowQpc();
    const double actualIntervalMs = (lastEventQpc > 0)
        ? m_timing.QpcToMilliseconds(dispatchQpc - lastEventQpc)
        : 0.0;
    lastEventQpc = dispatchQpc;

    const double holdMs = std::min(static_cast<double>(cfg.pressDurationMs), std::max(0.2, nextIntervalMs * 0.55));

    if (cfg.inputMode == KeyInputMode::Sequence && !cfg.sequenceVks.empty()) {
        const uint16_t vk = cfg.sequenceVks[seqIndex % cfg.sequenceVks.size()];
        ++seqIndex;
        m_keyboard.SendKeyDown(vk, cfg.dispatchMethod);
        if (holdMs >= 0.25) {
            m_timing.WaitForMs(holdMs, m_cancelEvent, m_abortWait, hybridSpin);
        }
        m_keyboard.SendKeyUp(vk, cfg.dispatchMethod);
    } else {
        m_keyboard.SendChordDown(cfg.targetVk, cfg.modCtrl, cfg.modShift, cfg.modAlt, cfg.dispatchMethod);
        if (holdMs >= 0.25) {
            m_timing.WaitForMs(holdMs, m_cancelEvent, m_abortWait, hybridSpin);
        }
        m_keyboard.SendChordUp(cfg.targetVk, cfg.modCtrl, cfg.modShift, cfg.modAlt, cfg.dispatchMethod);
    }

    ++sessionEvents;
    m_perf.RecordEvent(dispatchQpc, expectedIntervalMs, actualIntervalMs);

    const int64_t stepQpc = m_timing.MicrosecondsToQpc(nextIntervalMs * 1000.0);
    nextTargetQpc += stepQpc;
    const int64_t nowAfter = m_timing.NowQpc();
    if (nextTargetQpc < nowAfter - stepQpc) {
        nextTargetQpc = nowAfter + stepQpc;
    }
    expectedIntervalMs = nextIntervalMs;
}

} // namespace cadence
