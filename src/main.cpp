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
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "auto_gamma.h"
#include "gamma_math.h"
#include "gamma_toggle.h"
#include "resource.h"
#include "single_key.h"
#include "single_key_win32.h"

namespace {

using gamma_changer::ApplyRelativeGamma;
using gamma_changer::GammaRamp;
using gamma_changer::RampsApproximatelyEqual;

constexpr wchar_t kWindowClassName[] = L"GammaChanger.MainWindow";
constexpr wchar_t kWindowTitle[] = L"Gamma Changer";
constexpr wchar_t kAppRegistryPath[] = L"Software\\GammaChanger";
constexpr wchar_t kSingleInstanceName[] = L"Local\\GammaChanger.SingleInstance.v1";

constexpr UINT kTrayIconId = 1U;
constexpr UINT kTrayMessage = WM_APP + 1U;
constexpr UINT kShowWindowMessage = WM_APP + 2U;
constexpr UINT kSingleKeyMessage = WM_APP + 3U;
constexpr UINT kKeyCapturedMessage = WM_APP + 4U;
constexpr UINT kMapKeyStateMessage = WM_APP + 5U;
constexpr UINT kForegroundChangedMessage = WM_APP + 6U;
constexpr UINT_PTR kApplyTimerId = 1U;
constexpr UINT_PTR kDisplayRefreshTimerId = 2U;
constexpr UINT_PTR kAutoGammaTimerId = 3U;
constexpr UINT_PTR kMapReleaseDelayTimerId = 4U;
constexpr UINT kAutoGammaIntervalMilliseconds = 500U;
constexpr UINT kAutoGammaIdleIntervalMilliseconds = 2000U;
constexpr UINT kMapReleaseDelayMilliseconds = 100U;
constexpr wchar_t kRustClientExecutable[] = L"RustClient.exe";
constexpr UINT kRustMapVirtualKey = 'G';

constexpr int kControlDisplayLabel = 1003;
constexpr int kControlDisplayCombo = 1004;
constexpr int kControlGammaLabel = 1005;
constexpr int kControlGammaValue = 1006;
constexpr int kControlGammaSlider = 1007;
constexpr int kControlHotkeyLabel = 1015;
constexpr int kControlHotkey = 1016;
constexpr int kControlClearHotkey = 1018;

constexpr int kControlToggleLabel = 1019;
constexpr int kControlToggleFirst = 1020;
constexpr int kControlToggleSecond = 1021;
constexpr int kControlAutoRust = 1022;

constexpr int kMenuOpen = 2001;
constexpr int kMenuResetAllAndExit = 2002;

struct NativeGammaRamp {
    WORD red[gamma_changer::kGammaRampEntries];
    WORD green[gamma_changer::kGammaRampEntries];
    WORD blue[gamma_changer::kGammaRampEntries];
};

static_assert(sizeof(NativeGammaRamp) == 3U * 256U * sizeof(WORD));

struct ScopedIcon {
    HICON value = nullptr;

    ScopedIcon(HINSTANCE instance, const int widthMetric, const int heightMetric)
        : value(static_cast<HICON>(LoadImageW(
              instance,
              MAKEINTRESOURCEW(IDI_GAMMA_CHANGER),
              IMAGE_ICON,
              GetSystemMetrics(widthMetric),
              GetSystemMetrics(heightMetric),
              LR_DEFAULTCOLOR))) {}

    ScopedIcon(const ScopedIcon&) = delete;
    ScopedIcon& operator=(const ScopedIcon&) = delete;

    ~ScopedIcon() {
        if (value != nullptr) {
            DestroyIcon(value);
        }
    }
};

struct ScopedHandle {
    HANDLE value = nullptr;

    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : value(handle) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        if (value != nullptr) {
            CloseHandle(value);
        }
    }

    explicit operator bool() const noexcept { return value != nullptr; }
};

struct ScopedDc {
    HDC value = nullptr;
    bool releaseDc = false;

    ScopedDc(HDC dc, const bool release) : value(dc), releaseDc(release) {}
    ScopedDc(const ScopedDc&) = delete;
    ScopedDc& operator=(const ScopedDc&) = delete;

    ~ScopedDc() {
        if (value == nullptr) {
            return;
        }
        if (releaseDc) {
            ReleaseDC(nullptr, value);
        } else {
            DeleteDC(value);
        }
    }
};

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

struct DisplayState : PersistentDisplaySettings {
    std::wstring deviceName;
    std::wstring label;
    bool primary = false;
    int width = 0;
    int height = 0;
    bool rampReadable = false;
    bool automaticReapplyPaused = false;
    bool autoControlled = false;
    // The baseline ramp rarely changes; only rewrite it to the registry when it does.
    bool baselineDirty = false;
    std::wstring lastError;
};

struct SavedAutoControlState {
    std::wstring deviceName;
    double gamma = 1.0;
    bool enabled = false;
};

HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
HWND g_displayLabel = nullptr;
HWND g_displayCombo = nullptr;
HWND g_gammaLabel = nullptr;
HWND g_gammaValue = nullptr;
HWND g_gammaSlider = nullptr;
HWND g_hotkeyLabel = nullptr;
HWND g_hotkeyControl = nullptr;
HWND g_clearHotkey = nullptr;
HWND g_toggleLabel = nullptr;
HWND g_toggleFirst = nullptr;
HWND g_toggleSecond = nullptr;
HWND g_autoRustCheckbox = nullptr;
double g_toggleFirstGamma = 1.0;
double g_toggleSecondGamma = 3.0;
HFONT g_valueFont = nullptr;
HFONT g_bodyFont = nullptr;
HBRUSH g_backgroundBrush = nullptr;
std::vector<DisplayState> g_displays;
int g_selectedDisplay = -1;
bool g_updatingControls = false;
bool g_trayIconAdded = false;
UINT g_taskbarCreatedMessage = 0U;
gamma_changer::SingleKeyInput g_singleKeyInput;
UINT g_pendingHotkeyVirtualKey = 0U;
HHOOK g_keyboardHook = nullptr;
HWINEVENTHOOK g_foregroundHook = nullptr;
UINT g_autoTimerInterval = 0U;
bool g_sliderApplyPending = false;
bool g_autoRustEnabled = false;
bool g_rustMapKeyHeld = false;
std::optional<SavedAutoControlState> g_savedAutoState;
gamma_changer::AutoGammaDetector g_autoGammaDetector;

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
    const long long hundredths = std::llround(gamma * 100.0);
    const long long magnitude = hundredths < 0 ? -hundredths : hundredths;
    std::wstring text = hundredths < 0 ? L"-" : L"";
    text += std::to_wstring(magnitude / 100);
    text += L'.';
    if (magnitude % 100 < 10) {
        text += L'0';
    }
    text += std::to_wstring(magnitude % 100);
    return text;
}

bool IsNeutralGamma(const double gamma) noexcept {
    return std::abs(gamma - 1.0) < 0.0001;
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
    HDC deviceContext,
    GammaRamp& ramp,
    std::wstring& errorMessage) {
    NativeGammaRamp native{};
    SetLastError(ERROR_SUCCESS);
    const BOOL succeeded = GetDeviceGammaRamp(deviceContext, &native);
    const DWORD error = GetLastError();
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

bool ReadDeviceRamp(
    const std::wstring& deviceName,
    GammaRamp& ramp,
    std::wstring& errorMessage) {
    ScopedDc deviceContext(
        CreateDCW(L"DISPLAY", deviceName.c_str(), nullptr, nullptr),
        false);
    if (deviceContext.value == nullptr) {
        errorMessage = L"Could not open " + deviceName + L": " +
                       FormatSystemError(GetLastError());
        return false;
    }

    return ReadDeviceRamp(deviceContext.value, ramp, errorMessage);
}

bool WriteDeviceRamp(
    const std::wstring& deviceName,
    const GammaRamp& requested,
    GammaRamp& actual,
    std::wstring& errorMessage) {
    ScopedDc deviceContext(
        CreateDCW(L"DISPLAY", deviceName.c_str(), nullptr, nullptr),
        false);
    if (deviceContext.value == nullptr) {
        errorMessage = L"Could not open " + deviceName + L": " +
                       FormatSystemError(GetLastError());
        return false;
    }

    NativeGammaRamp native = ToNativeRamp(requested);
    SetLastError(ERROR_SUCCESS);
    const BOOL succeeded = SetDeviceGammaRamp(deviceContext.value, &native);
    const DWORD setError = GetLastError();
    if (succeeded == FALSE) {
        errorMessage = L"The display driver rejected the gamma ramp";
        if (setError != ERROR_SUCCESS) {
            errorMessage += L": " + FormatSystemError(setError);
        }
        return false;
    }

    // Verify through the context we already opened for this write.
    std::wstring readError;
    const bool verified =
        ReadDeviceRamp(deviceContext.value, actual, readError);
    if (!verified) {
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
    std::wstring path = kAppRegistryPath;
    path += L"\\Displays\\";
    const std::uint64_t hash = HashDeviceName(deviceName);
    for (int shift = 60; shift >= 0; shift -= 4) {
        path += L"0123456789abcdef"[(hash >> shift) & 0xFU];
    }
    return path;
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

bool WriteRamp(HKEY key, const wchar_t* name, const GammaRamp& ramp) {
    const NativeGammaRamp native = ToNativeRamp(ramp);
    return RegSetValueExW(
        key,
        name,
        0,
        REG_BINARY,
        reinterpret_cast<const BYTE*>(&native),
        sizeof(native)) == ERROR_SUCCESS;
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

void SaveDisplaySettings(DisplayState& display) {
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
    if (display.baselineDirty || !display.hasBaseline) {
        const DWORD nameBytes = static_cast<DWORD>(
            (display.deviceName.size() + 1U) * sizeof(wchar_t));
        RegSetValueExW(
            key.value,
            L"DeviceName",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(display.deviceName.c_str()),
            nameBytes);
    }
    if (display.hasBaseline && display.baselineDirty &&
        WriteRamp(key.value, L"BaselineRamp", display.baseline)) {
        display.baselineDirty = false;
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

    DWORD value = 0U;
    if (ReadDword(key.value, L"ToggleFirstGammaMilli", value)) {
        g_toggleFirstGamma = gamma_changer::ClampGamma(value / 1000.0);
    }
    if (ReadDword(key.value, L"ToggleSecondGammaMilli", value)) {
        g_toggleSecondGamma = gamma_changer::ClampGamma(value / 1000.0);
    }

    DWORD virtualKey = 0U;
    if (ReadDword(key.value, L"HotkeyVirtualKey", virtualKey)) {
        g_singleKeyInput.SetBinding(virtualKey);
    }

    DWORD autoRust = 0U;
    if (ReadDword(key.value, L"AutoRustEnabled", autoRust)) {
        g_autoRustEnabled = autoRust != 0U;
    }
}

bool SaveAutoRustSetting(const bool enabled, std::wstring& errorMessage) {
    auto key = OpenRegistryKey(kAppRegistryPath, KEY_SET_VALUE, true);
    if (!key) {
        errorMessage = L"Could not save the automatic Rust setting.";
        return false;
    }
    const DWORD value = enabled ? 1U : 0U;
    const LSTATUS status = RegSetValueExW(
        key.value,
        L"AutoRustEnabled",
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&value),
        sizeof(value));
    if (status != ERROR_SUCCESS) {
        errorMessage =
            L"Could not save the automatic Rust setting: " + FormatSystemError(status);
        return false;
    }
    return true;
}

bool SaveHotkeySettings(const DWORD virtualKey, std::wstring& errorMessage) {
    auto key = OpenRegistryKey(kAppRegistryPath, KEY_SET_VALUE, true);
    if (!key) {
        errorMessage = L"Could not save the hotkey for the current user.";
        return false;
    }
    const LSTATUS status = RegSetValueExW(
        key.value,
        L"HotkeyVirtualKey",
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&virtualKey),
        sizeof(virtualKey));
    if (status != ERROR_SUCCESS) {
        errorMessage = L"Could not save the single key: " + FormatSystemError(status);
        return false;
    }
    // Combination settings are obsolete; only the single virtual key is used.
    RegDeleteValueW(key.value, L"HotkeyFlags");
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

        std::wstring label = friendlyName;
        if (display.width > 0 && display.height > 0) {
            label += L"  ·  " + std::to_wstring(display.width) + L" × " +
                     std::to_wstring(display.height);
        }
        if (display.primary) {
            label += L"  ·  Primary";
        }
        label += L"  (" + display.deviceName + L")";
        display.label = std::move(label);
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

void ReportError(const std::wstring& message) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_mainWindow;
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_ERROR;
    lstrcpynW(data.szInfoTitle, kWindowTitle, ARRAYSIZE(data.szInfoTitle));
    lstrcpynW(data.szInfo, message.c_str(), ARRAYSIZE(data.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

// A persistent driver error is re-evaluated on every UI refresh, including each
// automatic gamma switch; show the same balloon at most once every few seconds.
void ReportDisplayError(const std::wstring& message) {
    static std::wstring lastMessage;
    static ULONGLONG lastTick = 0U;
    const ULONGLONG now = GetTickCount64();
    if (message == lastMessage && now - lastTick < 5000U) {
        return;
    }
    lastMessage = message;
    lastTick = now;
    ReportError(message);
}

bool SingleKeyIsAllowed(const UINT virtualKey, std::wstring& errorMessage) {
    if (!gamma_changer::IsSingleKeyAllowed(virtualKey)) {
        errorMessage = L"Click the key box and press one key, such as Right Shift.";
        return false;
    }
    return true;
}

LRESULT CALLBACK SingleKeyKeyboardProc(
    const int code,
    const WPARAM message,
    const LPARAM data) {
    if (code == HC_ACTION) {
        const auto* keyboard = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        const bool isKeyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const bool isKeyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
        if (isKeyDown || isKeyUp) {
            if (keyboard->vkCode == kRustMapVirtualKey &&
                g_rustMapKeyHeld != isKeyDown) {
                g_rustMapKeyHeld = isKeyDown;
                PostMessageW(
                    g_mainWindow,
                    kMapKeyStateMessage,
                    isKeyDown ? 1U : 0U,
                    0);
            }
            const bool keyBoxFocused = GetForegroundWindow() == g_mainWindow &&
                                       GetFocus() == g_hotkeyControl;
            if (!keyBoxFocused) {
                g_singleKeyInput.CancelCapture();
            }
            const UINT key = gamma_changer::SingleKeyFromHook(*keyboard);
            const auto result = g_singleKeyInput.Process(key, isKeyDown, !keyBoxFocused);
            if (result.action == gamma_changer::SingleKeyAction::Captured) {
                g_pendingHotkeyVirtualKey = key;
                PostMessageW(g_mainWindow, kKeyCapturedMessage, 0, 0);
            } else if (result.action == gamma_changer::SingleKeyAction::Toggle) {
                PostMessageW(g_mainWindow, kSingleKeyMessage, 0, 0);
            }
            if (result.suppress) {
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
    g_singleKeyInput.CancelCapture();
    g_rustMapKeyHeld = false;
}

bool InstallSingleKeyHook(std::wstring& errorMessage) {
    if (g_keyboardHook != nullptr) {
        return true;
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
    g_pendingHotkeyVirtualKey = virtualKey;
    if (g_hotkeyControl != nullptr) {
        SetWindowTextW(g_hotkeyControl, gamma_changer::SingleKeyName(virtualKey).c_str());
    }
}

bool InstallLoadedSingleKey(std::wstring& errorMessage) {
    SetHotkeyControlValue(g_singleKeyInput.BoundKey());
    // The hook also records modifier-only keys when no shortcut is saved yet.
    return InstallSingleKeyHook(errorMessage);
}

void BeginSingleKeyCapture(const HWND window) {
    std::wstring error;
    if (!InstallSingleKeyHook(error)) {
        ReportError(error);
        return;
    }
    g_singleKeyInput.BeginCapture();
    SetWindowTextW(window, L"Press one key...");
}

LRESULT CALLBACK SingleKeyControlProc(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam,
    const UINT_PTR subclassId,
    DWORD_PTR) {
    switch (message) {
        case WM_SETFOCUS:
            BeginSingleKeyCapture(window);
            break;
        case WM_LBUTTONDOWN:
            if (GetFocus() == window) {
                BeginSingleKeyCapture(window);
            }
            break;
        case WM_KILLFOCUS:
            g_singleKeyInput.CancelCapture();
            SetHotkeyControlValue(g_pendingHotkeyVirtualKey);
            break;
        case WM_GETDLGCODE:
            if (g_singleKeyInput.IsCapturing()) {
                return DLGC_WANTALLKEYS;
            }
            break;
        case WM_CHAR:
        case WM_SYSCHAR:
            return 0;
        case WM_NCDESTROY:
            g_singleKeyInput.CancelCapture();
            RemoveWindowSubclass(window, SingleKeyControlProc, subclassId);
            break;
        default:
            break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void SaveHotkeyFromControl() {
    const UINT requestedVirtualKey = g_pendingHotkeyVirtualKey;
    std::wstring error;
    if (!SingleKeyIsAllowed(requestedVirtualKey, error)) {
        ReportError(error);
        return;
    }

    if (!InstallSingleKeyHook(error) || !SaveHotkeySettings(requestedVirtualKey, error)) {
        SetHotkeyControlValue(g_singleKeyInput.BoundKey());
        ReportError(error);
        return;
    }

    g_singleKeyInput.SetBinding(requestedVirtualKey);
    g_singleKeyInput.CancelCapture();
    SetHotkeyControlValue(requestedVirtualKey);
    SetFocus(g_gammaSlider);
}

void ClearSavedHotkey() {
    std::wstring error;
    if (!SaveHotkeySettings(0U, error)) {
        ReportError(error);
        return;
    }

    g_singleKeyInput.SetBinding(0U);
    g_singleKeyInput.CancelCapture();
    SetHotkeyControlValue(0U);
}

bool ApplyGammaToDisplay(
    DisplayState& display,
    const double requestedGamma,
    const bool enabled,
    std::wstring& errorMessage,
    const bool persistSettings = true) {
    if (!display.hasBaseline) {
        errorMessage = L"No readable baseline gamma ramp is available for this display.";
        display.lastError = errorMessage;
        display.automaticReapplyPaused = true;
        return false;
    }

    const double gamma = enabled ? gamma_changer::ClampGamma(requestedGamma) : 1.0;
    const GammaRamp target = ApplyRelativeGamma(display.baseline, gamma);
    GammaRamp actual{};
    if (!WriteDeviceRamp(display.deviceName, target, actual, errorMessage)) {
        // Readback can be delayed or overwritten. A second write to the baseline
        // can cause a bright flash followed by an immediate return.
        display.lastError = errorMessage;
        display.automaticReapplyPaused = true;
        return false;
    }

    display.gamma = gamma;
    display.enabled = enabled;
    display.lastApplied = actual;
    display.hasLastApplied = true;
    display.automaticReapplyPaused = false;
    display.rampReadable = true;
    display.lastError.clear();
    if (persistSettings && !display.autoControlled) {
        SaveDisplaySettings(display);
    }
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
            display.baselineDirty = previous->baselineDirty;
            display.hasLastApplied = previous->hasLastApplied;
            display.lastError = previous->lastError;
            display.automaticReapplyPaused = previous->automaticReapplyPaused;
            display.autoControlled = previous->autoControlled;
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
                    display.baselineDirty = true;
                    SaveDisplaySettings(display);
                }
            }
        }

        if (!display.hasBaseline && display.rampReadable) {
            display.baseline = current;
            display.hasBaseline = true;
            display.baselineDirty = true;
            SaveDisplaySettings(display);
        }
        if (!display.rampReadable) {
            display.lastError = readError;
        } else if (!display.automaticReapplyPaused) {
            display.lastError.clear();
            const bool stillApplied = display.hasLastApplied &&
                RampsApproximatelyEqual(current, display.lastApplied, 0U);
            if (reapplySavedSettings && display.enabled && !stillApplied) {
                std::wstring error;
                ApplyGammaToDisplay(display, display.gamma, true, error);
            }
        }
    }

    g_displays = std::move(refreshed);

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
    HFONT newBody = CreateUiFont(g_mainWindow, 10, FW_NORMAL);
    HFONT newValue = CreateUiFont(g_mainWindow, 16, FW_SEMIBOLD);
    for (HWND control : {g_displayLabel, g_displayCombo, g_gammaLabel,
                         g_hotkeyLabel, g_hotkeyControl, g_clearHotkey,
                         g_toggleLabel, g_toggleFirst, g_toggleSecond,
                         g_autoRustCheckbox}) {
        AssignFont(control, newBody);
    }
    AssignFont(g_gammaValue, newValue);
    if (g_bodyFont != nullptr) DeleteObject(g_bodyFont);
    if (g_valueFont != nullptr) DeleteObject(g_valueFont);
    g_bodyFont = newBody;
    g_valueFont = newValue;
}

void LayoutControls() {
    RECT client{};
    GetClientRect(g_mainWindow, &client);
    const auto scale = [](int value) { return ScaleForDpi(g_mainWindow, value); };
    const int margin = scale(20);
    const int width = std::max(1, static_cast<int>(client.right) - 2 * margin);
    const auto move = [&](HWND control, int x, int y, int w, int h) {
        MoveWindow(control, x, scale(y), w, scale(h), TRUE);
    };
    move(g_displayLabel, margin, 18, width, 22);
    move(g_displayCombo, margin, 44, width, 220);
    move(g_gammaLabel, margin, 88, width / 2, 26);
    move(g_gammaValue, margin + width / 2, 82, width / 2, 32);
    move(g_gammaSlider, margin, 118, width, 36);
    move(g_hotkeyLabel, margin, 174, scale(100), 28);
    move(g_hotkeyControl, margin + scale(104), 174, width - scale(176), 28);
    move(g_clearHotkey, margin + width - scale(64), 174, scale(64), 28);
    move(g_toggleLabel, margin, 218, scale(100), 28);
    const int fieldWidth = (width - scale(116)) / 2;
    move(g_toggleFirst, margin + scale(104), 218, fieldWidth, 28);
    move(g_toggleSecond, margin + scale(116) + fieldWidth, 218, fieldWidth, 28);
    move(g_autoRustCheckbox, margin, 262, width, 28);
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

    if (!hasDisplay) {
        SetWindowTextW(g_gammaValue, L"—");
        ReportDisplayError(L"No active desktop display was found.");
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
    SetWindowTextW(g_gammaValue, display.automaticReapplyPaused ? L"—" : FormatGamma(gamma).c_str());

    if (!display.lastError.empty()) {
        ReportDisplayError(display.lastError);
        if (!display.rampReadable || !display.hasBaseline) {
            EnableWindow(g_gammaSlider, FALSE);
        }
    } else if (!display.rampReadable || !display.hasBaseline) {
        ReportDisplayError(
            L"This display driver does not provide a usable SDR hardware gamma ramp.");
        EnableWindow(g_gammaSlider, FALSE);
    }
    g_updatingControls = false;
}

int FindDisplayIndex(const std::wstring& deviceName) {
    for (std::size_t index = 0; index < g_displays.size(); ++index) {
        if (_wcsicmp(g_displays[index].deviceName.c_str(), deviceName.c_str()) == 0) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

std::wstring ProcessExecutableName(const DWORD processId) {
    ScopedHandle process(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
    if (!process) {
        return {};
    }

    // Most paths fit in MAX_PATH; grow only for long-path installs.
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD length = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process.value, 0U, path.data(), &length) != FALSE) {
            path.resize(length);
            break;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || path.size() >= 32768U) {
            return {};
        }
        path.assign(path.size() * 2U, L'\0');
    }

    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1U);
}

HWND RustForegroundWindow() {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr || IsWindowVisible(foreground) == FALSE ||
        IsIconic(foreground) != FALSE) {
        return nullptr;
    }

    DWORD processId = 0U;
    GetWindowThreadProcessId(foreground, &processId);
    if (processId == 0U) {
        return nullptr;
    }

    // This runs on every auto-gamma tick. Resolve the image name only when the
    // foreground process changes instead of opening a process handle each time.
    static DWORD cachedProcessId = 0U;
    static bool cachedIsRust = false;
    if (processId != cachedProcessId) {
        cachedIsRust = _wcsicmp(
                           ProcessExecutableName(processId).c_str(),
                           kRustClientExecutable) == 0;
        cachedProcessId = processId;
    }
    return cachedIsRust ? foreground : nullptr;
}

std::wstring DisplayDeviceForWindow(const HWND window) {
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr ||
        GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)) == FALSE) {
        return {};
    }
    return info.szDevice;
}

const std::array<double, 256>& LinearSrgbTable() {
    static const std::array<double, 256> table = [] {
        std::array<double, 256> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            const double channel = static_cast<double>(index) / 255.0;
            values[index] = channel <= 0.04045
                                ? channel / 12.92
                                : std::pow((channel + 0.055) / 1.055, 2.4);
        }
        return values;
    }();
    return table;
}

bool SampleWindowLuminance(const HWND window, double& luminance) {
    RECT client{};
    if (GetClientRect(window, &client) == FALSE) {
        return false;
    }
    POINT origin{0, 0};
    if (ClientToScreen(window, &origin) == FALSE) {
        return false;
    }

    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    if (clientWidth < 200 || clientHeight < 150) {
        return false;
    }

    const int cropX = clientWidth / 8;
    const int cropY = clientHeight / 8;
    const int sourceX = origin.x + cropX;
    const int sourceY = origin.y + cropY;
    const int sourceWidth = clientWidth - 2 * cropX;
    const int sourceHeight = clientHeight - 2 * cropY;

    constexpr int kSampleWidth = 80;
    constexpr int kSampleHeight = 60;

    // The capture surface is reused across ticks; recreating a DC and DIB
    // section twice a second is pure overhead.
    struct SampleSurface {
        HDC memory = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ previous = nullptr;
        void* pixels = nullptr;

        SampleSurface() = default;
        SampleSurface(const SampleSurface&) = delete;
        SampleSurface& operator=(const SampleSurface&) = delete;

        ~SampleSurface() {
            if (memory != nullptr) {
                if (previous != nullptr) {
                    SelectObject(memory, previous);
                }
                DeleteDC(memory);
            }
            if (bitmap != nullptr) {
                DeleteObject(bitmap);
            }
        }

        bool Create(HDC screen) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = kSampleWidth;
            info.bmiHeader.biHeight = -kSampleHeight;
            info.bmiHeader.biPlanes = 1U;
            info.bmiHeader.biBitCount = 32U;
            info.bmiHeader.biCompression = BI_RGB;
            memory = CreateCompatibleDC(screen);
            bitmap = CreateDIBSection(
                screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0U);
            if (memory == nullptr || bitmap == nullptr || pixels == nullptr) {
                return false;
            }
            previous = SelectObject(memory, bitmap);
            if (previous == nullptr || previous == HGDI_ERROR) {
                previous = nullptr;
                return false;
            }
            SetStretchBltMode(memory, COLORONCOLOR);
            return true;
        }
    };
    static SampleSurface surface;
    static bool surfaceReady = false;

    ScopedDc screen(GetDC(nullptr), true);
    if (screen.value == nullptr) {
        return false;
    }
    if (!surfaceReady) {
        if (!surface.Create(screen.value)) {
            return false;
        }
        surfaceReady = true;
    }

    // Nearest-neighbour decimation is far cheaper than HALFTONE on a large
    // window, and the median below does not need averaged samples.
    const BOOL copied = StretchBlt(
        surface.memory,
        0,
        0,
        kSampleWidth,
        kSampleHeight,
        screen.value,
        sourceX,
        sourceY,
        sourceWidth,
        sourceHeight,
        SRCCOPY);
    GdiFlush();
    if (copied == FALSE) {
        return false;
    }
    const void* pixels = surface.pixels;

    constexpr std::size_t kLuminanceBins = 1024U;
    std::array<std::uint32_t, kLuminanceBins> histogram{};
    std::size_t nonBlackPixels = 0U;
    const auto& linear = LinearSrgbTable();
    const auto* bytes = static_cast<const unsigned char*>(pixels);
    constexpr std::size_t pixelCount =
        static_cast<std::size_t>(kSampleWidth) * kSampleHeight;
    for (std::size_t index = 0; index < pixelCount; ++index) {
        const std::size_t offset = index * 4U;
        if (bytes[offset] != 0U || bytes[offset + 1U] != 0U ||
            bytes[offset + 2U] != 0U) {
            ++nonBlackPixels;
        }
        const double pixelLuminance =
            linear[bytes[offset + 2U]] * 0.2126 +
            linear[bytes[offset + 1U]] * 0.7152 +
            linear[bytes[offset]] * 0.0722;
        const std::size_t bin = std::min(
            static_cast<std::size_t>(pixelLuminance * kLuminanceBins),
            kLuminanceBins - 1U);
        ++histogram[bin];
    }

    // Screen capture protection and exclusive-fullscreen surfaces commonly
    // return an all-black image. Do not mistake that for an in-game night.
    if (nonBlackPixels < pixelCount / 100U) {
        return false;
    }
    // The median is resistant to HUD elements, muzzle flashes, and small lamps.
    const std::size_t medianTarget = pixelCount / 2U;
    std::size_t accumulated = 0U;
    for (std::size_t bin = 0; bin < histogram.size(); ++bin) {
        accumulated += histogram[bin];
        if (accumulated > medianTarget) {
            luminance =
                (static_cast<double>(bin) + 0.5) /
                static_cast<double>(kLuminanceBins);
            break;
        }
    }
    return true;
}

int AutoControlledDisplayIndex() {
    return g_savedAutoState
               ? FindDisplayIndex(g_savedAutoState->deviceName)
               : -1;
}

bool AutoControlsDisplay(const DisplayState& display) {
    return g_savedAutoState &&
           _wcsicmp(
               g_savedAutoState->deviceName.c_str(),
               display.deviceName.c_str()) == 0;
}

void DiscardSavedAutoState() {
    const int index = AutoControlledDisplayIndex();
    if (index >= 0) {
        g_displays[static_cast<std::size_t>(index)].autoControlled = false;
    }
    g_savedAutoState.reset();
    g_autoGammaDetector.Reset();
}

void RestoreAutoControlledDisplay() {
    if (!g_savedAutoState) {
        return;
    }
    const SavedAutoControlState saved = *g_savedAutoState;
    DiscardSavedAutoState();

    const int index = FindDisplayIndex(saved.deviceName);
    if (index < 0) {
        return;
    }
    DisplayState& display = g_displays[static_cast<std::size_t>(index)];
    display.autoControlled = false;
    std::wstring error;
    if (!ApplyGammaToDisplay(display, saved.gamma, saved.enabled, error)) {
        ReportError(L"Could not restore gamma after automatic Rust mode: " + error);
    }
    if (index == g_selectedDisplay) {
        UpdateSelectedDisplayUi();
    }
}

void NeutralizeAutoControlledDisplay() {
    const int index = AutoControlledDisplayIndex();
    if (index < 0) {
        return;
    }
    DisplayState& display = g_displays[static_cast<std::size_t>(index)];
    if (!display.enabled && IsNeutralGamma(display.gamma)) {
        return;
    }
    std::wstring error;
    if (!ApplyGammaToDisplay(display, 1.0, false, error, false)) {
        ReportError(L"Could not return gamma to neutral outside Rust: " + error);
    }
    if (index == g_selectedDisplay) {
        UpdateSelectedDisplayUi();
    }
}

void CaptureAutoControlState(DisplayState& display) {
    NeutralizeAutoControlledDisplay();
    DiscardSavedAutoState();
    g_savedAutoState = SavedAutoControlState{
        display.deviceName,
        display.gamma,
        display.enabled,
    };
    display.autoControlled = true;
}

void CALLBACK ForegroundEventProc(
    HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (g_mainWindow != nullptr) {
        PostMessageW(g_mainWindow, kForegroundChangedMessage, 0, 0);
    }
}

void SetAutoTimerInterval(const UINT interval) {
    if (g_autoTimerInterval == interval) {
        return;
    }
    if (SetTimer(g_mainWindow, kAutoGammaTimerId, interval, nullptr) != 0U) {
        g_autoTimerInterval = interval;
    }
}

// Polls quickly only while Rust is in front. The foreground hook reacts to
// Alt-Tab immediately, so the idle poll is just a safety net.
bool StartAutoGamma() {
    SetAutoTimerInterval(kAutoGammaIntervalMilliseconds);
    if (g_autoTimerInterval == 0U) {
        return false;
    }
    if (g_foregroundHook == nullptr) {
        g_foregroundHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND,
            nullptr,
            ForegroundEventProc,
            0U,
            0U,
            WINEVENT_OUTOFCONTEXT);
    }
    return true;
}

void StopAutoGamma() {
    KillTimer(g_mainWindow, kAutoGammaTimerId);
    g_autoTimerInterval = 0U;
    if (g_foregroundHook != nullptr) {
        UnhookWinEvent(g_foregroundHook);
        g_foregroundHook = nullptr;
    }
}

void UpdateAutomaticGamma() {
    if (!g_autoRustEnabled) {
        return;
    }

    const HWND rustWindow = RustForegroundWindow();
    SetAutoTimerInterval(
        rustWindow != nullptr ? kAutoGammaIntervalMilliseconds
                              : kAutoGammaIdleIntervalMilliseconds);
    if (rustWindow == nullptr) {
        g_autoGammaDetector.Reset();
        g_rustMapKeyHeld = false;
        NeutralizeAutoControlledDisplay();
        return;
    }

    int displayIndex = FindDisplayIndex(DisplayDeviceForWindow(rustWindow));
    if (displayIndex < 0) {
        displayIndex = g_selectedDisplay;
    }
    if (displayIndex < 0 ||
        displayIndex >= static_cast<int>(g_displays.size())) {
        return;
    }

    DisplayState& display = g_displays[static_cast<std::size_t>(displayIndex)];
    if (!display.hasBaseline || !display.rampReadable) {
        return;
    }

    if (!AutoControlsDisplay(display)) {
        CaptureAutoControlState(display);
    }

    const double nightGamma =
        std::max(1.0, std::max(g_toggleFirstGamma, g_toggleSecondGamma));
    g_rustMapKeyHeld =
        (GetAsyncKeyState(static_cast<int>(kRustMapVirtualKey)) & 0x8000) != 0;

    double target = 1.0;
    if (!g_rustMapKeyHeld) {
        double luminance = 0.0;
        if (!SampleWindowLuminance(rustWindow, luminance)) {
            g_autoGammaDetector.Reset();
            NeutralizeAutoControlledDisplay();
            return;
        }
        const bool night = g_autoGammaDetector.Update(luminance);
        target = gamma_changer::AutomaticGammaTarget(night, nightGamma);
    }
    const double current = display.enabled ? display.gamma : 1.0;
    if (std::abs(target - current) < 0.005) {
        return;
    }

    const bool enabled = !IsNeutralGamma(target);
    std::wstring error;
    if (!ApplyGammaToDisplay(display, target, enabled, error, false)) {
        g_autoRustEnabled = false;
        StopAutoGamma();
        std::wstring saveError;
        SaveAutoRustSetting(false, saveError);
        DiscardSavedAutoState();
        if (g_autoRustCheckbox != nullptr) {
            SendMessageW(g_autoRustCheckbox, BM_SETCHECK, BST_UNCHECKED, 0);
        }
        ReportError(L"Automatic Rust gamma was disabled: " + error);
        UpdateSelectedDisplayUi();
        return;
    }
    if (displayIndex == g_selectedDisplay) {
        UpdateSelectedDisplayUi();
    }
}

void SetAutoRustControlState() {
    if (g_autoRustCheckbox != nullptr) {
        SendMessageW(
            g_autoRustCheckbox,
            BM_SETCHECK,
            g_autoRustEnabled ? BST_CHECKED : BST_UNCHECKED,
            0);
    }
}

void SetAutoRustEnabled(const bool enabled, const bool restoreManualGamma) {
    if (g_autoRustEnabled == enabled) {
        SetAutoRustControlState();
        return;
    }

    g_autoRustEnabled = enabled;
    g_autoGammaDetector.Reset();
    if (!enabled) {
        g_rustMapKeyHeld = false;
    }
    std::wstring error;
    if (!SaveAutoRustSetting(enabled, error)) {
        ReportError(error);
    }
    SetAutoRustControlState();

    if (enabled) {
        if (!StartAutoGamma()) {
            g_autoRustEnabled = false;
            std::wstring timerError;
            SaveAutoRustSetting(false, timerError);
            SetAutoRustControlState();
            ReportError(L"Windows could not start automatic Rust gamma.");
        }
    } else {
        StopAutoGamma();
        KillTimer(g_mainWindow, kMapReleaseDelayTimerId);
        if (restoreManualGamma) {
            RestoreAutoControlledDisplay();
        } else {
            DiscardSavedAutoState();
        }
    }
    UpdateSelectedDisplayUi();
}

void DisableAutoGammaForManualInput() {
    if (g_autoRustEnabled) {
        SetAutoRustEnabled(false, false);
    }
}

double SliderGamma() {
    const LRESULT position = SendMessageW(g_gammaSlider, TBM_GETPOS, 0, 0);
    return gamma_changer::ClampGamma(static_cast<double>(position) / 100.0);
}

void ApplySliderValue() {
    DisableAutoGammaForManualInput();
    if (g_selectedDisplay < 0 ||
        g_selectedDisplay >= static_cast<int>(g_displays.size())) {
        return;
    }
    DisplayState& display = g_displays[static_cast<std::size_t>(g_selectedDisplay)];
    const double gamma = SliderGamma();
    const bool enabled = !IsNeutralGamma(gamma);
    std::wstring error;
    if (!ApplyGammaToDisplay(display, gamma, enabled, error)) {
        UpdateSelectedDisplayUi();
        return;
    }
    SetWindowTextW(g_gammaValue, FormatGamma(display.gamma).c_str());
    SaveSelectedDevice(display.deviceName);
}

void CancelPendingSliderApply() {
    KillTimer(g_mainWindow, kApplyTimerId);
    g_sliderApplyPending = false;
}

void FlushPendingSliderApply() {
    if (!g_sliderApplyPending) {
        return;
    }
    CancelPendingSliderApply();
    ApplySliderValue();
}

void ToggleGamma() {
    DisableAutoGammaForManualInput();
    CancelPendingSliderApply();
    if (g_selectedDisplay < 0 ||
        g_selectedDisplay >= static_cast<int>(g_displays.size())) {
        ReportError(L"No display is selected for the gamma hotkey.");
        return;
    }

    DisplayState& display = g_displays[static_cast<std::size_t>(g_selectedDisplay)];
    const double current = display.enabled ? display.gamma : 1.0;
    const double gamma = gamma_changer::NextToggleGamma(
        current, g_toggleFirstGamma, g_toggleSecondGamma);
    std::wstring error;
    ApplyGammaToDisplay(display, gamma, !IsNeutralGamma(gamma), error);
    UpdateSelectedDisplayUi();
}

void ResetAllDisplays() {
    CancelPendingSliderApply();
    StopAutoGamma();
    KillTimer(g_mainWindow, kMapReleaseDelayTimerId);
    DiscardSavedAutoState();
    for (auto& display : g_displays) {
        display.autoControlled = false;
    }
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
        ReportError(firstError);
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
    data.hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(g_mainWindow, GCLP_HICONSM));
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
    AppendMenuW(menu, MF_STRING, kMenuResetAllAndExit, L"Exit and reset gamma to 1.00");

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

void SaveToggleValue(HWND control) {
    double& saved = control == g_toggleFirst ? g_toggleFirstGamma : g_toggleSecondGamma;
    wchar_t text[64]{};
    GetWindowTextW(control, text, ARRAYSIZE(text));
    double value = 0.0;
    if (!gamma_changer::ParseToggleGamma(text, value)) {
        ReportError(L"Enter a gamma value between 0.50 and 3.00.");
    } else if (value != saved) {
        auto key = OpenRegistryKey(kAppRegistryPath, KEY_SET_VALUE, true);
        const DWORD milli = static_cast<DWORD>(std::lround(value * 1000.0));
        if (!key || RegSetValueExW(key.value,
                control == g_toggleFirst ? L"ToggleFirstGammaMilli" : L"ToggleSecondGammaMilli",
                0, REG_DWORD, reinterpret_cast<const BYTE*>(&milli), sizeof(milli)) != ERROR_SUCCESS) {
            ReportError(L"Could not save the hotkey gamma values.");
        } else {
            saved = value;
        }
    }
    SetWindowTextW(control, FormatGamma(saved).c_str());
}

void CreateMainControls() {
    g_displayLabel = CreateControl(L"STATIC", L"Display", SS_NOPREFIX, kControlDisplayLabel);
    g_displayCombo = CreateControl(WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        kControlDisplayCombo, WS_EX_CLIENTEDGE);
    g_gammaLabel = CreateControl(L"STATIC", L"Gamma", SS_NOPREFIX, kControlGammaLabel);
    g_gammaValue = CreateControl(L"STATIC", L"1.00", SS_RIGHT, kControlGammaValue);
    g_gammaSlider = CreateControl(TRACKBAR_CLASSW, L"",
        TBS_HORZ | TBS_NOTICKS | TBS_DOWNISLEFT | WS_TABSTOP, kControlGammaSlider);
    SendMessageW(g_gammaSlider, TBM_SETRANGE, TRUE, MAKELPARAM(50, 300));
    SendMessageW(g_gammaSlider, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(g_gammaSlider, TBM_SETLINESIZE, 0, 1);
    SendMessageW(g_gammaSlider, TBM_SETPOS, TRUE, 100);
    g_hotkeyLabel = CreateControl(L"STATIC", L"Hotkey", SS_CENTERIMAGE, kControlHotkeyLabel);
    g_hotkeyControl = CreateControl(L"EDIT", L"Not set",
        ES_READONLY | ES_CENTER | ES_AUTOHSCROLL | WS_TABSTOP,
        kControlHotkey, WS_EX_CLIENTEDGE);
    SetWindowSubclass(g_hotkeyControl, SingleKeyControlProc, 1U, 0U);
    g_clearHotkey = CreateControl(L"BUTTON", L"Clear", BS_PUSHBUTTON | WS_TABSTOP, kControlClearHotkey);
    g_toggleLabel = CreateControl(L"STATIC", L"Toggle values", SS_CENTERIMAGE, kControlToggleLabel);
    g_toggleFirst = CreateControl(L"EDIT", FormatGamma(g_toggleFirstGamma).c_str(),
        ES_CENTER | ES_AUTOHSCROLL | WS_TABSTOP, kControlToggleFirst, WS_EX_CLIENTEDGE);
    g_toggleSecond = CreateControl(L"EDIT", FormatGamma(g_toggleSecondGamma).c_str(),
        ES_CENTER | ES_AUTOHSCROLL | WS_TABSTOP, kControlToggleSecond, WS_EX_CLIENTEDGE);
    g_autoRustCheckbox = CreateControl(
        L"BUTTON",
        L"Auto-brighten Rust when the game gets dark",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        kControlAutoRust);
    SendMessageW(g_toggleFirst, EM_SETLIMITTEXT, 16, 0);
    SendMessageW(g_toggleSecond, EM_SETLIMITTEXT, 16, 0);
    SetAutoRustControlState();
    RecreateFonts();
    LayoutControls();
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
                ReportError(hotkeyError);
            }
            if (g_autoRustEnabled) {
                StartAutoGamma();
            }
            return 0;
        }

        case WM_SIZE:
            LayoutControls();
            InvalidateRect(window, nullptr, FALSE);
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
            minMax->ptMinTrackSize.x = ScaleForDpi(window, 400);
            minMax->ptMinTrackSize.y = ScaleForDpi(window, 350);
            return 0;
        }

        case WM_ERASEBKGND:
            // WM_PAINT fills the whole client area; erasing first only flickers.
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client, g_backgroundBrush);
            EndPaint(window, &paint);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(28, 33, 41));
            return reinterpret_cast<LRESULT>(g_backgroundBrush);
        }

        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == g_gammaSlider && !g_updatingControls) {
                DisableAutoGammaForManualInput();
                const int scrollCode = LOWORD(wParam);
                if (scrollCode == TB_ENDTRACK) {
                    // A release can follow THUMBPOSITION or a completed debounce.
                    // Only apply if there is still an outstanding change.
                    FlushPendingSliderApply();
                    return 0;
                }
                const double gamma = SliderGamma();
                SetWindowTextW(g_gammaValue, FormatGamma(gamma).c_str());
                CancelPendingSliderApply();
                if (g_selectedDisplay >= 0 &&
                    g_selectedDisplay < static_cast<int>(g_displays.size())) {
                    const DisplayState& display =
                        g_displays[static_cast<std::size_t>(g_selectedDisplay)];
                    const double appliedGamma = display.enabled ? display.gamma : 1.0;
                    if (display.lastError.empty() && gamma == appliedGamma) {
                        return 0;
                    }
                }
                g_sliderApplyPending = true;
                if (scrollCode == TB_THUMBPOSITION ||
                    SetTimer(window, kApplyTimerId, 90U, nullptr) == 0U) {
                    FlushPendingSliderApply();
                }
            }
            return 0;

        case WM_TIMER:
            if (wParam == kApplyTimerId) {
                FlushPendingSliderApply();
                return 0;
            }
            if (wParam == kDisplayRefreshTimerId) {
                KillTimer(window, kDisplayRefreshTimerId);
                FlushPendingSliderApply();
                RefreshDisplays(true);
                PopulateDisplayCombo();
                UpdateSelectedDisplayUi();
                return 0;
            }
            if (wParam == kAutoGammaTimerId) {
                UpdateAutomaticGamma();
                return 0;
            }
            if (wParam == kMapReleaseDelayTimerId) {
                KillTimer(window, kMapReleaseDelayTimerId);
                UpdateAutomaticGamma();
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
                        FlushPendingSliderApply();
                        g_selectedDisplay = static_cast<int>(data);
                        SaveSelectedDevice(
                            g_displays[static_cast<std::size_t>(g_selectedDisplay)].deviceName);
                        UpdateSelectedDisplayUi();
                    }
                }
                return 0;
            }
            if (identifier == kControlClearHotkey && notification == BN_CLICKED) {
                ClearSavedHotkey();
                return 0;
            }
            if (identifier == kControlAutoRust && notification == BN_CLICKED) {
                CancelPendingSliderApply();
                const bool enabled =
                    SendMessageW(g_autoRustCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                SetAutoRustEnabled(enabled, true);
                return 0;
            }
            if ((identifier == kControlToggleFirst || identifier == kControlToggleSecond) &&
                notification == EN_KILLFOCUS) {
                SaveToggleValue(reinterpret_cast<HWND>(lParam));
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
            if (GetFocus() == g_toggleFirst || GetFocus() == g_toggleSecond) {
                SaveToggleValue(GetFocus());
            }
            ToggleGamma();
            return 0;

        case kKeyCapturedMessage:
            SaveHotkeyFromControl();
            return 0;

        case kMapKeyStateMessage:
            if (g_autoRustEnabled) {
                KillTimer(window, kMapReleaseDelayTimerId);
                if (wParam == 0U) {
                    if (SetTimer(
                            window,
                            kMapReleaseDelayTimerId,
                            kMapReleaseDelayMilliseconds,
                            nullptr) == 0U) {
                        UpdateAutomaticGamma();
                    }
                } else {
                    UpdateAutomaticGamma();
                }
            }
            return 0;

        case kForegroundChangedMessage:
            if (g_autoRustEnabled) {
                UpdateAutomaticGamma();
            }
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
            SaveToggleValue(g_toggleFirst);
            SaveToggleValue(g_toggleSecond);
            CancelPendingSliderApply();
            ResetAllDisplays();
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            CancelPendingSliderApply();
            KillTimer(window, kDisplayRefreshTimerId);
            StopAutoGamma();
            KillTimer(window, kMapReleaseDelayTimerId);
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

bool RegisterWindowClasses(const HICON icon, const HICON smallIcon) {
    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.style = CS_HREDRAW | CS_VREDRAW;
    mainClass.lpfnWndProc = MainWindowProc;
    mainClass.hInstance = g_instance;
    mainClass.hIcon = icon;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hbrBackground = g_backgroundBrush;
    mainClass.lpszClassName = kWindowClassName;
    mainClass.hIconSm = smallIcon;
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

    ScopedHandle instanceMutex(
        CreateMutexW(nullptr, FALSE, kSingleInstanceName));
    if (!instanceMutex) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!startInBackground) {
            const HWND existing = FindWindowW(kWindowClassName, nullptr);
            if (existing != nullptr) {
                PostMessageW(existing, kShowWindowMessage, 0, 0);
            }
        }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&commonControls);

    g_backgroundBrush = CreateSolidBrush(RGB(247, 249, 252));
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    const ScopedIcon icon(instance, SM_CXICON, SM_CYICON);
    const ScopedIcon smallIcon(instance, SM_CXSMICON, SM_CYSMICON);
    if (icon.value == nullptr || smallIcon.value == nullptr ||
        !RegisterWindowClasses(icon.value, smallIcon.value)) {
        DeleteObject(g_backgroundBrush);
        return 1;
    }

    const int width = ScaleForDpi(nullptr, 460);
    const int height = ScaleForDpi(nullptr, 350);
    HWND window = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
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
        return 1;
    }

    if (!startInBackground) {
        ShowWindow(window, showCommand);
        UpdateWindow(window);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN &&
            (GetFocus() == g_toggleFirst || GetFocus() == g_toggleSecond)) {
            SetFocus(g_gammaSlider);
            continue;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (g_valueFont != nullptr) {
        DeleteObject(g_valueFont);
    }
    if (g_bodyFont != nullptr) {
        DeleteObject(g_bodyFont);
    }
    DeleteObject(g_backgroundBrush);
    return static_cast<int>(message.wParam);
}
