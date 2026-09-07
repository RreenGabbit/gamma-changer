#pragma once

#include <array>
#include <cstdint>

namespace gamma_changer {

// Windows virtual-key codes, with left/right modifier keys resolved before use.
constexpr bool IsSingleKeyAllowed(const std::uint32_t key) noexcept {
    return key >= 0x08U && key <= 0xFEU &&
           key != 0x10U && key != 0x11U && key != 0x12U;
}

enum class SingleKeyAction { None, Captured, Toggle };

struct SingleKeyResult {
    bool suppress = false;
    SingleKeyAction action = SingleKeyAction::None;
};

class SingleKeyInput {
public:
    void SetBinding(const std::uint32_t key) noexcept {
        boundKey_ = IsSingleKeyAllowed(key) ? key : 0U;
    }

    std::uint32_t BoundKey() const noexcept { return boundKey_; }
    void BeginCapture() noexcept { capturing_ = true; }
    void CancelCapture() noexcept { capturing_ = false; }
    bool IsCapturing() const noexcept { return capturing_; }

    SingleKeyResult Process(
        const std::uint32_t key,
        const bool keyDown,
        const bool allowToggle = true) noexcept {
        if (!IsSingleKeyAllowed(key)) {
            return {};
        }
        if (!keyDown) {
            const bool suppressed = suppressedKeys_[key];
            suppressedKeys_[key] = false;
            return {suppressed, SingleKeyAction::None};
        }
        if (suppressedKeys_[key]) {
            return {true, SingleKeyAction::None};
        }
        if (capturing_) {
            capturing_ = false;
            suppressedKeys_[key] = true;
            return {true, SingleKeyAction::Captured};
        }
        if (allowToggle && key == boundKey_) {
            suppressedKeys_[key] = true;
            return {true, SingleKeyAction::Toggle};
        }
        return {};
    }

private:
    std::uint32_t boundKey_ = 0U;
    bool capturing_ = false;
    // Keep consuming repeats and the matching release even if the binding is
    // changed or cleared while a captured/bound key is still held.
    std::array<bool, 256> suppressedKeys_{};
};

}  // namespace gamma_changer
