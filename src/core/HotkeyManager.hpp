#pragma once

#include "core/Types.hpp"

#include <array>
#include <cstdint>
#include <functional>

namespace cadence {

struct HotkeyCallbacks {
    std::function<void()> onCpsPress;
    std::function<void()> onCpsRelease;
    std::function<void()> onKpsPress;
    std::function<void()> onKpsRelease;
    std::function<void()> onPanicStop;
};

class HotkeyManager {
public:
    HotkeyManager() = default;
    ~HotkeyManager();

    HotkeyManager(const HotkeyManager&)            = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    // Registers Raw Input (RIDEV_INPUTSINK) on hwnd for zero-hook-lag global hotkeys
    bool Initialize(HWND hwnd, HotkeyCallbacks callbacks);
    void Shutdown();

    void SetBindings(const HotkeyBinding& cpsHotkey,
                     const HotkeyBinding& kpsHotkey,
                     const HotkeyBinding& panicHotkey);

    // RegisterHotKey fallback for the kill key (still works while an elevated window has focus).
    // RegisterHotKey takes the key away from every other app, so it is only held while the engine is
    // armed/active/paused; Raw Input handles the kill key at all other times.
    void SetPanicFallbackEnabled(bool enabled);

    // Capture mode for rebinding hotkeys or KPS target key
    void BeginCapture(bool allowMouseButtons,
                      std::function<void(HotkeyBinding)> onCaptured,
                      std::function<void()> onCancelled = nullptr);
    void CancelCapture();
    [[nodiscard]] bool IsCapturing() const noexcept { return m_capturing; }

    // Global physical Left-Click capture (for multi-monitor Coordinate Pick & Target Window Lock)
    void BeginGlobalLeftClickCapture(std::function<void(POINT, HWND)> onPicked,
                                     std::function<void()> onCancelled = nullptr);
    void CancelLeftClickCapture();
    [[nodiscard]] bool IsPickingLeftClick() const noexcept { return m_pickingLeftClick; }

    // Called by MainWindow WndProc on WM_INPUT and WM_HOTKEY
    void ProcessRawInput(LPARAM lParam);
    void ProcessWmHotkey(WPARAM wParam);

private:
    static constexpr int kPanicHotkeyId = 0x504E; // 'PN'

    void RegisterPanicFallback();
    void UnregisterPanicFallback();

    void HandleKeyTransition(uint16_t vk, bool isDown);
    void HandleMouseTransition(uint16_t mouseVk, bool isDown);
    void DispatchBinding(const HotkeyBinding& binding, bool isDown);
    void TriggerPanicStop();

    HWND            m_hwnd = nullptr;
    HotkeyCallbacks m_callbacks{};
    HotkeyBinding   m_cpsBinding{HotkeySource::Keyboard, VK_F6};
    HotkeyBinding   m_kpsBinding{HotkeySource::Keyboard, VK_F7};
    HotkeyBinding   m_panicBinding{HotkeySource::Keyboard, VK_F12};
    bool            m_panicRegistered = false;
    bool            m_panicFallbackWanted = false;
    uint64_t        m_lastPanicMs     = 0;

    // Tracks physical key-down state & timestamps to ignore OS auto-repeat while auto-healing dropped KEY_UP (UAC/UIPI)
    std::array<bool, 256>     m_keyDownState{};
    std::array<uint64_t, 256> m_keyLastDownMs{};

    // Interactive binding capture
    bool                               m_capturing         = false;
    bool                               m_captureAllowMouse = true;
    std::function<void(HotkeyBinding)> m_captureCallback;
    std::function<void()>              m_captureCancelCallback;

    // Global physical left-click capture (Coordinate Pick / Target Window Pick)
    bool                             m_pickingLeftClick = false;
    std::function<void(POINT, HWND)> m_leftClickCallback;
    std::function<void()>            m_leftClickCancelCallback;
};

} // namespace cadence
