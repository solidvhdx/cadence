#include "core/MouseInputEngine.hpp"

#include <algorithm>
#include <cmath>

namespace cadence {

MouseInputEngine::~MouseInputEngine() {
    ReleaseAll();
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
    const int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));

    // Pixel-center mapping avoids off-by-1 truncation in Windows' (dx * vw) >> 16 conversion
    const double nx = (static_cast<double>(screenX - vx) + 0.5) * 65536.0 / static_cast<double>(vw);
    const double ny = (static_cast<double>(screenY - vy) + 0.5) * 65536.0 / static_cast<double>(vh);

    mi.dx      = static_cast<LONG>(std::clamp(std::floor(nx), 0.0, 65535.0));
    mi.dy      = static_cast<LONG>(std::clamp(std::floor(ny), 0.0, 65535.0));
    mi.dwFlags |= MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
}

bool MouseInputEngine::SendDown(MouseButton button, CursorMode cursorMode, int32_t lockX, int32_t lockY, bool microWiggle) noexcept {
    INPUT inputs[3]{};
    UINT count = 0;

    if (cursorMode == CursorMode::LockedPoint && microWiggle) {
        // Move to 1px adjacent pixel then nudge back onto exact (lockX, lockY) so game UI hover fires AND click lands on exact pixel
        const int vx        = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int32_t preX  = (lockX > vx) ? (lockX - 1) : (lockX + 1);
        const LONG nudgeDx  = (lockX > vx) ? 1 : -1;

        inputs[count].type           = INPUT_MOUSE;
        inputs[count].mi.dwExtraInfo = kCadenceInjectedTag;
        ApplyAbsoluteVirtualCoords(inputs[count].mi, preX, lockY);
        ++count;

        inputs[count].type           = INPUT_MOUSE;
        inputs[count].mi.dx          = nudgeDx;
        inputs[count].mi.dy          = 0;
        inputs[count].mi.dwFlags     = MOUSEEVENTF_MOVE;
        inputs[count].mi.dwExtraInfo = kCadenceInjectedTag;
        ++count;

        inputs[count].type           = INPUT_MOUSE;
        inputs[count].mi.dwFlags     = ButtonDownFlag(button);
        inputs[count].mi.dwExtraInfo = kCadenceInjectedTag;
        ++count;
    } else {
        inputs[0].type           = INPUT_MOUSE;
        inputs[0].mi.dwFlags     = ButtonDownFlag(button);
        inputs[0].mi.dwExtraInfo = kCadenceInjectedTag;
        if (cursorMode == CursorMode::LockedPoint) {
            ApplyAbsoluteVirtualCoords(inputs[0].mi, lockX, lockY);
        }
        count = 1;
    }

    const UINT sent = SendInput(count, inputs, sizeof(INPUT));

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

    const UINT sent = SendInput(1, &input, sizeof(INPUT));

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
        SendInput(count, inputs, sizeof(INPUT));
    }
}

} // namespace cadence
