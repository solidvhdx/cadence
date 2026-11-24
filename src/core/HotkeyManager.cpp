#include "core/HotkeyManager.hpp"

#include <vector>

namespace cadence {

HotkeyManager::~HotkeyManager() {
    Shutdown();
}

bool HotkeyManager::Initialize(HWND hwnd, HotkeyCallbacks callbacks) {
    m_hwnd      = hwnd;
    m_callbacks = std::move(callbacks);
    m_keyDownState.fill(false);
    m_keyLastDownMs.fill(0);

    // Register both Keyboard and Mouse Raw Input with RIDEV_INPUTSINK (background global input, zero hook latency)
    RAWINPUTDEVICE devices[2]{};
    devices[0].usUsagePage = 0x01; // Generic Desktop Controls
    devices[0].usUsage     = 0x06; // Keyboard
    devices[0].dwFlags     = RIDEV_INPUTSINK;
    devices[0].hwndTarget  = hwnd;

    devices[1].usUsagePage = 0x01; // Generic Desktop Controls
    devices[1].usUsage     = 0x02; // Mouse
    devices[1].dwFlags     = RIDEV_INPUTSINK;
    devices[1].hwndTarget  = hwnd;

    const bool ok = RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
    RegisterPanicFallback();
    return ok;
}

void HotkeyManager::Shutdown() {
    UnregisterPanicFallback();
    if (m_hwnd) {
        RAWINPUTDEVICE devices[2]{};
        devices[0].usUsagePage = 0x01;
        devices[0].usUsage     = 0x06;
        devices[0].dwFlags     = RIDEV_REMOVE;
        devices[0].hwndTarget  = nullptr;

        devices[1].usUsagePage = 0x01;
        devices[1].usUsage     = 0x02;
        devices[1].dwFlags     = RIDEV_REMOVE;
        devices[1].hwndTarget  = nullptr;

        RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
        m_hwnd = nullptr;
    }
}

void HotkeyManager::SetBindings(const HotkeyBinding& cpsHotkey,
                                const HotkeyBinding& kpsHotkey,
                                const HotkeyBinding& panicHotkey) {
    m_cpsBinding   = cpsHotkey;
    m_kpsBinding   = kpsHotkey;
    m_panicBinding = panicHotkey;
    RegisterPanicFallback();
}

void HotkeyManager::SetPanicFallbackEnabled(bool enabled) {
    if (m_panicFallbackWanted == enabled) {
        return;
    }
    m_panicFallbackWanted = enabled;
    RegisterPanicFallback();
}

void HotkeyManager::RegisterPanicFallback() {
    UnregisterPanicFallback();
    if (m_panicFallbackWanted && m_hwnd &&
        m_panicBinding.source == HotkeySource::Keyboard && m_panicBinding.code != 0) {
        m_panicRegistered = RegisterHotKey(m_hwnd, kPanicHotkeyId, MOD_NOREPEAT, m_panicBinding.code) != FALSE;
    }
}

void HotkeyManager::UnregisterPanicFallback() {
    if (m_hwnd && m_panicRegistered) {
        UnregisterHotKey(m_hwnd, kPanicHotkeyId);
        m_panicRegistered = false;
    }
}

void HotkeyManager::BeginCapture(bool allowMouseButtons,
                                 std::function<void(HotkeyBinding)> onCaptured,
                                 std::function<void()> onCancelled) {
    m_capturing             = true;
    m_captureAllowMouse     = allowMouseButtons;
    m_captureCallback       = std::move(onCaptured);
    m_captureCancelCallback = std::move(onCancelled);
}

void HotkeyManager::CancelCapture() {
    auto cancelCb = std::move(m_captureCancelCallback);
    m_capturing             = false;
    m_captureCallback       = nullptr;
    m_captureCancelCallback = nullptr;
    if (cancelCb) {
        cancelCb();
    }
}

void HotkeyManager::BeginGlobalLeftClickCapture(std::function<void(POINT, HWND)> onPicked,
                                                std::function<void()> onCancelled) {
    m_pickingLeftClick        = true;
    m_leftClickCallback       = std::move(onPicked);
    m_leftClickCancelCallback = std::move(onCancelled);
}

void HotkeyManager::CancelLeftClickCapture() {
    auto cancelCb = std::move(m_leftClickCancelCallback);
    m_pickingLeftClick        = false;
    m_leftClickCallback       = nullptr;
    m_leftClickCancelCallback = nullptr;
    if (cancelCb) {
        cancelCb();
    }
}

void HotkeyManager::TriggerPanicStop() {
    const uint64_t nowMs = GetTickCount64();
    if (nowMs - m_lastPanicMs < 100ULL) {
        return; // Debounce duplicate WM_INPUT + WM_HOTKEY delivery on the same physical press
    }
    m_lastPanicMs = nowMs;
    if (m_callbacks.onPanicStop) {
        m_callbacks.onPanicStop();
    }
}

void HotkeyManager::ProcessWmHotkey(WPARAM wParam) {
    if (static_cast<int>(wParam) == kPanicHotkeyId && !m_capturing) {
        TriggerPanicStop();
    }
}

void HotkeyManager::ProcessRawInput(LPARAM lParam) {
    UINT size = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) {
        return;
    }

    // Stack buffer for fast path (RAWINPUT is ~48 bytes), avoiding heap allocations per input packet
    alignas(RAWINPUT) BYTE stackBuf[128];
    std::vector<BYTE> heapBuf;
    BYTE* bufPtr = stackBuf;
    if (size > sizeof(stackBuf)) {
        heapBuf.resize(size);
        bufPtr = heapBuf.data();
    }

    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, bufPtr, &size, sizeof(RAWINPUTHEADER)) != size) {
        return;
    }

    const auto* raw = reinterpret_cast<const RAWINPUT*>(bufPtr);

    // Kernel-guaranteed filter: SendInput synthetic events arrive with hDevice == nullptr.
    // Real physical hardware events ALWAYS carry a valid device handle (never drop real F12 Panic Stop!).
    if (raw->header.hDevice == nullptr) {
        return;
    }

    if (raw->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& kb = raw->data.keyboard;
        uint16_t vk = kb.VKey;
        // Ignore fake 0xFF extended prefix packets (e.g. PrintScreen / Pause / NumLock prefix)
        if (vk == 0 || vk >= 255) {
            return;
        }
        // Ignore German AltGr synthetic fake LCtrl (MakeCode 0x1D with RI_KEY_E0? No, real AltGr sends fake LCtrl with MakeCode 0x21D / E1 or Flags)
        if (vk == VK_CONTROL && (kb.Flags & RI_KEY_E1)) {
            return;
        }
        // Cancel global left-click pick on Escape
        const bool isUp = (kb.Flags & RI_KEY_BREAK) != 0;
        if (!isUp && vk == VK_ESCAPE && m_pickingLeftClick) {
            CancelLeftClickCapture();
            return;
        }
        HandleKeyTransition(vk, !isUp);
    } else if (raw->header.dwType == RIM_TYPEMOUSE) {
        const USHORT flags = raw->data.mouse.usButtonFlags;
        if (flags == 0) {
            return;
        }
        if ((flags & RI_MOUSE_LEFT_BUTTON_DOWN) && m_pickingLeftClick) {
            POINT pt{};
            GetCursorPos(&pt);
            HWND under = WindowFromPoint(pt);
            if (under) under = GetAncestor(under, GA_ROOT);
            auto cb = std::move(m_leftClickCallback);
            m_pickingLeftClick        = false;
            m_leftClickCancelCallback = nullptr;
            if (cb) {
                cb(pt, under);
            }
            return;
        }
        if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) HandleMouseTransition(VK_MBUTTON, true);
        if (flags & RI_MOUSE_MIDDLE_BUTTON_UP)   HandleMouseTransition(VK_MBUTTON, false);
        if (flags & RI_MOUSE_BUTTON_4_DOWN)      HandleMouseTransition(VK_XBUTTON1, true);
        if (flags & RI_MOUSE_BUTTON_4_UP)        HandleMouseTransition(VK_XBUTTON1, false);
        if (flags & RI_MOUSE_BUTTON_5_DOWN)      HandleMouseTransition(VK_XBUTTON2, true);
        if (flags & RI_MOUSE_BUTTON_5_UP)        HandleMouseTransition(VK_XBUTTON2, false);
    }
}

void HotkeyManager::HandleKeyTransition(uint16_t vk, bool isDown) {
    if (isDown) {
        const uint64_t nowMs = GetTickCount64();
        // Suppress OS typematic auto-repeat while held, but auto-heal if KEY_UP was dropped by UAC/Lock-Screen (>1200ms gap)
        if (m_keyDownState[vk] && (nowMs - m_keyLastDownMs[vk]) < 1200ULL) {
            m_keyLastDownMs[vk] = nowMs;
            return;
        }
        m_keyDownState[vk]  = true;
        m_keyLastDownMs[vk] = nowMs;

        if (m_capturing) {
            if (vk == VK_ESCAPE) {
                CancelCapture();
                return;
            }
            auto cb = std::move(m_captureCallback);
            m_capturing             = false;
            m_captureCancelCallback = nullptr;
            if (cb) {
                cb(HotkeyBinding{HotkeySource::Keyboard, vk});
            }
            return;
        }

        DispatchBinding(HotkeyBinding{HotkeySource::Keyboard, vk}, true);
    } else {
        m_keyDownState[vk] = false;
        if (!m_capturing) {
            DispatchBinding(HotkeyBinding{HotkeySource::Keyboard, vk}, false);
        }
    }
}

void HotkeyManager::HandleMouseTransition(uint16_t mouseVk, bool isDown) {
    if (isDown && m_capturing) {
        if (!m_captureAllowMouse) {
            return;
        }
        auto cb = std::move(m_captureCallback);
        m_capturing             = false;
        m_captureCancelCallback = nullptr;
        if (cb) {
            cb(HotkeyBinding{HotkeySource::Mouse, mouseVk});
        }
        return;
    }

    if (!m_capturing) {
        DispatchBinding(HotkeyBinding{HotkeySource::Mouse, mouseVk}, isDown);
    }
}

void HotkeyManager::DispatchBinding(const HotkeyBinding& binding, bool isDown) {
    if (isDown && binding == m_panicBinding) {
        TriggerPanicStop();
        return;
    }

    if (binding == m_cpsBinding) {
        if (isDown && m_callbacks.onCpsPress)     m_callbacks.onCpsPress();
        if (!isDown && m_callbacks.onCpsRelease)  m_callbacks.onCpsRelease();
    }

    if (binding == m_kpsBinding) {
        if (isDown && m_callbacks.onKpsPress)     m_callbacks.onKpsPress();
        if (!isDown && m_callbacks.onKpsRelease)  m_callbacks.onKpsRelease();
    }
}

} // namespace cadence
