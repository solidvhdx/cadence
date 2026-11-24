#include "core/EngineController.hpp"
#include "core/StringUtil.hpp"

#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <limits>

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
    auto next = std::make_shared<const EngineSettings>(EngineSettings{mode, cps, kps, settings});
    const auto prev = m_settings.exchange(std::move(next), std::memory_order_acq_rel);
    const bool modeChanged = !prev || prev->mode != mode;

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
        result = WideToUtf8(sv);
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
    const auto cfg = Settings();

    m_perf.ResetSession();
    m_abortWait.store(false, std::memory_order_release);
    if (m_cancelEvent) ResetEvent(m_cancelEvent);

    const bool selfFocused = m_appFocused.load(std::memory_order_acquire) ||
                             IsSelfWindowBlocking(cfg->mode == OperatingMode::CPS, cfg->cps.cursorMode);

    if (cfg->settings.stopOnSelfFocus && selfFocused) {
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
        if (Settings()->settings.stopOnSelfFocus && m_appFocused.load(std::memory_order_acquire)) {
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
    const auto cfg = Settings();
    const OperatingMode  activeMode = cfg->mode;
    const ActivationMode activation = (activeMode == OperatingMode::CPS) ? cfg->cps.activation : cfg->kps.activation;

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
    const auto cfg = Settings();
    const OperatingMode  activeMode = cfg->mode;
    const ActivationMode activation = (activeMode == OperatingMode::CPS) ? cfg->cps.activation : cfg->kps.activation;

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

    if (!Settings()->settings.stopOnSelfFocus) {
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

        // Elevate thread via MMCSS ONLY for the duration of active clicking/pressing
        MmcssGuard mmcss(Settings()->settings.mmcssPriority);

        SessionState session{};
        session.startQpc      = m_timing.NowQpc();
        session.nextTargetQpc = session.startQpc;
        HWND cachedTargetHwnd  = nullptr;
        bool cachedTargetMatch = false;

        while (!st.stop_requested() && State() == EngineState::Active && !m_abortWait.load(std::memory_order_acquire)) {
            // One atomic load per cycle; the snapshot stays valid even if the UI publishes a newer one meanwhile
            const auto snapshot = Settings();
            const EngineSettings& cfg = *snapshot;

            // Self-click protection: stop immediately if Cadence window is focused or hovered
            if (cfg.settings.stopOnSelfFocus && IsSelfWindowBlocking(cfg.mode == OperatingMode::CPS, cfg.cps.cursorMode)) {
                Stop();
                break;
            }

            // Target-Window Lock (AutoHotkey #HotIf WinActive style): hold quietly while non-target app is focused
            if (!MatchesTargetWindowLock(cfg.settings, cachedTargetHwnd, cachedTargetMatch)) {
                m_mouse.ReleaseAll();
                m_keyboard.ReleaseAll();
                if (!m_timing.WaitFor(Millis{25.0}, m_cancelEvent, m_abortWait, false)) {
                    break;
                }
                session.nextTargetQpc    = m_timing.NowQpc();
                session.lastEventQpc     = 0;
                session.expectedInterval = Millis{0.0};
                continue;
            }

            // Repeat limit check (Count or Duration)
            const RepeatLimitMode limMode = (cfg.mode == OperatingMode::CPS) ? cfg.cps.limitMode : cfg.kps.limitMode;
            const uint32_t limVal         = (cfg.mode == OperatingMode::CPS) ? cfg.cps.limitValue : cfg.kps.limitValue;
            if (limMode == RepeatLimitMode::Count && session.events >= limVal) {
                Stop();
                break;
            }
            if (limMode == RepeatLimitMode::Duration) {
                const Seconds elapsed = m_timing.QpcToMillis(m_timing.NowQpc() - session.startQpc);
                if (elapsed >= Seconds{static_cast<double>(limVal)}) {
                    Stop();
                    break;
                }
            }

            if (cfg.mode == OperatingMode::CPS) {
                // A burst must never overshoot a Count limit
                const uint64_t budget = (limMode == RepeatLimitMode::Count)
                    ? static_cast<uint64_t>(limVal) - session.events
                    : std::numeric_limits<uint64_t>::max();
                RunCpsCycle(cfg.cps, cfg.settings.hybridQpcTimer, cfg.settings.stopOnSelfFocus, budget, session);
            } else {
                RunKpsCycle(cfg.kps, cfg.settings.hybridQpcTimer, cfg.settings.stopOnSelfFocus, session);
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
                                   uint64_t eventBudget,
                                   SessionState& s) {
    const Millis baseInterval{1000.0 / std::clamp(cfg.rateCps, 0.5, 500.0)};
    const Millis jitter       = cfg.jitterEnabled ? m_timing.SampleJitter(Millis{cfg.jitterMs}) : Millis{0.0};
    const Millis nextInterval = std::max(Millis{0.5}, baseInterval + jitter);

    if (!m_timing.WaitUntil(s.nextTargetQpc, m_cancelEvent, m_abortWait, hybridSpin)) {
        return;
    }

    // Re-check self-click protection right at dispatch moment
    if (stopOnSelfFocus && IsSelfWindowBlocking(true, cfg.cursorMode)) {
        Stop();
        return;
    }

    const int64_t dispatchQpc = m_timing.NowQpc();
    const Millis actualInterval = (s.lastEventQpc > 0)
        ? m_timing.QpcToMillis(dispatchQpc - s.lastEventQpc)
        : Millis{0.0};
    s.lastEventQpc = dispatchQpc;

    uint32_t clicksInPattern = 1;
    if (cfg.pattern == ClickPattern::Double)           clicksInPattern = 2;
    else if (cfg.pattern == ClickPattern::TripleBurst) clicksInPattern = 3;
    else if (cfg.pattern == ClickPattern::CustomBurst) clicksInPattern = std::clamp(cfg.burstCount, 2u, 20u);
    clicksInPattern = static_cast<uint32_t>(std::clamp<uint64_t>(eventBudget, 1, clicksInPattern));

    const Millis maxHold = std::max(Millis{0.2}, (nextInterval / static_cast<double>(clicksInPattern)) * 0.45);
    const Millis hold    = std::min(Millis{static_cast<double>(cfg.holdDurationMs)}, maxHold);

    for (uint32_t i = 0; i < clicksInPattern; ++i) {
        if (m_abortWait.load(std::memory_order_acquire)) break;
        if (stopOnSelfFocus && IsSelfWindowBlocking(true, cfg.cursorMode)) {
            Stop();
            return;
        }

        m_mouse.SendDown(cfg.button, cfg.cursorMode, cfg.lockX, cfg.lockY, cfg.lockedMicroWiggle);
        if (hold >= Millis{0.25}) {
            if (!m_timing.WaitFor(hold, m_cancelEvent, m_abortWait, hybridSpin)) {
                return; // Aborted: WorkerLoop's ReleaseAll() handles clean release without duplicate UP
            }
        }
        m_mouse.SendUp(cfg.button, cfg.cursorMode, cfg.lockX, cfg.lockY);

        ++s.events;
        if (i == 0) {
            m_perf.RecordEvent(dispatchQpc, s.expectedInterval, actualInterval);
        } else {
            const int64_t subQpc = m_timing.NowQpc();
            const Millis  subDt  = m_timing.QpcToMillis(subQpc - s.lastEventQpc);
            s.lastEventQpc = subQpc;
            m_perf.RecordEvent(subQpc, Millis{0.0}, subDt);
        }

        if (i + 1 < clicksInPattern) {
            const Millis intraGap = std::min(Millis{22.0}, maxHold);
            if (!m_timing.WaitFor(intraGap, m_cancelEvent, m_abortWait, hybridSpin)) {
                return;
            }
        }
    }

    const int64_t nowAfter = m_timing.NowQpc();
    if (cfg.pattern == ClickPattern::CustomBurst) {
        // Measure burst cooldown strictly after the burst finishes
        const Millis cooldown = std::max(Millis{0.5}, Millis{static_cast<double>(cfg.burstCooldownMs)} + jitter);
        s.nextTargetQpc    = nowAfter + m_timing.ToQpcTicks(cooldown);
        s.expectedInterval = m_timing.QpcToMillis(s.nextTargetQpc - s.lastEventQpc);
    } else {
        // Drift-compensated next deadline (clamped if OS stalled to avoid burst catch-up)
        const int64_t stepQpc = m_timing.ToQpcTicks(nextInterval);
        s.nextTargetQpc += stepQpc;
        if (s.nextTargetQpc < nowAfter - stepQpc) {
            s.nextTargetQpc = nowAfter + stepQpc;
        }
        s.expectedInterval = (clicksInPattern == 1)
            ? nextInterval
            : m_timing.QpcToMillis(std::max<int64_t>(0, s.nextTargetQpc - s.lastEventQpc));
    }
}

void EngineController::RunKpsCycle(const KpsConfig& cfg,
                                   bool hybridSpin,
                                   bool stopOnSelfFocus,
                                   SessionState& s) {
    const Millis baseInterval{1000.0 / std::clamp(cfg.rateKps, 0.5, 500.0)};
    const Millis jitter       = cfg.jitterEnabled ? m_timing.SampleJitter(Millis{cfg.jitterMs}) : Millis{0.0};
    const Millis nextInterval = std::max(Millis{0.5}, baseInterval + jitter);

    if (!m_timing.WaitUntil(s.nextTargetQpc, m_cancelEvent, m_abortWait, hybridSpin)) {
        return;
    }

    if (stopOnSelfFocus && IsSelfWindowBlocking(false, CursorMode::FollowCursor)) {
        Stop();
        return;
    }

    const int64_t dispatchQpc = m_timing.NowQpc();
    const Millis actualInterval = (s.lastEventQpc > 0)
        ? m_timing.QpcToMillis(dispatchQpc - s.lastEventQpc)
        : Millis{0.0};
    s.lastEventQpc = dispatchQpc;

    const Millis hold = std::min(Millis{static_cast<double>(cfg.pressDurationMs)},
                                 std::max(Millis{0.2}, nextInterval * 0.55));

    if (cfg.inputMode == KeyInputMode::Sequence && !cfg.sequenceVks.empty()) {
        const uint16_t vk = cfg.sequenceVks[s.seqIndex % cfg.sequenceVks.size()];
        ++s.seqIndex;
        m_keyboard.SendKeyDown(vk, cfg.dispatchMethod);
        if (hold >= Millis{0.25}) {
            if (!m_timing.WaitFor(hold, m_cancelEvent, m_abortWait, hybridSpin)) {
                return;
            }
        }
        m_keyboard.SendKeyUp(vk, cfg.dispatchMethod);
    } else {
        m_keyboard.SendChordDown(cfg.targetVk, cfg.modCtrl, cfg.modShift, cfg.modAlt, cfg.dispatchMethod);
        if (hold >= Millis{0.25}) {
            if (!m_timing.WaitFor(hold, m_cancelEvent, m_abortWait, hybridSpin)) {
                return;
            }
        }
        m_keyboard.SendChordUp(cfg.targetVk, cfg.modCtrl, cfg.modShift, cfg.modAlt, cfg.dispatchMethod);
    }

    ++s.events;
    m_perf.RecordEvent(dispatchQpc, s.expectedInterval, actualInterval);

    const int64_t stepQpc = m_timing.ToQpcTicks(nextInterval);
    s.nextTargetQpc += stepQpc;
    const int64_t nowAfter = m_timing.NowQpc();
    if (s.nextTargetQpc < nowAfter - stepQpc) {
        s.nextTargetQpc = nowAfter + stepQpc;
    }
    s.expectedInterval = nextInterval;
}

} // namespace cadence
