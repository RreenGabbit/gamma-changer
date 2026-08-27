#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "gamma_math.h"

namespace {

using gamma_changer::ApplyRelativeGamma;
using gamma_changer::GammaRamp;
using gamma_changer::RampsApproximatelyEqual;

constexpr wchar_t kWindowClassName[] = L"GammaChanger.MainWindow";
constexpr wchar_t kPatternClassName[] = L"GammaChanger.CalibrationPattern";
constexpr wchar_t kWindowTitle[] = L"Gamma Changer";
constexpr wchar_t kAppRegistryPath[] = L"Software\\GammaChanger";
constexpr wchar_t kRunRegistryPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"GammaChanger";
constexpr wchar_t kSingleInstanceName[] = L"Local\\GammaChanger.SingleInstance.v1";

constexpr UINT kTrayIconId = 1U;
constexpr UINT kTrayMessage = WM_APP + 1U;
constexpr UINT kShowWindowMessage = WM_APP + 2U;
constexpr UINT kSingleKeyMessage = WM_APP + 3U;
constexpr UINT_PTR kApplyTimerId = 1U;
constexpr UINT_PTR kDisplayRefreshTimerId = 2U;

constexpr int kControlHeader = 1001;
constexpr int kControlSubtitle = 1002;
constexpr int kControlDisplayLabel = 1003;
constexpr int kControlDisplayCombo = 1004;
constexpr int kControlGammaLabel = 1005;
constexpr int kControlGammaValue = 1006;
constexpr int kControlGammaSlider = 1007;
constexpr int kControlMinimumLabel = 1008;
constexpr int kControlMaximumLabel = 1009;
constexpr int kControlPattern = 1010;
constexpr int kControlHint = 1011;
constexpr int kControlStartup = 1012;
constexpr int kControlReset = 1013;
constexpr int kControlStatus = 1014;
constexpr int kControlHotkeyLabel = 1015;
constexpr int kControlHotkey = 1016;
constexpr int kControlSaveHotkey = 1017;
constexpr int kControlClearHotkey = 1018;

constexpr int kMenuOpen = 2001;
constexpr int kMenuResetAllAndExit = 2002;
constexpr int kMenuExit = 2003;

struct NativeGammaRamp {
    WORD red[gamma_changer::kGammaRampEntries];
    WORD green[gamma_changer::kGammaRampEntries];
    WORD blue[gamma_changer::kGammaRampEntries];
};

static_assert(sizeof(NativeGammaRamp) == 3U * 256U * sizeof(WORD));

struct ScopedRegistryKey {
    HKEY value = nullptr;

    ScopedRegistryKey() = default;
    explicit ScopedRegistryKey(HKEY key) : value(key) {}
    ScopedRegistryKey(const ScopedRegistryKey&) = delete;
    ScopedRegistryKey& operator=(const ScopedRegistryKey&) = delete;

    ScopedRegistryKey(ScopedRegistryKey&& other) noexcept : value(other.value) {
        other.value = nullptr;
    }

    ScopedRegistryKey& operator=(ScopedRegistryKey&& other) noexcept {
        if (this != &other) {
            if (value != nullptr) {
                RegCloseKey(value);
            }
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }

    ~ScopedRegistryKey() {
        if (value != nullptr) {
            RegCloseKey(value);
        }
    }

    explicit operator bool() const noexcept { return value != nullptr; }
};

struct PersistentDisplaySettings {
    double gamma = 1.0;
    bool enabled = false;
    GammaRamp baseline{};
    GammaRamp lastApplied{};
    bool hasBaseline = false;
    bool hasLastApplied = false;
};

struct DisplayState {
    std::wstring deviceName;
    std::wstring label;
    bool primary = false;
    int width = 0;
    int height = 0;
    bool rampReadable = false;
    double gamma = 1.0;
    bool enabled = false;
    GammaRamp baseline{};
    GammaRamp lastApplied{};
    bool hasBaseline = false;
    bool hasLastApplied = false;
    std::wstring lastError;
};

HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
HWND g_header = nullptr;
HWND g_subtitle = nullptr;
HWND g_displayLabel = nullptr;
HWND g_displayCombo = nullptr;
HWND g_gammaLabel = nullptr;
HWND g_gammaValue = nullptr;
HWND g_gammaSlider = nullptr;
HWND g_minimumLabel = nullptr;
HWND g_maximumLabel = nullptr;
HWND g_pattern = nullptr;
HWND g_hint = nullptr;
HWND g_startup = nullptr;
HWND g_reset = nullptr;
HWND g_status = nullptr;
HWND g_hotkeyLabel = nullptr;
HWND g_hotkeyControl = nullptr;
HWND g_saveHotkey = nullptr;
HWND g_clearHotkey = nullptr;
HFONT g_headerFont = nullptr;
HFONT g_valueFont = nullptr;
HFONT g_bodyFont = nullptr;
HFONT g_smallFont = nullptr;
HBRUSH g_backgroundBrush = nullptr;
std::vector<DisplayState> g_displays;
int g_selectedDisplay = -1;
bool g_updatingControls = false;
bool g_statusIsError = false;
bool g_trayIconAdded = false;
UINT g_taskbarCreatedMessage = 0U;
UINT g_hotkeyVirtualKey = 0U;
HHOOK g_keyboardHook = nullptr;
bool g_boundKeyIsDown = false;

std::wstring FormatSystemError(const DWORD error) {
    if (error == ERROR_SUCCESS) {
        return L"Unknown error";
    }

    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    if (length == 0U || message == nullptr) {
        return L"Windows error " + std::to_wstring(error);
    }

    std::wstring result(message, length);
    LocalFree(message);
    while (!result.empty() &&
           (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) {
        result.pop_back();
    }
    return result;
}

std::wstring FormatGamma(const double gamma) {
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(2) << gamma;
    return stream.str();
}

NativeGammaRamp ToNativeRamp(const GammaRamp& ramp) {
    NativeGammaRamp native{};
    for (std::size_t index = 0; index < gamma_changer::kGammaRampEntries; ++index) {
        native.red[index] = ramp.channels[0][index];
        native.green[index] = ramp.channels[1][index];
        native.blue[index] = ramp.channels[2][index];
    }
    return native;
}

GammaRamp FromNativeRamp(const NativeGammaRamp& native) {
    GammaRamp ramp{};
    for (std::size_t index = 0; index < gamma_changer::kGammaRampEntries; ++index) {
        ramp.channels[0][index] = native.red[index];
        ramp.channels[1][index] = native.green[index];
        ramp.channels[2][index] = native.blue[index];
    }
    return ramp;
}

bool ReadDeviceRamp(
    const std::wstring& deviceName,
    GammaRamp& ramp,
    std::wstring& errorMessage) {
    HDC deviceContext = CreateDCW(L"DISPLAY", deviceName.c_str(), nullptr, nullptr);
    if (deviceContext == nullptr) {
        errorMessage = L"Could not open " + deviceName + L": " +
                       FormatSystemError(GetLastError());
        return false;
    }

    NativeGammaRamp native{};
    SetLastError(ERROR_SUCCESS);
    const BOOL succeeded = GetDeviceGammaRamp(deviceContext, &native);
    const DWORD error = GetLastError();
    DeleteDC(deviceContext);
    if (succeeded == FALSE) {
        errorMessage = L"The display driver does not expose a readable gamma ramp";
        if (error != ERROR_SUCCESS) {
            errorMessage += L": " + FormatSystemError(error);
        }
        return false;
    }

    ramp = FromNativeRamp(native);
    return true;
}

bool WriteDeviceRamp(
    const std::wstring& deviceName,
    const GammaRamp& requested,
    GammaRamp& actual,
    std::wstring& errorMessage) {
    HDC deviceContext = CreateDCW(L"DISPLAY", deviceName.c_str(), nullptr, nullptr);
    if (deviceContext == nullptr) {
        errorMessage = L"Could not open " + deviceName + L": " +
                       FormatSystemError(GetLastError());
        return false;
    }

    NativeGammaRamp native = ToNativeRamp(requested);
    SetLastError(ERROR_SUCCESS);
    const BOOL succeeded = SetDeviceGammaRamp(deviceContext, &native);
    const DWORD setError = GetLastError();
    DeleteDC(deviceContext);
    if (succeeded == FALSE) {
        errorMessage = L"The display driver rejected the gamma ramp";
        if (setError != ERROR_SUCCESS) {
            errorMessage += L": " + FormatSystemError(setError);
        }
        return false;
    }

    std::wstring readError;
    if (!ReadDeviceRamp(deviceName, actual, readError)) {
        errorMessage = L"Windows accepted the gamma ramp, but it could not be verified. " +
                       readError;
        return false;
    }

    // GDI can report success while its safety heuristics silently reject a ramp.
    // A 1024-unit allowance covers ordinary 8/10-bit LUT quantization on readback.
    if (!RampsApproximatelyEqual(requested, actual, 1024U)) {
        errorMessage =
            L"Windows reported success, but the display driver did not apply the requested "
            L"ramp. Turn HDR off and check the graphics driver.";
        return false;
    }

    return true;
}

std::uint64_t HashDeviceName(const std::wstring& deviceName) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const wchar_t character : deviceName) {
        const auto folded = static_cast<std::uint32_t>(std::towlower(character));
        hash ^= static_cast<std::uint8_t>(folded & 0xFFU);
        hash *= 1099511628211ULL;
        hash ^= static_cast<std::uint8_t>((folded >> 8U) & 0xFFU);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::wstring DisplayRegistryPath(const std::wstring& deviceName) {
    std::wostringstream stream;
    stream << kAppRegistryPath << L"\\Displays\\" << std::hex << std::setw(16)
           << std::setfill(L'0') << HashDeviceName(deviceName);
    return stream.str();
}

ScopedRegistryKey OpenRegistryKey(
    const std::wstring& path,
    const REGSAM access,
    const bool create) {
    HKEY key = nullptr;
    const LSTATUS status = create
                               ? RegCreateKeyExW(
                                     HKEY_CURRENT_USER,
                                     path.c_str(),
                                     0,
                                     nullptr,
                                     REG_OPTION_NON_VOLATILE,
                                     access,
                                     nullptr,
                                     &key,
                                     nullptr)
                               : RegOpenKeyExW(
                                     HKEY_CURRENT_USER,
                                     path.c_str(),
                                     0,
                                     access,
                                     &key);
    return status == ERROR_SUCCESS ? ScopedRegistryKey(key) : ScopedRegistryKey();
}

bool ReadDword(HKEY key, const wchar_t* name, DWORD& value) {
    DWORD type = 0;
    DWORD size = sizeof(value);
    return RegQueryValueExW(
               key,
               name,
               nullptr,
               &type,
               reinterpret_cast<BYTE*>(&value),
               &size) == ERROR_SUCCESS &&
           type == REG_DWORD && size == sizeof(value);
}

void WriteDword(HKEY key, const wchar_t* name, const DWORD value) {
    RegSetValueExW(
        key,
        name,
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&value),
        sizeof(value));
}

bool ReadRamp(HKEY key, const wchar_t* name, GammaRamp& ramp) {
    NativeGammaRamp native{};
    DWORD type = 0;
    DWORD size = sizeof(native);
    if (RegQueryValueExW(
            key,
            name,
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(&native),
            &size) != ERROR_SUCCESS ||
        type != REG_BINARY || size != sizeof(native)) {
        return false;
    }
    ramp = FromNativeRamp(native);
    return true;
}

void WriteRamp(HKEY key, const wchar_t* name, const GammaRamp& ramp) {
    const NativeGammaRamp native = ToNativeRamp(ramp);
    RegSetValueExW(
        key,
        name,
        0,
        REG_BINARY,
        reinterpret_cast<const BYTE*>(&native),
        sizeof(native));
}

PersistentDisplaySettings LoadDisplaySettings(const std::wstring& deviceName) {
    PersistentDisplaySettings settings{};
    auto key = OpenRegistryKey(DisplayRegistryPath(deviceName), KEY_READ, false);
    if (!key) {
        return settings;
    }

    DWORD gammaMilli = 1000U;
    DWORD enabled = 0U;
    if (ReadDword(key.value, L"GammaMilli", gammaMilli)) {
        settings.gamma = gamma_changer::ClampGamma(
            static_cast<double>(gammaMilli) / 1000.0);
    }
    if (ReadDword(key.value, L"Enabled", enabled)) {
        settings.enabled = enabled != 0U;
    }
    settings.hasBaseline = ReadRamp(key.value, L"BaselineRamp", settings.baseline);
    settings.hasLastApplied = ReadRamp(key.value, L"LastAppliedRamp", settings.lastApplied);
    return settings;
}

void SaveDisplaySettings(const DisplayState& display) {
    auto key = OpenRegistryKey(
        DisplayRegistryPath(display.deviceName),
        KEY_SET_VALUE,
        true);
    if (!key) {
        return;
    }

    const DWORD gammaMilli = static_cast<DWORD>(std::lround(display.gamma * 1000.0));
    WriteDword(key.value, L"GammaMilli", gammaMilli);
    WriteDword(key.value, L"Enabled", display.enabled ? 1U : 0U);
    const DWORD nameBytes = static_cast<DWORD>(
        (display.deviceName.size() + 1U) * sizeof(wchar_t));
    RegSetValueExW(
        key.value,
        L"DeviceName",
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(display.deviceName.c_str()),
        nameBytes);
    if (display.hasBaseline) {
        WriteRamp(key.value, L"BaselineRamp", display.baseline);
    }
    if (display.hasLastApplied) {
        WriteRamp(key.value, L"LastAppliedRamp", display.lastApplied);
    }
}

std::wstring ReadSelectedDevice() {
    auto key = OpenRegistryKey(kAppRegistryPath, KEY_READ, false);
    if (!key) {
        return {};
    }

    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(
            key.value,
            L"SelectedDisplay",
            nullptr,
            &type,
            nullptr,
            &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t)) {
        return {};
    }

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1U, L'\0');
    if (RegQueryValueExW(
            key.value,
            L"SelectedDisplay",
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(buffer.data()),
            &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return std::wstring(buffer.data());
}

void SaveSelectedDevice(const std::wstring& deviceName) {
    auto key = OpenRegistryKey(kAppRegistryPath, KEY_SET_VALUE, true);
    if (!key) {
        return;
    }
    const DWORD bytes = static_cast<DWORD>((deviceName.size() + 1U) * sizeof(wchar_t));
    RegSetValueExW(
        key.value,
        L"SelectedDisplay",
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(deviceName.c_str()),
        bytes);
}

void LoadHotkeySettings() {
    auto key = OpenRegistryKey(kAppRegistryPath, KEY_READ, false);
    if (!key) {
        return;
    }

    DWORD virtualKey = 0U;
    if (ReadDword(key.value, L"HotkeyVirtualKey", virtualKey) && virtualKey <= 0xFFU) {
        g_hotkeyVirtualKey = virtualKey;
    }
}

bool SaveHotkeySettings(std::wstring& errorMessage) {
    auto key = OpenRegistryKey(kAppRegistryPath, KEY_SET_VALUE, true);
    if (!key) {
        errorMessage = L"Could not save the hotkey for the current user.";
        return false;
    }
    WriteDword(key.value, L"HotkeyVirtualKey", g_hotkeyVirtualKey);
    // Clear modifier data written by versions that used combination hotkeys.
    WriteDword(key.value, L"HotkeyFlags", 0U);
    return true;
}

WORD PackedHotkey(const UINT virtualKey) {
    return MAKEWORD(
        static_cast<BYTE>(virtualKey & 0xFFU),
        0U);
}

std::wstring GetExecutablePath() {
    std::vector<wchar_t> buffer(512U);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0U) {
            return {};
        }
        if (length < buffer.size()) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2U);
    }
}

bool IsStartupEnabled() {
    auto key = OpenRegistryKey(kRunRegistryPath, KEY_QUERY_VALUE, false);
    if (!key) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    return RegQueryValueExW(
               key.value,
               kRunValueName,
               nullptr,
               &type,
               nullptr,
               &bytes) == ERROR_SUCCESS &&
           type == REG_SZ && bytes > sizeof(wchar_t);
}

bool SetStartupEnabled(const bool enabled, std::wstring& errorMessage) {
    auto key = OpenRegistryKey(kRunRegistryPath, KEY_SET_VALUE, true);
    if (!key) {
        errorMessage = L"Could not open the current user's Startup registry key.";
        return false;
    }

    LSTATUS status = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring executable = GetExecutablePath();
        if (executable.empty()) {
            errorMessage = L"Could not find the application executable.";
            return false;
        }
        const std::wstring command = L"\"" + executable + L"\" --background";
        const DWORD bytes = static_cast<DWORD>((command.size() + 1U) * sizeof(wchar_t));
        status = RegSetValueExW(
            key.value,
            kRunValueName,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            bytes);
    } else {
        status = RegDeleteValueW(key.value, kRunValueName);
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    }

    if (status != ERROR_SUCCESS) {
        errorMessage = L"Could not update Startup: " + FormatSystemError(status);
        return false;
    }
    return true;
}

std::vector<DisplayState> EnumerateDisplays() {
    std::vector<DisplayState> displays;
    for (DWORD adapterIndex = 0;; ++adapterIndex) {
        DISPLAY_DEVICEW adapter{};
        adapter.cb = sizeof(adapter);
        if (EnumDisplayDevicesW(nullptr, adapterIndex, &adapter, 0) == FALSE) {
            break;
        }
        if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0U ||
            (adapter.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0U) {
            continue;
        }

        std::wstring friendlyName;
        for (DWORD monitorIndex = 0;; ++monitorIndex) {
            DISPLAY_DEVICEW monitor{};
            monitor.cb = sizeof(monitor);
            if (EnumDisplayDevicesW(adapter.DeviceName, monitorIndex, &monitor, 0) == FALSE) {
                break;
            }
            if (friendlyName.empty() || (monitor.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0U) {
                friendlyName = monitor.DeviceString;
            }
            if ((monitor.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0U) {
                break;
            }
        }
        if (friendlyName.empty()) {
            friendlyName = adapter.DeviceString;
        }
        if (friendlyName.empty()) {
            friendlyName = L"Display";
        }

        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        const bool hasMode = EnumDisplaySettingsExW(
                                 adapter.DeviceName,
                                 ENUM_CURRENT_SETTINGS,
                                 &mode,
                                 0) != FALSE;

        DisplayState display{};
        display.deviceName = adapter.DeviceName;
        display.primary = (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0U;
        if (hasMode) {
            display.width = static_cast<int>(mode.dmPelsWidth);
            display.height = static_cast<int>(mode.dmPelsHeight);
        }

        std::wostringstream label;
        label << friendlyName;
        if (display.width > 0 && display.height > 0) {
            label << L"  ·  " << display.width << L" × " << display.height;
        }
        if (display.primary) {
            label << L"  ·  Primary";
        }
        label << L"  (" << display.deviceName << L")";
        display.label = label.str();
        displays.push_back(std::move(display));
    }

    std::stable_sort(
        displays.begin(),
        displays.end(),
        [](const DisplayState& left, const DisplayState& right) {
            return left.primary && !right.primary;
        });
    return displays;
}

void SetStatus(const std::wstring& message, const bool error) {
    g_statusIsError = error;
    if (g_status != nullptr) {
        SetWindowTextW(g_status, message.c_str());
        InvalidateRect(g_status, nullptr, TRUE);
    }
}

bool SingleKeyIsAllowed(const UINT virtualKey, std::wstring& errorMessage) {
    if (virtualKey == 0U) {
        errorMessage = L"Press one key in the key box first.";
        return false;
    }

    switch (virtualKey) {
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
            errorMessage = L"Choose a non-modifier key, such as Caps Lock or an F-key.";
            return false;
        default:
            return true;
    }
}

LRESULT CALLBACK SingleKeyKeyboardProc(
    const int code,
    const WPARAM message,
    const LPARAM data) {
    if (code == HC_ACTION && g_hotkeyVirtualKey != 0U) {
        const auto* keyboard = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        if (keyboard->vkCode == g_hotkeyVirtualKey) {
            const bool isKeyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            const bool isKeyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
            if (isKeyDown) {
                if (!g_boundKeyIsDown) {
                    g_boundKeyIsDown = true;
                    PostMessageW(g_mainWindow, kSingleKeyMessage, 0, 0);
                }
                return 1;
            }
            if (isKeyUp) {
                g_boundKeyIsDown = false;
                return 1;
            }
        }
    }
    return CallNextHookEx(g_keyboardHook, code, message, data);
}

void RemoveSingleKeyHook() {
    if (g_keyboardHook != nullptr) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    g_boundKeyIsDown = false;
}

bool InstallSingleKeyHook(std::wstring& errorMessage) {
    if (g_hotkeyVirtualKey == 0U) {
        return true;
    }
    if (!SingleKeyIsAllowed(g_hotkeyVirtualKey, errorMessage)) {
        return false;
    }

    SetLastError(ERROR_SUCCESS);
    g_keyboardHook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        SingleKeyKeyboardProc,
        g_instance,
        0U);
    if (g_keyboardHook == nullptr) {
        const DWORD error = GetLastError();
        errorMessage = L"Windows could not activate the single-key shortcut: " +
                       FormatSystemError(error);
        return false;
    }
    return true;
}

void SetHotkeyControlValue(const UINT virtualKey) {
    if (g_hotkeyControl != nullptr) {
        SendMessageW(
            g_hotkeyControl,
            HKM_SETHOTKEY,
            PackedHotkey(virtualKey),
            0);
    }
}

bool InstallLoadedSingleKey(std::wstring& errorMessage) {
    SetHotkeyControlValue(g_hotkeyVirtualKey);
    if (g_hotkeyVirtualKey == 0U) {
        return true;
    }
    return InstallSingleKeyHook(errorMessage);
}

void SaveHotkeyFromControl() {
    const WORD packed = static_cast<WORD>(
        SendMessageW(g_hotkeyControl, HKM_GETHOTKEY, 0, 0));
    const UINT requestedVirtualKey = LOBYTE(packed);
    std::wstring error;
    if (!SingleKeyIsAllowed(requestedVirtualKey, error)) {
        SetStatus(error, true);
        return;
    }

    const UINT previousVirtualKey = g_hotkeyVirtualKey;
    RemoveSingleKeyHook();
    g_hotkeyVirtualKey = requestedVirtualKey;
    if (!InstallSingleKeyHook(error)) {
        g_hotkeyVirtualKey = previousVirtualKey;
        std::wstring ignored;
        InstallSingleKeyHook(ignored);
        SetHotkeyControlValue(previousVirtualKey);
        SetStatus(error, true);
        return;
    }

    if (!SaveHotkeySettings(error)) {
        RemoveSingleKeyHook();
        g_hotkeyVirtualKey = previousVirtualKey;
        std::wstring ignored;
        InstallSingleKeyHook(ignored);
        SetHotkeyControlValue(previousVirtualKey);
        SetStatus(error, true);
        return;
    }

    SetHotkeyControlValue(requestedVirtualKey);
    SetStatus(
        L"Key saved · It works even while Shift, Ctrl, or Alt is held",
        false);
}

void ClearSavedHotkey() {
    const UINT previousVirtualKey = g_hotkeyVirtualKey;
    g_hotkeyVirtualKey = 0U;
    std::wstring error;
    if (!SaveHotkeySettings(error)) {
        g_hotkeyVirtualKey = previousVirtualKey;
        SetHotkeyControlValue(previousVirtualKey);
        SetStatus(error, true);
        return;
    }

    RemoveSingleKeyHook();
    SetHotkeyControlValue(0U);
    SetStatus(L"Global gamma key cleared", false);
}

bool ApplyGammaToDisplay(
    DisplayState& display,
    const double requestedGamma,
    const bool enabled,
    std::wstring& errorMessage) {
    if (!display.hasBaseline) {
        errorMessage = L"No readable baseline gamma ramp is available for this display.";
        display.lastError = errorMessage;
        return false;
    }

    const double gamma = gamma_changer::ClampGamma(requestedGamma);
    const GammaRamp target = enabled ? ApplyRelativeGamma(display.baseline, gamma)
                                     : display.baseline;
    GammaRamp actual{};
    if (!WriteDeviceRamp(display.deviceName, target, actual, errorMessage)) {
        display.lastError = errorMessage;
        if (enabled) {
            GammaRamp restored{};
            std::wstring restoreError;
            if (WriteDeviceRamp(
                    display.deviceName,
                    display.baseline,
                    restored,
                    restoreError)) {
                display.gamma = 1.0;
                display.enabled = false;
                display.lastApplied = restored;
                display.hasLastApplied = true;
                display.rampReadable = true;
                SaveDisplaySettings(display);
                errorMessage += L" The captured Windows calibration was restored.";
                display.lastError = errorMessage;
            } else {
                errorMessage += L" Automatic restoration also failed: " + restoreError;
                display.lastError = errorMessage;
            }
        }
        return false;
    }

    display.gamma = enabled ? gamma : 1.0;
    display.enabled = enabled;
    display.lastApplied = actual;
    display.hasLastApplied = true;
    display.rampReadable = true;
    display.lastError.clear();
    SaveDisplaySettings(display);
    return true;
}

void RefreshDisplays(const bool reapplySavedSettings) {
    const std::wstring selectedName =
        (g_selectedDisplay >= 0 && g_selectedDisplay < static_cast<int>(g_displays.size()))
            ? g_displays[static_cast<std::size_t>(g_selectedDisplay)].deviceName
            : ReadSelectedDevice();
    std::vector<DisplayState> refreshed = EnumerateDisplays();

    for (auto& display : refreshed) {
        const auto previous = std::find_if(
            g_displays.begin(),
            g_displays.end(),
            [&](const DisplayState& candidate) {
                return candidate.deviceName == display.deviceName;
            });

        std::wstring readError;
        GammaRamp current{};
        display.rampReadable = ReadDeviceRamp(display.deviceName, current, readError);

        if (previous != g_displays.end()) {
            display.gamma = previous->gamma;
            display.enabled = previous->enabled;
            display.baseline = previous->baseline;
            display.lastApplied = previous->lastApplied;
            display.hasBaseline = previous->hasBaseline;
            display.hasLastApplied = previous->hasLastApplied;
            display.lastError = previous->lastError;
        } else {
            const PersistentDisplaySettings saved = LoadDisplaySettings(display.deviceName);
            display.gamma = saved.gamma;
            display.enabled = saved.enabled;
            display.baseline = saved.baseline;
            display.lastApplied = saved.lastApplied;
            display.hasBaseline = saved.hasBaseline;
            display.hasLastApplied = saved.hasLastApplied;

            if (display.rampReadable) {
                // If the current ramp is ours (for example after a crash/relaunch), retain
                // the saved Windows baseline. Otherwise treat Windows' current state as a
                // new baseline, which also respects later ICC/profile changes.
                const bool currentIsOurs = display.hasBaseline && display.hasLastApplied &&
                                           RampsApproximatelyEqual(
                                               current,
                                               display.lastApplied,
                                               1024U);
                if (!currentIsOurs) {
                    display.baseline = current;
                    display.hasBaseline = true;
                    SaveDisplaySettings(display);
                }
            }
        }

        if (!display.hasBaseline && display.rampReadable) {
            display.baseline = current;
            display.hasBaseline = true;
            SaveDisplaySettings(display);
        }
        if (!display.rampReadable) {
            display.lastError = readError;
        } else if (!display.enabled) {
            display.lastError.clear();
        }
    }

    g_displays = std::move(refreshed);

    if (reapplySavedSettings) {
        for (auto& display : g_displays) {
            if (!display.enabled) {
                continue;
            }
            std::wstring error;
            ApplyGammaToDisplay(display, display.gamma, true, error);
        }
    }

    g_selectedDisplay = -1;
    for (std::size_t index = 0; index < g_displays.size(); ++index) {
        if (g_displays[index].deviceName == selectedName) {
            g_selectedDisplay = static_cast<int>(index);
            break;
        }
    }
    if (g_selectedDisplay < 0 && !g_displays.empty()) {
        g_selectedDisplay = 0;
    }

}

int ScaleForDpi(const HWND window, const int value) {
    const UINT dpi = window != nullptr ? GetDpiForWindow(window) : GetDpiForSystem();
    return MulDiv(value, static_cast<int>(dpi), 96);
}

HFONT CreateUiFont(const HWND window, const int pointSize, const int weight) {
    const UINT dpi = window != nullptr ? GetDpiForWindow(window) : 96U;
    return CreateFontW(
        -MulDiv(pointSize, static_cast<int>(dpi), 72),
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

void AssignFont(const HWND control, const HFONT font) {
    if (control != nullptr && font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void RecreateFonts() {
    HFONT newHeader = CreateUiFont(g_mainWindow, 23, FW_SEMIBOLD);
    HFONT newValue = CreateUiFont(g_mainWindow, 21, FW_SEMIBOLD);
    HFONT newBody = CreateUiFont(g_mainWindow, 10, FW_NORMAL);
    HFONT newSmall = CreateUiFont(g_mainWindow, 9, FW_NORMAL);

    AssignFont(g_header, newHeader);
    AssignFont(g_gammaValue, newValue);
    const std::array<HWND, 12> bodyControls = {
        g_subtitle,
        g_displayLabel,
        g_displayCombo,
        g_gammaLabel,
        g_hotkeyLabel,
        g_hotkeyControl,
        g_saveHotkey,
        g_clearHotkey,
        g_startup,
        g_reset,
        g_status,
        g_hint,
    };
    for (const HWND control : bodyControls) {
        AssignFont(control, newBody);
    }
    AssignFont(g_minimumLabel, newSmall);
    AssignFont(g_maximumLabel, newSmall);

    if (g_headerFont != nullptr) {
        DeleteObject(g_headerFont);
    }
    if (g_valueFont != nullptr) {
        DeleteObject(g_valueFont);
    }
    if (g_bodyFont != nullptr) {
        DeleteObject(g_bodyFont);
    }
    if (g_smallFont != nullptr) {
        DeleteObject(g_smallFont);
    }
    g_headerFont = newHeader;
    g_valueFont = newValue;
    g_bodyFont = newBody;
    g_smallFont = newSmall;
    InvalidateRect(g_pattern, nullptr, TRUE);
}

void LayoutControls() {
    if (g_mainWindow == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(g_mainWindow, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = ScaleForDpi(g_mainWindow, 32);
    const int contentWidth = std::max(1, width - margin * 2);
    const int patternY = ScaleForDpi(g_mainWindow, 306);
    const int bottomRowY = height - ScaleForDpi(g_mainWindow, 69);
    const int hotkeyRowY = bottomRowY - ScaleForDpi(g_mainWindow, 42);
    const int statusY = height - ScaleForDpi(g_mainWindow, 30);
    const int patternHeight = std::max(
        ScaleForDpi(g_mainWindow, 122),
        hotkeyRowY - ScaleForDpi(g_mainWindow, 58) - patternY);
    const int clearWidth = ScaleForDpi(g_mainWindow, 66);
    const int saveWidth = ScaleForDpi(g_mainWindow, 82);
    const int hotkeyWidth = ScaleForDpi(g_mainWindow, 150);
    const int smallGap = ScaleForDpi(g_mainWindow, 8);
    const int clearX = margin + contentWidth - clearWidth;
    const int saveX = clearX - smallGap - saveWidth;
    const int hotkeyX = saveX - smallGap - hotkeyWidth;

    MoveWindow(g_header, margin, ScaleForDpi(g_mainWindow, 20), contentWidth,
               ScaleForDpi(g_mainWindow, 39), TRUE);
    MoveWindow(g_subtitle, margin, ScaleForDpi(g_mainWindow, 61), contentWidth,
               ScaleForDpi(g_mainWindow, 39), TRUE);
    MoveWindow(g_displayLabel, margin, ScaleForDpi(g_mainWindow, 124), contentWidth,
               ScaleForDpi(g_mainWindow, 22), TRUE);
    MoveWindow(g_displayCombo, margin, ScaleForDpi(g_mainWindow, 150), contentWidth,
               ScaleForDpi(g_mainWindow, 220), TRUE);
    MoveWindow(g_gammaLabel, margin, ScaleForDpi(g_mainWindow, 204), contentWidth / 2,
               ScaleForDpi(g_mainWindow, 24), TRUE);
    MoveWindow(g_gammaValue, margin + contentWidth / 2, ScaleForDpi(g_mainWindow, 194),
               contentWidth / 2, ScaleForDpi(g_mainWindow, 42), TRUE);
    MoveWindow(g_gammaSlider, margin, ScaleForDpi(g_mainWindow, 230), contentWidth,
               ScaleForDpi(g_mainWindow, 45), TRUE);
    MoveWindow(g_minimumLabel, margin, ScaleForDpi(g_mainWindow, 274), contentWidth / 2,
               ScaleForDpi(g_mainWindow, 20), TRUE);
    MoveWindow(g_maximumLabel, margin + contentWidth / 2, ScaleForDpi(g_mainWindow, 274),
               contentWidth / 2, ScaleForDpi(g_mainWindow, 20), TRUE);
    MoveWindow(g_pattern, margin, patternY, contentWidth, patternHeight, TRUE);
    MoveWindow(g_hint, margin, patternY + patternHeight + ScaleForDpi(g_mainWindow, 8),
               contentWidth, ScaleForDpi(g_mainWindow, 42), TRUE);
    MoveWindow(g_hotkeyLabel, margin, hotkeyRowY,
               std::max(1, hotkeyX - margin - smallGap), ScaleForDpi(g_mainWindow, 32), TRUE);
    MoveWindow(g_hotkeyControl, hotkeyX, hotkeyRowY, hotkeyWidth,
               ScaleForDpi(g_mainWindow, 30), TRUE);
    MoveWindow(g_saveHotkey, saveX, hotkeyRowY, saveWidth,
               ScaleForDpi(g_mainWindow, 30), TRUE);
    MoveWindow(g_clearHotkey, clearX, hotkeyRowY, clearWidth,
               ScaleForDpi(g_mainWindow, 30), TRUE);
    MoveWindow(g_startup, margin, bottomRowY, contentWidth - ScaleForDpi(g_mainWindow, 176),
               ScaleForDpi(g_mainWindow, 34), TRUE);
    MoveWindow(g_reset, margin + contentWidth - ScaleForDpi(g_mainWindow, 164), bottomRowY,
               ScaleForDpi(g_mainWindow, 164), ScaleForDpi(g_mainWindow, 34), TRUE);
    MoveWindow(g_status, margin, statusY, contentWidth, ScaleForDpi(g_mainWindow, 22), TRUE);
}

void PopulateDisplayCombo() {
    g_updatingControls = true;
    SendMessageW(g_displayCombo, CB_RESETCONTENT, 0, 0);
    for (std::size_t index = 0; index < g_displays.size(); ++index) {
        const LRESULT item = SendMessageW(
            g_displayCombo,
            CB_ADDSTRING,
            0,
            reinterpret_cast<LPARAM>(g_displays[index].label.c_str()));
        if (item != CB_ERR && item != CB_ERRSPACE) {
            SendMessageW(
                g_displayCombo,
                CB_SETITEMDATA,
                static_cast<WPARAM>(item),
                static_cast<LPARAM>(index));
        }
    }
    if (g_selectedDisplay >= 0) {
        SendMessageW(g_displayCombo, CB_SETCURSEL, g_selectedDisplay, 0);
    }
    g_updatingControls = false;
}

void UpdateSelectedDisplayUi() {
    g_updatingControls = true;
    const bool hasDisplay =
        g_selectedDisplay >= 0 && g_selectedDisplay < static_cast<int>(g_displays.size());
    EnableWindow(g_displayCombo, hasDisplay ? TRUE : FALSE);
    EnableWindow(g_gammaSlider, hasDisplay ? TRUE : FALSE);
    EnableWindow(g_reset, hasDisplay ? TRUE : FALSE);

    if (!hasDisplay) {
        SetWindowTextW(g_gammaValue, L"—");
        SetStatus(L"No active desktop display was found.", true);
        g_updatingControls = false;
        return;
    }

    const DisplayState& display = g_displays[static_cast<std::size_t>(g_selectedDisplay)];
    const double gamma = display.enabled ? display.gamma : 1.0;
    SendMessageW(
        g_gammaSlider,
        TBM_SETPOS,
        TRUE,
        static_cast<LPARAM>(std::lround(gamma * 100.0)));
    SetWindowTextW(g_gammaValue, FormatGamma(gamma).c_str());

    if (!display.lastError.empty()) {
        SetStatus(display.lastError, true);
        if (!display.rampReadable || !display.hasBaseline) {
            EnableWindow(g_gammaSlider, FALSE);
            EnableWindow(g_reset, FALSE);
        }
    } else if (!display.rampReadable || !display.hasBaseline) {
        SetStatus(
            L"This display driver does not provide a usable SDR hardware gamma ramp.",
            true);
        EnableWindow(g_gammaSlider, FALSE);
        EnableWindow(g_reset, FALSE);
    } else if (display.enabled) {
        SetStatus(
            L"Active · Windows hardware gamma ramp applied at " +
                FormatGamma(display.gamma),
            false);
    } else {
        SetStatus(L"System calibration is active · Gamma correction 1.00", false);
    }
    g_updatingControls = false;
}

double SliderGamma() {
    const LRESULT position = SendMessageW(g_gammaSlider, TBM_GETPOS, 0, 0);
    return gamma_changer::ClampGamma(static_cast<double>(position) / 100.0);
}

void ApplySliderValue() {
    if (g_selectedDisplay < 0 ||
        g_selectedDisplay >= static_cast<int>(g_displays.size())) {
        return;
    }
    DisplayState& display = g_displays[static_cast<std::size_t>(g_selectedDisplay)];
    const double gamma = SliderGamma();
    const bool enabled = std::abs(gamma - 1.0) >= 0.0001;
    std::wstring error;
    if (!ApplyGammaToDisplay(display, gamma, enabled, error)) {
        SetStatus(error, true);
        return;
    }
    SetWindowTextW(g_gammaValue, FormatGamma(display.gamma).c_str());
    SaveSelectedDevice(display.deviceName);
    SetStatus(
        display.enabled
            ? L"Active · Windows hardware gamma ramp applied at " +
                  FormatGamma(display.gamma)
            : L"System calibration restored · Gamma correction 1.00",
        false);
}

void ResetSelectedDisplay() {
    if (g_selectedDisplay < 0 ||
        g_selectedDisplay >= static_cast<int>(g_displays.size())) {
        return;
    }
    KillTimer(g_mainWindow, kApplyTimerId);
    g_updatingControls = true;
    SendMessageW(g_gammaSlider, TBM_SETPOS, TRUE, 100);
    SetWindowTextW(g_gammaValue, L"1.00");
    g_updatingControls = false;
    ApplySliderValue();
}

void ToggleMaximumGamma() {
    KillTimer(g_mainWindow, kApplyTimerId);
    if (g_selectedDisplay < 0 ||
        g_selectedDisplay >= static_cast<int>(g_displays.size())) {
        SetStatus(L"No display is selected for the gamma hotkey.", true);
        return;
    }

    DisplayState& display = g_displays[static_cast<std::size_t>(g_selectedDisplay)];
    const bool restoreDefault =
        display.enabled &&
        std::abs(display.gamma - gamma_changer::kMaximumGamma) < 0.0001;
    const double gamma = restoreDefault ? 1.0 : gamma_changer::kMaximumGamma;
    std::wstring error;
    ApplyGammaToDisplay(display, gamma, !restoreDefault, error);
    UpdateSelectedDisplayUi();
}

void ResetAllDisplays() {
    std::wstring firstError;
    for (auto& display : g_displays) {
        if (!display.hasBaseline) {
            continue;
        }
        std::wstring error;
        if (!ApplyGammaToDisplay(display, 1.0, false, error) && firstError.empty()) {
            firstError = display.deviceName + L": " + error;
        }
    }
    if (!firstError.empty()) {
        SetStatus(firstError, true);
    }
}

void ShowMainWindow() {
    ShowWindow(g_mainWindow, SW_SHOWNORMAL);
    SetForegroundWindow(g_mainWindow);
}

void AddTrayIcon() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_mainWindow;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(data.szTip, L"Gamma Changer", ARRAYSIZE(data.szTip));
    g_trayIconAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
}

void RemoveTrayIcon() {
    if (!g_trayIconAdded) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_mainWindow;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    g_trayIconAdded = false;
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, kMenuOpen, L"Open Gamma Changer");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuResetAllAndExit, L"Reset all displays and exit");
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit (keep current gamma)");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_mainWindow);
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        cursor.x,
        cursor.y,
        0,
        g_mainWindow,
        nullptr);
    DestroyMenu(menu);
    if (command != 0U) {
        PostMessageW(g_mainWindow, WM_COMMAND, command, 0);
    }
}

HWND CreateControl(
    const wchar_t* className,
    const wchar_t* text,
    const DWORD style,
    const int identifier,
    const DWORD extendedStyle = 0U) {
    return CreateWindowExW(
        extendedStyle,
        className,
        text,
        WS_CHILD | WS_VISIBLE | style,
        0,
        0,
        1,
        1,
        g_mainWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
        g_instance,
        nullptr);
}

void CreateMainControls() {
    g_header = CreateControl(L"STATIC", L"Gamma Changer", SS_LEFT | SS_NOPREFIX, kControlHeader);
    g_subtitle = CreateControl(
        L"STATIC",
        L"Uses the GPU's 256-point RGB gamma ramp—the same SDR adjustment path as Windows Display Color Calibration.",
        SS_LEFT | SS_NOPREFIX,
        kControlSubtitle);
    g_displayLabel = CreateControl(
        L"STATIC", L"Display", SS_LEFT | SS_NOPREFIX, kControlDisplayLabel);
    g_displayCombo = CreateControl(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        kControlDisplayCombo,
        WS_EX_CLIENTEDGE);
    g_gammaLabel = CreateControl(
        L"STATIC", L"Gamma correction", SS_LEFT | SS_NOPREFIX, kControlGammaLabel);
    g_gammaValue = CreateControl(
        L"STATIC", L"1.00", SS_RIGHT | SS_NOPREFIX, kControlGammaValue);
    g_gammaSlider = CreateControl(
        TRACKBAR_CLASSW,
        L"",
        TBS_HORZ | TBS_AUTOTICKS | TBS_DOWNISLEFT | WS_TABSTOP,
        kControlGammaSlider);
    SendMessageW(g_gammaSlider, TBM_SETRANGE, TRUE, MAKELPARAM(50, 300));
    SendMessageW(g_gammaSlider, TBM_SETTICFREQ, 25, 0);
    SendMessageW(g_gammaSlider, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(g_gammaSlider, TBM_SETLINESIZE, 0, 1);
    SendMessageW(g_gammaSlider, TBM_SETPOS, TRUE, 100);
    g_minimumLabel = CreateControl(
        L"STATIC", L"0.50 · darker midtones", SS_LEFT | SS_NOPREFIX, kControlMinimumLabel);
    g_maximumLabel = CreateControl(
        L"STATIC", L"3.00 · brighter midtones", SS_RIGHT | SS_NOPREFIX, kControlMaximumLabel);
    g_pattern = CreateControl(kPatternClassName, L"", 0U, kControlPattern);
    g_hint = CreateControl(
        L"STATIC",
        L"Move the slider until the middle dots are only faintly visible. Changes are live and saved for the selected display.",
        SS_LEFT | SS_NOPREFIX,
        kControlHint);
    g_hotkeyLabel = CreateControl(
        L"STATIC",
        L"Single-key toggle: 3.00 ↔ 1.00",
        SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
        kControlHotkeyLabel);
    g_hotkeyControl = CreateControl(
        HOTKEY_CLASSW,
        L"",
        WS_TABSTOP,
        kControlHotkey,
        WS_EX_CLIENTEDGE);
    g_saveHotkey = CreateControl(
        L"BUTTON", L"Set key", BS_PUSHBUTTON | WS_TABSTOP, kControlSaveHotkey);
    g_clearHotkey = CreateControl(
        L"BUTTON", L"Clear", BS_PUSHBUTTON | WS_TABSTOP, kControlClearHotkey);
    g_startup = CreateControl(
        L"BUTTON",
        L"Start with Windows and reapply after display changes",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        kControlStartup);
    SendMessageW(g_startup, BM_SETCHECK, IsStartupEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    g_reset = CreateControl(
        L"BUTTON", L"Reset this display", BS_PUSHBUTTON | WS_TABSTOP, kControlReset);
    g_status = CreateControl(
        L"STATIC", L"Starting…", SS_LEFT | SS_NOPREFIX, kControlStatus);

    RecreateFonts();
    LayoutControls();
}

void PaintCalibrationPattern(const HWND window) {
    PAINTSTRUCT paint{};
    HDC destination = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    HDC buffer = CreateCompatibleDC(destination);
    HBITMAP bitmap = CreateCompatibleBitmap(destination, std::max(1, width), std::max(1, height));
    HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);

    HBRUSH background = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(buffer, &client, background);
    DeleteObject(background);

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(214, 220, 228));
    HGDIOBJ oldPen = SelectObject(buffer, borderPen);
    HGDIOBJ oldBrush = SelectObject(buffer, GetStockObject(NULL_BRUSH));
    RoundRect(buffer, 0, 0, std::max(1, width), std::max(1, height), 12, 12);

    SetBkMode(buffer, TRANSPARENT);
    SetTextColor(buffer, RGB(64, 72, 86));
    HGDIOBJ oldFont = SelectObject(buffer, g_smallFont != nullptr ? g_smallFont : GetStockObject(DEFAULT_GUI_FONT));
    RECT titleRect{16, 9, width - 16, 30};
    DrawTextW(buffer, L"Visual calibration target", -1, &titleRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    const int availableHeight = std::max(50, height - 52);
    const int radius = std::clamp(availableHeight / 3, 19, 35);
    const int centerY = 35 + radius;
    const std::array<int, 3> centerGray = {116, 128, 140};
    const std::array<const wchar_t*, 3> captions = {L"visible", L"faint", L"visible"};

    for (int target = 0; target < 3; ++target) {
        const int centerX = width * (target + 1) / 4;
        HRGN circle = CreateEllipticRgn(
            centerX - radius,
            centerY - radius,
            centerX + radius + 1,
            centerY + radius + 1);
        const int savedDc = SaveDC(buffer);
        SelectClipRgn(buffer, circle);
        for (int x = centerX - radius; x <= centerX + radius; x += 2) {
            RECT stripe{x, centerY - radius, x + 2, centerY + radius + 1};
            HBRUSH stripeBrush = CreateSolidBrush(
                ((x - centerX + radius) / 2) % 2 == 0
                    ? RGB(72, 72, 72)
                    : RGB(184, 184, 184));
            FillRect(buffer, &stripe, stripeBrush);
            DeleteObject(stripeBrush);
        }
        RestoreDC(buffer, savedDc);
        DeleteObject(circle);

        SelectObject(buffer, borderPen);
        SelectObject(buffer, GetStockObject(NULL_BRUSH));
        Ellipse(
            buffer,
            centerX - radius,
            centerY - radius,
            centerX + radius + 1,
            centerY + radius + 1);

        const int dotRadius = std::max(7, radius / 3);
        HBRUSH dotBrush = CreateSolidBrush(RGB(
            centerGray[static_cast<std::size_t>(target)],
            centerGray[static_cast<std::size_t>(target)],
            centerGray[static_cast<std::size_t>(target)]));
        SelectObject(buffer, dotBrush);
        Ellipse(
            buffer,
            centerX - dotRadius,
            centerY - dotRadius,
            centerX + dotRadius + 1,
            centerY + dotRadius + 1);
        SelectObject(buffer, GetStockObject(NULL_BRUSH));
        DeleteObject(dotBrush);

        RECT captionRect{
            centerX - radius - 12,
            centerY + radius + 2,
            centerX + radius + 12,
            height - 5};
        DrawTextW(
            buffer,
            captions[static_cast<std::size_t>(target)],
            -1,
            &captionRect,
            DT_CENTER | DT_SINGLELINE | DT_TOP);
    }

    SelectObject(buffer, oldFont);
    SelectObject(buffer, oldBrush);
    SelectObject(buffer, oldPen);
    DeleteObject(borderPen);
    BitBlt(destination, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    EndPaint(window, &paint);
}

LRESULT CALLBACK PatternWindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            PaintCalibrationPattern(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT CALLBACK MainWindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    if (message == g_taskbarCreatedMessage && g_taskbarCreatedMessage != 0U) {
        g_trayIconAdded = false;
        AddTrayIcon();
        return 0;
    }

    switch (message) {
        case WM_CREATE: {
            g_mainWindow = window;
            LoadHotkeySettings();
            CreateMainControls();
            AddTrayIcon();
            WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
            RefreshDisplays(true);
            PopulateDisplayCombo();
            UpdateSelectedDisplayUi();
            std::wstring hotkeyError;
            if (!InstallLoadedSingleKey(hotkeyError)) {
                SetStatus(hotkeyError, true);
            }
            return 0;
        }

        case WM_SIZE:
            LayoutControls();
            InvalidateRect(window, nullptr, TRUE);
            return 0;

        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(
                window,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            RecreateFonts();
            LayoutControls();
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* minMax = reinterpret_cast<MINMAXINFO*>(lParam);
            minMax->ptMinTrackSize.x = ScaleForDpi(window, 620);
            minMax->ptMinTrackSize.y = ScaleForDpi(window, 660);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client, g_backgroundBrush);
            HPEN separator = CreatePen(PS_SOLID, 1, RGB(220, 225, 232));
            HGDIOBJ oldPen = SelectObject(dc, separator);
            const int y = ScaleForDpi(window, 109);
            MoveToEx(dc, ScaleForDpi(window, 32), y, nullptr);
            LineTo(dc, client.right - ScaleForDpi(window, 32), y);
            SelectObject(dc, oldPen);
            DeleteObject(separator);
            EndPaint(window, &paint);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            SetBkMode(dc, TRANSPARENT);
            if (control == g_status) {
                SetTextColor(dc, g_statusIsError ? RGB(180, 42, 42) : RGB(32, 116, 76));
            } else if (control == g_subtitle || control == g_hint ||
                       control == g_minimumLabel || control == g_maximumLabel) {
                SetTextColor(dc, RGB(92, 101, 116));
            } else {
                SetTextColor(dc, RGB(28, 33, 41));
            }
            return reinterpret_cast<LRESULT>(g_backgroundBrush);
        }

        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == g_gammaSlider && !g_updatingControls) {
                const double gamma = SliderGamma();
                SetWindowTextW(g_gammaValue, FormatGamma(gamma).c_str());
                KillTimer(window, kApplyTimerId);
                const int scrollCode = LOWORD(wParam);
                if (scrollCode == TB_ENDTRACK || scrollCode == TB_THUMBPOSITION) {
                    ApplySliderValue();
                } else {
                    SetTimer(window, kApplyTimerId, 90U, nullptr);
                }
            }
            return 0;

        case WM_TIMER:
            if (wParam == kApplyTimerId) {
                KillTimer(window, kApplyTimerId);
                ApplySliderValue();
                return 0;
            }
            if (wParam == kDisplayRefreshTimerId) {
                KillTimer(window, kDisplayRefreshTimerId);
                RefreshDisplays(true);
                PopulateDisplayCombo();
                UpdateSelectedDisplayUi();
                return 0;
            }
            break;

        case WM_COMMAND: {
            const int identifier = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            if (identifier == kControlDisplayCombo && notification == CBN_SELCHANGE &&
                !g_updatingControls) {
                const LRESULT selection = SendMessageW(g_displayCombo, CB_GETCURSEL, 0, 0);
                if (selection != CB_ERR) {
                    const LRESULT data = SendMessageW(
                        g_displayCombo,
                        CB_GETITEMDATA,
                        static_cast<WPARAM>(selection),
                        0);
                    if (data != CB_ERR && data >= 0 &&
                        data < static_cast<LRESULT>(g_displays.size())) {
                        g_selectedDisplay = static_cast<int>(data);
                        SaveSelectedDevice(
                            g_displays[static_cast<std::size_t>(g_selectedDisplay)].deviceName);
                        UpdateSelectedDisplayUi();
                    }
                }
                return 0;
            }
            if (identifier == kControlReset && notification == BN_CLICKED) {
                ResetSelectedDisplay();
                return 0;
            }
            if (identifier == kControlSaveHotkey && notification == BN_CLICKED) {
                SaveHotkeyFromControl();
                return 0;
            }
            if (identifier == kControlClearHotkey && notification == BN_CLICKED) {
                ClearSavedHotkey();
                return 0;
            }
            if (identifier == kControlStartup && notification == BN_CLICKED) {
                const bool requested =
                    SendMessageW(g_startup, BM_GETCHECK, 0, 0) == BST_CHECKED;
                std::wstring error;
                if (!SetStartupEnabled(requested, error)) {
                    SendMessageW(
                        g_startup,
                        BM_SETCHECK,
                        requested ? BST_UNCHECKED : BST_CHECKED,
                        0);
                    SetStatus(error, true);
                } else {
                    SetStatus(
                        requested
                            ? L"Startup enabled · Gamma Changer will run in the notification area"
                            : L"Startup disabled",
                        false);
                }
                return 0;
            }
            if (identifier == kMenuOpen) {
                ShowMainWindow();
                return 0;
            }
            if (identifier == kMenuResetAllAndExit) {
                ResetAllDisplays();
                DestroyWindow(window);
                return 0;
            }
            if (identifier == kMenuExit) {
                DestroyWindow(window);
                return 0;
            }
            break;
        }

        case WM_DISPLAYCHANGE:
        case WM_DEVICECHANGE:
            KillTimer(window, kDisplayRefreshTimerId);
            SetTimer(window, kDisplayRefreshTimerId, 1200U, nullptr);
            return 0;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                KillTimer(window, kDisplayRefreshTimerId);
                SetTimer(window, kDisplayRefreshTimerId, 1200U, nullptr);
            }
            return TRUE;

        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_UNLOCK || wParam == WTS_CONSOLE_CONNECT ||
                wParam == WTS_REMOTE_CONNECT) {
                KillTimer(window, kDisplayRefreshTimerId);
                SetTimer(window, kDisplayRefreshTimerId, 1200U, nullptr);
            }
            return 0;

        case kSingleKeyMessage:
            ToggleMaximumGamma();
            return 0;

        case kTrayMessage:
            if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
                ShowMainWindow();
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayMenu();
            }
            return 0;

        case kShowWindowMessage:
            ShowMainWindow();
            return 0;

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            KillTimer(window, kApplyTimerId);
            KillTimer(window, kDisplayRefreshTimerId);
            WTSUnRegisterSessionNotification(window);
            RemoveSingleKeyHook();
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterWindowClasses() {
    WNDCLASSEXW patternClass{};
    patternClass.cbSize = sizeof(patternClass);
    patternClass.style = CS_HREDRAW | CS_VREDRAW;
    patternClass.lpfnWndProc = PatternWindowProc;
    patternClass.hInstance = g_instance;
    patternClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    patternClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    patternClass.lpszClassName = kPatternClassName;
    if (RegisterClassExW(&patternClass) == 0U) {
        return false;
    }

    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.style = CS_HREDRAW | CS_VREDRAW;
    mainClass.lpfnWndProc = MainWindowProc;
    mainClass.hInstance = g_instance;
    mainClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hbrBackground = g_backgroundBrush;
    mainClass.lpszClassName = kWindowClassName;
    mainClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    return RegisterClassExW(&mainClass) != 0U;
}

bool HasBackgroundArgument() {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return false;
    }
    bool background = false;
    for (int index = 1; index < argumentCount; ++index) {
        if (_wcsicmp(arguments[index], L"--background") == 0) {
            background = true;
            break;
        }
    }
    LocalFree(arguments);
    return background;
}

}  // namespace

int APIENTRY wWinMain(
    const HINSTANCE instance,
    HINSTANCE,
    wchar_t*,
    const int showCommand) {
    g_instance = instance;
    const bool startInBackground = HasBackgroundArgument();

    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, kSingleInstanceName);
    if (instanceMutex == nullptr) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!startInBackground) {
            const HWND existing = FindWindowW(kWindowClassName, nullptr);
            if (existing != nullptr) {
                PostMessageW(existing, kShowWindowMessage, 0, 0);
            }
        }
        CloseHandle(instanceMutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_HOTKEY_CLASS;
    InitCommonControlsEx(&commonControls);

    g_backgroundBrush = CreateSolidBrush(RGB(247, 249, 252));
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    if (!RegisterWindowClasses()) {
        DeleteObject(g_backgroundBrush);
        CloseHandle(instanceMutex);
        return 1;
    }

    const int width = ScaleForDpi(nullptr, 730);
    const int height = ScaleForDpi(nullptr, 700);
    HWND window = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr) {
        DeleteObject(g_backgroundBrush);
        CloseHandle(instanceMutex);
        return 1;
    }

    if (!startInBackground) {
        ShowWindow(window, showCommand);
        UpdateWindow(window);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (g_headerFont != nullptr) {
        DeleteObject(g_headerFont);
    }
    if (g_valueFont != nullptr) {
        DeleteObject(g_valueFont);
    }
    if (g_bodyFont != nullptr) {
        DeleteObject(g_bodyFont);
    }
    if (g_smallFont != nullptr) {
        DeleteObject(g_smallFont);
    }
    DeleteObject(g_backgroundBrush);
    CloseHandle(instanceMutex);
    return static_cast<int>(message.wParam);
}
