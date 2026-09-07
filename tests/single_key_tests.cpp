#include "single_key.h"

#ifdef _WIN32
#include "single_key_win32.h"
#endif

#include <cstdlib>
#include <iostream>

using gamma_changer::SingleKeyAction;
using gamma_changer::SingleKeyInput;
using gamma_changer::SingleKeyResult;

namespace {

constexpr std::uint32_t kLeftShift = 0xA0U;
constexpr std::uint32_t kRightShift = 0xA1U;
constexpr std::uint32_t kLeftCtrl = 0xA2U;
constexpr std::uint32_t kA = 0x41U;

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void Expect(
    const SingleKeyResult result,
    const bool suppress,
    const SingleKeyAction action,
    const char* message) {
    Require(result.suppress == suppress && result.action == action, message);
}

void TestRightShiftAndRepeats() {
    SingleKeyInput input;
    input.SetBinding(kRightShift);
    Expect(input.Process(kLeftShift, true), false, SingleKeyAction::None,
           "Left Shift does not activate a Right Shift binding");
    Expect(input.Process(kRightShift, true), true, SingleKeyAction::Toggle,
           "Right Shift toggles gamma and is consumed");
    for (int repeat = 0; repeat < 10; ++repeat) {
        Expect(input.Process(kRightShift, true), true, SingleKeyAction::None,
               "holding Right Shift does not repeatedly toggle gamma");
    }
    Expect(input.Process(kRightShift, false), true, SingleKeyAction::None,
           "the dedicated modifier's release is consumed");
    Expect(input.Process(kLeftShift, false), false, SingleKeyAction::None,
           "unbound Left Shift releases normally");
    Expect(input.Process(kRightShift, true), true, SingleKeyAction::Toggle,
           "the next Right Shift press toggles gamma again");
}

void TestSingleModifierCapture() {
    const std::array<std::uint32_t, 8> modifiers = {
        0xA0U, 0xA1U, 0xA2U, 0xA3U, 0xA4U, 0xA5U, 0x5BU, 0x5CU,
    };
    for (const auto key : modifiers) {
        SingleKeyInput input;
        input.SetBinding(kA);
        input.BeginCapture();
        Expect(input.Process(key, true, false), true, SingleKeyAction::Captured,
               "a modifier by itself is recorded and suppressed");
        Require(!input.IsCapturing(), "recording stops after exactly one key");
        Require(input.BoundKey() == kA, "recording does not save a new binding implicitly");
        Expect(input.Process(key, true, false), true, SingleKeyAction::None,
               "a recorded key's repeats are suppressed");
        Expect(input.Process(kA, true, false), false, SingleKeyAction::None,
               "a second key cannot create a combination or toggle while editing");
        Expect(input.Process(key, false, false), true, SingleKeyAction::None,
               "recording consumes the matching modifier release");
        Expect(input.Process(kA, false, false), false, SingleKeyAction::None,
               "the second key's release passes through");
        input.SetBinding(key);
        Expect(input.Process(key, true), true, SingleKeyAction::Toggle,
               "each saved modifier works as a standalone binding");
    }
}

void TestRecordingExistingBinding() {
    SingleKeyInput input;
    input.SetBinding(kRightShift);
    input.BeginCapture();
    Expect(input.Process(kRightShift, true, false), true, SingleKeyAction::Captured,
           "recording the current shortcut does not toggle gamma");
    input.SetBinding(kRightShift);
    Expect(input.Process(kRightShift, true), true, SingleKeyAction::None,
           "saving a still-held recorded key does not toggle gamma");
    Expect(input.Process(kRightShift, false), true, SingleKeyAction::None,
           "saving preserves the recorded key's release suppression");
    Expect(input.Process(kRightShift, true), true, SingleKeyAction::Toggle,
           "the saved shortcut activates on its next press");
}

void TestRebindAndClearWhileHeld() {
    SingleKeyInput input;
    input.SetBinding(kRightShift);
    input.Process(kRightShift, true);
    input.SetBinding(kLeftShift);
    Expect(input.Process(kRightShift, true), true, SingleKeyAction::None,
           "rebinding does not leak repeats from the previous dedicated key");
    Expect(input.Process(kRightShift, false), true, SingleKeyAction::None,
           "rebinding still consumes the previous key's release");
    Expect(input.Process(kRightShift, true), false, SingleKeyAction::None,
           "the previous key works normally after release");
    Expect(input.Process(kLeftShift, true), true, SingleKeyAction::Toggle,
           "the new binding toggles gamma");
    input.SetBinding(0U);
    Expect(input.Process(kLeftShift, false), true, SingleKeyAction::None,
           "clearing a held binding still consumes its release");
    Expect(input.Process(kLeftShift, true), false, SingleKeyAction::None,
           "cleared keys resume normal behavior on the next press");
}

void TestHeldModifiersAndCancellation() {
    SingleKeyInput input;
    input.SetBinding(kRightShift);
    Expect(input.Process(kLeftCtrl, true), false, SingleKeyAction::None,
           "unbound modifiers remain available to other applications");
    input.BeginCapture();
    input.CancelCapture();
    Expect(input.Process(kRightShift, true), true, SingleKeyAction::Toggle,
           "cancelling recording restores the shortcut even with Ctrl held");
    Expect(input.Process(kRightShift, false), true, SingleKeyAction::None,
           "the shortcut release is suppressed with Ctrl held");
    Expect(input.Process(kLeftCtrl, false), false, SingleKeyAction::None,
           "the unbound modifier's release is not suppressed");
}

void TestDisabledAndInvalidKeys() {
    SingleKeyInput input;
    Expect(input.Process(kRightShift, true), false, SingleKeyAction::None,
           "no binding leaves normal keyboard input alone");
    input.BeginCapture();
    for (const auto key : {0U, 1U, 0x10U, 0x11U, 0x12U, 0xFFU, 0x100U}) {
        input.SetBinding(key);
        Require(input.BoundKey() == 0U, "invalid or unresolved keys cannot be bound");
        Expect(input.Process(key, true), false, SingleKeyAction::None,
               "invalid input is ignored without consuming the recording");
    }
    Expect(input.Process(kRightShift, true, false), true, SingleKeyAction::Captured,
           "Right Shift can be recorded with no existing binding");
}

#ifdef _WIN32
void TestWindowsModifierResolution() {
    static_assert(kRightShift == VK_RSHIFT && kLeftShift == VK_LSHIFT);
    struct Mapping { DWORD key; DWORD scan; DWORD flags; UINT expected; };
    const std::array<Mapping, 7> mappings = {{
        {VK_SHIFT, 0x36U, 0U, VK_RSHIFT},
        {VK_SHIFT, 0x2AU, 0U, VK_LSHIFT},
        {VK_CONTROL, 0x1DU, 0U, VK_LCONTROL},
        {VK_CONTROL, 0x1DU, LLKHF_EXTENDED, VK_RCONTROL},
        {VK_MENU, 0x38U, 0U, VK_LMENU},
        {VK_MENU, 0x38U, LLKHF_EXTENDED, VK_RMENU},
        {VK_RSHIFT, 0x36U, 0U, VK_RSHIFT},
    }};
    for (const auto& mapping : mappings) {
        KBDLLHOOKSTRUCT keyboard{};
        keyboard.vkCode = mapping.key;
        keyboard.scanCode = mapping.scan;
        keyboard.flags = mapping.flags;
        const UINT key = gamma_changer::SingleKeyFromHook(keyboard);
        Require(key == mapping.expected, "Windows scan codes preserve modifier side");
        SingleKeyInput input;
        input.BeginCapture();
        Expect(input.Process(key, true, false), true, SingleKeyAction::Captured,
               "the resolved Windows modifier is accepted by the recorder");
    }
    Require(gamma_changer::SingleKeyName(VK_RSHIFT) == L"Right Shift",
            "the key box identifies Right Shift explicitly");
    Require(gamma_changer::SingleKeyName(VK_LSHIFT) == L"Left Shift",
            "the key box identifies Left Shift explicitly");
}
#endif

}  // namespace

int main() {
    TestRightShiftAndRepeats();
    TestSingleModifierCapture();
    TestRecordingExistingBinding();
    TestRebindAndClearWhileHeld();
    TestHeldModifiersAndCancellation();
    TestDisabledAndInvalidKeys();
#ifdef _WIN32
    TestWindowsModifierResolution();
#endif
    std::cout << "single_key_tests: all tests passed\n";
}
