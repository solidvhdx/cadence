#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace cadence {

enum class OperatingMode : uint8_t {
    CPS = 0, // Mouse Autoclicker
    KPS = 1  // Keyboard Auto-Presser
};

enum class MouseButton : uint8_t {
    Left   = 0,
    Right  = 1,
    Middle = 2
};

enum class ClickPattern : uint8_t {
    Single      = 0,
    Double      = 1,
    TripleBurst = 2,
    CustomBurst = 3
};

enum class CursorMode : uint8_t {
    FollowCursor = 0,
    LockedPoint  = 1
};

enum class ActivationMode : uint8_t {
    Toggle = 0,
    Hold   = 1
};

enum class RepeatLimitMode : uint8_t {
    Unlimited = 0,
    Count     = 1,
    Duration  = 2 // seconds
};

enum class KeyInputMode : uint8_t {
    SingleOrChord = 0,
    Sequence      = 1
};

enum class KeyDispatchMethod : uint8_t {
    ScanCode   = 0, // KEYEVENTF_SCANCODE (DirectInput / Games)
    VirtualKey = 1  // Standard VK dispatch
};

enum class EngineState : uint8_t {
    Idle   = 0,
    Armed  = 1, // Waiting for user to leave Cadence window (self-click protection)
    Active = 2,
    Paused = 3
};

// Supports both Keyboard VKs and Mouse buttons (X1/X2/Middle) as global hotkeys via Raw Input
enum class HotkeySource : uint8_t {
    Keyboard = 0,
    Mouse    = 1
};

struct HotkeyBinding {
    HotkeySource source = HotkeySource::Keyboard;
    uint16_t     code   = VK_F6; // VK_* for Keyboard; VK_MBUTTON / VK_XBUTTON1 / VK_XBUTTON2 for Mouse

    [[nodiscard]] bool operator==(const HotkeyBinding& other) const noexcept = default;
};

struct CpsConfig {
    double          rateCps         = 14.5;
    bool            displayAsMs     = false;
    MouseButton     button          = MouseButton::Left;
    ClickPattern    pattern         = ClickPattern::Single;
    uint32_t        burstCount      = 3;
    uint32_t        burstCooldownMs = 120;
    uint32_t        holdDurationMs  = 20;
    bool            jitterEnabled   = true;
    double          jitterMs        = 5.0;
    ActivationMode  activation      = ActivationMode::Toggle;
    HotkeyBinding   hotkey          = {HotkeySource::Keyboard, VK_F6};
    CursorMode      cursorMode        = CursorMode::FollowCursor;
    int32_t         lockX             = 960;
    int32_t         lockY             = 540;
    bool            lockedMicroWiggle = true;  // Subtle +-1px relative nudge for game UIs (e.g. Roblox GuiButton)
    RepeatLimitMode limitMode         = RepeatLimitMode::Unlimited;
    uint32_t        limitValue        = 500;
};

struct KpsConfig {
    double            rateKps         = 10.0;
    bool              displayAsMs     = false;
    KeyInputMode      inputMode       = KeyInputMode::SingleOrChord;
    uint16_t          targetVk        = 'E';
    bool              modCtrl         = false;
    bool              modShift        = false;
    bool              modAlt          = false;
    std::vector<uint16_t> sequenceVks = {'1', '2', '3', 'E'};
    uint32_t          pressDurationMs = 30;
    bool              jitterEnabled   = true;
    double            jitterMs        = 6.0;
    KeyDispatchMethod dispatchMethod  = KeyDispatchMethod::ScanCode;
    ActivationMode    activation      = ActivationMode::Hold;
    HotkeyBinding     hotkey          = {HotkeySource::Keyboard, VK_F7};
    bool              releaseOnStop   = true;
    RepeatLimitMode   limitMode       = RepeatLimitMode::Unlimited;
    uint32_t          limitValue      = 200;
};

struct GlobalSettings {
    bool          hybridQpcTimer       = true;  // High-res WaitableTimer + bounded QPC spin
    bool          disablePowerThrottle = true;  // Win11 PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    bool          mmcssPriority        = true;  // AvSetMmThreadCharacteristicsW("Pro Audio") while active
    bool          stopOnSelfFocus      = true;  // Never click / auto-stop when Cadence window is focused
    bool          targetWindowLock     = false; // Only click/press while targetProcessName is foreground
    std::string   targetProcessName    = "";    // e.g. "RobloxPlayerBeta.exe"
    bool          minimizeToTray       = true;
    bool          alwaysOnTop          = false;
    bool          checkForUpdates      = true;  // Check GitHub Releases for new versions on startup
    HotkeyBinding panicHotkey          = {HotkeySource::Keyboard, VK_F12};
};

struct Profile {
    std::string   id;
    std::string   name;
    OperatingMode mode = OperatingMode::CPS;
    CpsConfig     cps{};
    KpsConfig     kps{};
};

struct AppConfig {
    OperatingMode        activeMode      = OperatingMode::CPS;
    std::string          activeProfileId = "default";
    CpsConfig            cps{};
    KpsConfig            kps{};
    GlobalSettings       settings{};
    std::vector<Profile> profiles{};
};

struct TelemetrySnapshot {
    double   actualRate     = 0.0; // Real measured CPS or KPS
    double   avgIntervalMs  = 0.0;
    double   jitterStdDevMs = 0.0;
    double   p99ErrorMs     = 0.0;
    uint64_t sessionCount   = 0;
    double   cpuPercent     = 0.0;
    double   memoryMb       = 0.0;
    std::array<float, 40> history{};
    size_t   historyCount   = 0;
};

// Human-readable key / hotkey formatting
[[nodiscard]] inline std::wstring FormatVkName(uint16_t vk) {
    switch (vk) {
        case VK_LBUTTON:  return L"LMB";
        case VK_RBUTTON:  return L"RMB";
        case VK_MBUTTON:  return L"MMB";
        case VK_XBUTTON1: return L"X1";
        case VK_XBUTTON2: return L"X2";
        case VK_SPACE:    return L"Space";
        case VK_RETURN:   return L"Enter";
        case VK_TAB:      return L"Tab";
        case VK_BACK:     return L"Bksp";
        case VK_ESCAPE:   return L"Esc";
        case VK_CAPITAL:  return L"Caps";
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:   return L"Shift";
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL: return L"Ctrl";
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:    return L"Alt";
        case VK_INSERT:   return L"Ins";
        case VK_DELETE:   return L"Del";
        case VK_HOME:     return L"Home";
        case VK_END:      return L"End";
        case VK_PRIOR:    return L"PgUp";
        case VK_NEXT:     return L"PgDn";
        case VK_UP:       return L"Up";
        case VK_DOWN:     return L"Down";
        case VK_LEFT:     return L"Left";
        case VK_RIGHT:    return L"Right";
        case VK_OEM_3:    return L"`";
        case VK_OEM_MINUS:return L"-";
        case VK_OEM_PLUS: return L"=";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return std::format(L"F{}", vk - VK_F1 + 1);
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return std::format(L"Num{}", vk - VK_NUMPAD0);
    }
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t buf[32] = {};
    if (scan != 0 && GetKeyNameTextW(static_cast<LONG>(scan << 16), buf, 32) > 0) {
        return std::wstring(buf);
    }
    return std::format(L"0x{:02X}", vk);
}

[[nodiscard]] inline std::wstring FormatHotkey(const HotkeyBinding& hk) {
    if (hk.source == HotkeySource::Mouse) {
        switch (hk.code) {
            case VK_MBUTTON:  return L"Mouse M";
            case VK_XBUTTON1: return L"Mouse X1";
            case VK_XBUTTON2: return L"Mouse X2";
            default:          return L"Mouse";
        }
    }
    return FormatVkName(hk.code);
}

} // namespace cadence
