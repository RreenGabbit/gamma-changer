#pragma once

#include <windows.h>

#include <iterator>
#include <string>

namespace gamma_changer {

inline UINT SingleKeyFromHook(const KBDLLHOOKSTRUCT& keyboard) noexcept {
    switch (keyboard.vkCode) {
        case VK_SHIFT:
        case VK_CONTROL:
        case VK_MENU: {
            const UINT scanCode = keyboard.scanCode |
                                  ((keyboard.flags & LLKHF_EXTENDED) != 0U ? 0xE000U : 0U);
            return MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX);
        }
        default:
            return keyboard.vkCode;
    }
}

inline std::wstring SingleKeyName(const UINT key) {
    switch (key) {
        case 0U: return L"Not set";
        case VK_LSHIFT: return L"Left Shift";
        case VK_RSHIFT: return L"Right Shift";
        case VK_LCONTROL: return L"Left Ctrl";
        case VK_RCONTROL: return L"Right Ctrl";
        case VK_LMENU: return L"Left Alt";
        case VK_RMENU: return L"Right Alt";
        case VK_LWIN: return L"Left Windows";
        case VK_RWIN: return L"Right Windows";
        default: break;
    }

    const UINT scanCode = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    const LONG keyFlags = static_cast<LONG>(
        ((scanCode & 0xFFU) << 16U) | ((scanCode & 0xFF00U) != 0U ? 1U << 24U : 0U));
    wchar_t name[128]{};
    if (GetKeyNameTextW(keyFlags, name, static_cast<int>(std::size(name))) > 0) {
        return name;
    }
    return L"Key " + std::to_wstring(key);
}

}  // namespace gamma_changer
