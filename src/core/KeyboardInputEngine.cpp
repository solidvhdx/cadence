#include "core/KeyboardInputEngine.hpp"
#include "core/MouseInputEngine.hpp"

#include <vector>

namespace cadence {

KeyboardInputEngine::~KeyboardInputEngine() {
    ReleaseAll();
}

bool KeyboardInputEngine::IsExtendedKey(uint16_t vk) noexcept {
    switch (vk) {
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_UP:
        case VK_RIGHT:
        case VK_DOWN:
        case VK_NUMLOCK:
        case VK_CANCEL:
        case VK_SNAPSHOT:
        case VK_DIVIDE:
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
            return true;
        default:
            return false;
    }
}

INPUT KeyboardInputEngine::BuildKeyInput(uint16_t vk, bool keyUp, KeyDispatchMethod method) noexcept {
    INPUT input{};
    input.type           = INPUT_KEYBOARD;
    input.ki.dwExtraInfo = kCadenceInjectedTag;

    if (method == KeyDispatchMethod::ScanCode) {
        // Use the active foreground window's keyboard layout (not Cadence's worker thread layout)
        DWORD fgTid = 0;
        if (HWND fg = GetForegroundWindow()) {
            fgTid = GetWindowThreadProcessId(fg, nullptr);
        }
        const HKL layout = GetKeyboardLayout(fgTid);
        const UINT sc = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, layout);
        if (sc != 0) {
            input.ki.wScan   = static_cast<WORD>(sc & 0xFF);
            input.ki.dwFlags = KEYEVENTF_SCANCODE;
            if ((sc & 0xFF00) == 0xE000 || (sc & 0xFF00) == 0xE100 || IsExtendedKey(vk)) {
                input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            }
        } else {
            // Fallback to VK if layout has no scan code mapping
            input.ki.wVk = vk;
            if (IsExtendedKey(vk)) {
                input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            }
        }
    } else {
        input.ki.wVk = vk;
        if (IsExtendedKey(vk)) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
    }

    if (keyUp) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    return input;
}

void KeyboardInputEngine::MarkHeld(uint16_t vk, KeyDispatchMethod method) noexcept {
    const uint64_t bit = 1ULL << (vk & 63);
    if (method == KeyDispatchMethod::VirtualKey) {
        m_heldVkMethodBits[vk >> 6].fetch_or(bit, std::memory_order_acq_rel);
    } else {
        m_heldVkMethodBits[vk >> 6].fetch_and(~bit, std::memory_order_acq_rel);
    }
    m_heldBits[vk >> 6].fetch_or(bit, std::memory_order_acq_rel);
}

void KeyboardInputEngine::MarkReleased(uint16_t vk) noexcept {
    const uint64_t bit = 1ULL << (vk & 63);
    m_heldBits[vk >> 6].fetch_and(~bit, std::memory_order_acq_rel);
}

bool KeyboardInputEngine::SendKeyDown(uint16_t vk, KeyDispatchMethod method) noexcept {
    if (vk == 0 || vk >= 256) {
        return false;
    }
    INPUT input = BuildKeyInput(vk, false, method);
    const UINT sent = SendInput(1, &input, sizeof(INPUT));

    if (sent == 1) {
        MarkHeld(vk, method);
        return true;
    }
    return false;
}

bool KeyboardInputEngine::SendKeyUp(uint16_t vk, KeyDispatchMethod method) noexcept {
    if (vk == 0 || vk >= 256) {
        return false;
    }
    INPUT input = BuildKeyInput(vk, true, method);
    const UINT sent = SendInput(1, &input, sizeof(INPUT));

    MarkReleased(vk);
    return sent == 1;
}

bool KeyboardInputEngine::SendChordDown(uint16_t primaryVk, bool ctrl, bool shift, bool alt, KeyDispatchMethod method) noexcept {
    if (primaryVk == 0 || primaryVk >= 256) {
        return false;
    }

    INPUT batch[4]{};
    uint16_t vks[4]{};
    UINT count = 0;

    if (ctrl && primaryVk != VK_CONTROL && primaryVk != VK_LCONTROL) {
        vks[count] = VK_LCONTROL;
        batch[count++] = BuildKeyInput(VK_LCONTROL, false, method);
    }
    if (shift && primaryVk != VK_SHIFT && primaryVk != VK_LSHIFT) {
        vks[count] = VK_LSHIFT;
        batch[count++] = BuildKeyInput(VK_LSHIFT, false, method);
    }
    if (alt && primaryVk != VK_MENU && primaryVk != VK_LMENU) {
        vks[count] = VK_LMENU;
        batch[count++] = BuildKeyInput(VK_LMENU, false, method);
    }
    vks[count] = primaryVk;
    batch[count++] = BuildKeyInput(primaryVk, false, method);

    const UINT sent = SendInput(count, batch, sizeof(INPUT));

    for (UINT i = 0; i < sent; ++i) {
        MarkHeld(vks[i], method);
    }
    return sent == count;
}

bool KeyboardInputEngine::SendChordUp(uint16_t primaryVk, bool ctrl, bool shift, bool alt, KeyDispatchMethod method) noexcept {
    if (primaryVk == 0 || primaryVk >= 256) {
        return false;
    }

    INPUT batch[4]{};
    uint16_t vks[4]{};
    UINT count = 0;

    // Release primary key first, then modifiers in reverse order
    vks[count] = primaryVk;
    batch[count++] = BuildKeyInput(primaryVk, true, method);

    if (alt && primaryVk != VK_MENU && primaryVk != VK_LMENU) {
        vks[count] = VK_LMENU;
        batch[count++] = BuildKeyInput(VK_LMENU, true, method);
    }
    if (shift && primaryVk != VK_SHIFT && primaryVk != VK_LSHIFT) {
        vks[count] = VK_LSHIFT;
        batch[count++] = BuildKeyInput(VK_LSHIFT, true, method);
    }
    if (ctrl && primaryVk != VK_CONTROL && primaryVk != VK_LCONTROL) {
        vks[count] = VK_LCONTROL;
        batch[count++] = BuildKeyInput(VK_LCONTROL, true, method);
    }

    const UINT sent = SendInput(count, batch, sizeof(INPUT));

    for (UINT i = 0; i < count; ++i) {
        MarkReleased(vks[i]);
    }
    return sent == count;
}

void KeyboardInputEngine::ReleaseAll() noexcept {
    std::vector<INPUT> inputs;
    for (size_t word = 0; word < 4; ++word) {
        uint64_t bits       = m_heldBits[word].exchange(0, std::memory_order_acq_rel);
        const uint64_t vkM  = m_heldVkMethodBits[word].load(std::memory_order_acquire);
        while (bits != 0) {
            unsigned long bitIndex = 0;
            _BitScanForward64(&bitIndex, bits);
            const uint16_t vk = static_cast<uint16_t>((word << 6) + bitIndex);
            const KeyDispatchMethod method = ((vkM & (1ULL << bitIndex)) != 0)
                ? KeyDispatchMethod::VirtualKey
                : KeyDispatchMethod::ScanCode;
            inputs.push_back(BuildKeyInput(vk, true, method));
            bits &= (bits - 1);
        }
    }

    if (!inputs.empty()) {
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
}

} // namespace cadence
