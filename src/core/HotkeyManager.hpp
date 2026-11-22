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

    // Capture mode for rebinding hotkeys or KPS target key
    void BeginCapture(bool allowMouseButtons, std::function<void(HotkeyBinding)> onCaptured);
    void CancelCapture();
    [[nodiscard]] bool IsCapturing() const noexcept { return m_capturing; }

    // Global physical Left-Click capture (for multi-monitor Coordinate Pick & Target Window Lock)
    void BeginGlobalLeftClickCapture(std::function<void(POINT, HWND)> onPicked);
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

    HWND            m_hwnd = nullptr;
    HotkeyCallbacks m_callbacks{};
    HotkeyBinding   m_cpsBinding{HotkeySource::Keyboard, VK_F6};
    HotkeyBinding   m_kpsBinding{HotkeySource::Keyboard, VK_F7};
    HotkeyBinding   m_panicBinding{HotkeySource::Keyboard, VK_F12};
    bool            m_panicRegistered = false;

    // Tracks physical key-down state to ignore Windows auto-repeat in Raw Input
    std::array<bool, 256> m_keyDownState{};

    // Interactive binding capture
    bool                               m_capturing         = false;
    bool                               m_captureAllowMouse = true;
    std::function<void(HotkeyBinding)> m_captureCallback;

    // Global physical left-click capture (Coordinate Pick / Target Window Pick)
    bool                             m_pickingLeftClick = false;
    std::function<void(POINT, HWND)> m_leftClickCallback;
};

} // namespace cadence
