#pragma once

#include "core/Types.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace cadence {

class KeyboardInputEngine {
public:
    KeyboardInputEngine() = default;
    ~KeyboardInputEngine();

    KeyboardInputEngine(const KeyboardInputEngine&)            = delete;
    KeyboardInputEngine& operator=(const KeyboardInputEngine&) = delete;

    bool SendKeyDown(uint16_t vk, KeyDispatchMethod method) noexcept;
    bool SendKeyUp(uint16_t vk, KeyDispatchMethod method) noexcept;

    // Sends modifier keys down, then primary key down
    bool SendChordDown(uint16_t primaryVk, bool ctrl, bool shift, bool alt, KeyDispatchMethod method) noexcept;
    // Sends primary key up, then modifier keys up in reverse order
    bool SendChordUp(uint16_t primaryVk, bool ctrl, bool shift, bool alt, KeyDispatchMethod method) noexcept;

    // Guarantees every key pressed by this engine is cleanly released using its original dispatch method
    void ReleaseAll() noexcept;

private:
    static bool  IsExtendedKey(uint16_t vk) noexcept;
    static INPUT BuildKeyInput(uint16_t vk, bool keyUp, KeyDispatchMethod method) noexcept;
    void         MarkHeld(uint16_t vk, KeyDispatchMethod method) noexcept;
    void         MarkReleased(uint16_t vk) noexcept;

    // 256-bit atomic masks for VKs 0..255 (held state + whether VirtualKey dispatch was used)
    std::array<std::atomic<uint64_t>, 4> m_heldBits{};
    std::array<std::atomic<uint64_t>, 4> m_heldVkMethodBits{};
};

} // namespace cadence
