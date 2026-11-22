#pragma once

#include "core/Types.hpp"

#include <atomic>
#include <cstdint>

namespace cadence {

// Unique tag embedded in SendInput dwExtraInfo to identify our own synthetic events
inline constexpr ULONG_PTR kCadenceInjectedTag = static_cast<ULONG_PTR>(0x434144454E43454AULL);

class MouseInputEngine {
public:
    MouseInputEngine() = default;
    ~MouseInputEngine();

    MouseInputEngine(const MouseInputEngine&)            = delete;
    MouseInputEngine& operator=(const MouseInputEngine&) = delete;

    // Emit mouse down / up events. Tracks held state for guaranteed safety release.
    bool SendDown(MouseButton button, CursorMode cursorMode, int32_t lockX, int32_t lockY, bool microWiggle = false) noexcept;
    bool SendUp(MouseButton button, CursorMode cursorMode, int32_t lockX, int32_t lockY) noexcept;

    // Immediately releases any buttons currently held by this engine
    void ReleaseAll() noexcept;

    // Monotonic counter incremented around SendInput calls (helps Raw Input filter synthetic events)
    static std::atomic<uint64_t>& InjectionEpoch() noexcept;
    static std::atomic<bool>&     IsInjecting() noexcept;

private:
    static DWORD ButtonDownFlag(MouseButton button) noexcept;
    static DWORD ButtonUpFlag(MouseButton button) noexcept;
    static void  ApplyAbsoluteVirtualCoords(MOUSEINPUT& mi, int32_t screenX, int32_t screenY) noexcept;

    std::atomic<uint8_t> m_heldMask{0}; // bit 0=Left, bit 1=Right, bit 2=Middle
};

} // namespace cadence
