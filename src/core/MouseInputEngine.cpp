#include "core/MouseInputEngine.hpp"

#include <algorithm>

namespace cadence {

MouseInputEngine::~MouseInputEngine() {
    ReleaseAll();
}

std::atomic<uint64_t>& MouseInputEngine::InjectionEpoch() noexcept {
    static std::atomic<uint64_t> epoch{0};
    return epoch;
}

std::atomic<bool>& MouseInputEngine::IsInjecting() noexcept {
    static std::atomic<bool> active{false};
    return active;
}

DWORD MouseInputEngine::ButtonDownFlag(MouseButton button) noexcept {
    switch (button) {
        case MouseButton::Left:   return MOUSEEVENTF_LEFTDOWN;
        case MouseButton::Right:  return MOUSEEVENTF_RIGHTDOWN;
        case MouseButton::Middle: return MOUSEEVENTF_MIDDLEDOWN;
    }
    return MOUSEEVENTF_LEFTDOWN;
}

DWORD MouseInputEngine::ButtonUpFlag(MouseButton button) noexcept {
    switch (button) {
        case MouseButton::Left:   return MOUSEEVENTF_LEFTUP;
        case MouseButton::Right:  return MOUSEEVENTF_RIGHTUP;
        case MouseButton::Middle: return MOUSEEVENTF_MIDDLEUP;
    }
    return MOUSEEVENTF_LEFTUP;
}

void MouseInputEngine::ApplyAbsoluteVirtualCoords(MOUSEINPUT& mi, int32_t screenX, int32_t screenY) noexcept {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    const int vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);

    const double nx = static_cast<double>(screenX - vx) * 65535.0 / static_cast<double>(vw);
    const double ny = static_cast<double>(screenY - vy) * 65535.0 / static_cast<double>(vh);

    mi.dx      = static_cast<LONG>(std::clamp(nx, 0.0, 65535.0));
    mi.dy      = static_cast<LONG>(std::clamp(ny, 0.0, 65535.0));
    mi.dwFlags |= MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
}

bool MouseInputEngine::SendDown(MouseButton button, CursorMode cursorMode, int32_t lockX, int32_t lockY, bool microWiggle) noexcept {
    INPUT inputs[3]{};
    UINT count = 0;

    if (cursorMode == CursorMode::LockedPoint) {
        inputs[count].type           = INPUT_MOUSE;
        inputs[count].mi.dwExtraInfo = kCadenceInjectedTag;
        ApplyAbsoluteVirtualCoords(inputs[count].mi, lockX, lockY);
        ++count;

        if (microWiggle) {
            // Subtle relative nudge so game UI hover states (e.g. Roblox GuiButton) trigger reliably
            inputs[count].type           = INPUT_MOUSE;
            inputs[count].mi.dx          = 1;
            inputs[count].mi.dy          = 0;
            inputs[count].mi.dwFlags     = MOUSEEVENTF_MOVE;
            inputs[count].mi.dwExtraInfo = kCadenceInjectedTag;
            ++count;
        }
    }

    inputs[count].type           = INPUT_MOUSE;
    inputs[count].mi.dwFlags     = ButtonDownFlag(button);
    inputs[count].mi.dwExtraInfo = kCadenceInjectedTag;
    if (cursorMode == CursorMode::LockedPoint && !microWiggle) {
        // Combine into single event when no wiggle needed
        ApplyAbsoluteVirtualCoords(inputs[0].mi, lockX, lockY);
        inputs[0].mi.dwFlags |= ButtonDownFlag(button);
        count = 1;
    } else {
        ++count;
    }

    IsInjecting().store(true, std::memory_order_release);
    InjectionEpoch().fetch_add(1, std::memory_order_acq_rel);
    const UINT sent = SendInput(count, inputs, sizeof(INPUT));
    IsInjecting().store(false, std::memory_order_release);

    if (sent == count) {
        const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(button));
        m_heldMask.fetch_or(bit, std::memory_order_acq_rel);
        return true;
    }
    return false;
}

bool MouseInputEngine::SendUp(MouseButton button, CursorMode cursorMode, int32_t lockX, int32_t lockY) noexcept {
    INPUT input{};
    input.type           = INPUT_MOUSE;
    input.mi.dwFlags     = ButtonUpFlag(button);
    input.mi.dwExtraInfo = kCadenceInjectedTag;

    if (cursorMode == CursorMode::LockedPoint) {
        ApplyAbsoluteVirtualCoords(input.mi, lockX, lockY);
    }

    IsInjecting().store(true, std::memory_order_release);
    InjectionEpoch().fetch_add(1, std::memory_order_acq_rel);
    const UINT sent = SendInput(1, &input, sizeof(INPUT));
    IsInjecting().store(false, std::memory_order_release);

    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(button));
    m_heldMask.fetch_and(static_cast<uint8_t>(~bit), std::memory_order_acq_rel);
    return sent == 1;
}

void MouseInputEngine::ReleaseAll() noexcept {
    const uint8_t mask = m_heldMask.exchange(0, std::memory_order_acq_rel);
    if (mask == 0) {
        return;
    }

    INPUT inputs[3]{};
    UINT count = 0;

    if (mask & (1u << 0)) {
        inputs[count].type           = INPUT_MOUSE;
        inputs[count].mi.dwFlags     = MOUSEEVENTF_LEFTUP;
        inputs[count].mi.dwExtraInfo = kCadenceInjectedTag;
        ++count;
    }
    if (mask & (1u << 1)) {
        inputs[count].type           = INPUT_MOUSE;
        inputs[count].mi.dwFlags     = MOUSEEVENTF_RIGHTUP;
        inputs[count].mi.dwExtraInfo = kCadenceInjectedTag;
        ++count;
    }
    if (mask & (1u << 2)) {
        inputs[count].type           = INPUT_MOUSE;
        inputs[count].mi.dwFlags     = MOUSEEVENTF_MIDDLEUP;
        inputs[count].mi.dwExtraInfo = kCadenceInjectedTag;
        ++count;
    }

    if (count > 0) {
        IsInjecting().store(true, std::memory_order_release);
        InjectionEpoch().fetch_add(1, std::memory_order_acq_rel);
        SendInput(count, inputs, sizeof(INPUT));
        IsInjecting().store(false, std::memory_order_release);
    }
}

} // namespace cadence
