# Gamma Changer

Gamma Changer is a small native Windows utility that adjusts the display adapter's
hardware gamma lookup table. It uses the same 256-entry RGB gamma-ramp path used by
the gamma step in Windows Display Color Calibration (`dccw.exe`); it does not use a
transparent overlay, shader, or fake brightness filter.

The app keeps Windows' current calibration ramp as its `1.00` baseline and composes
the selected correction on top of it. This preserves existing per-channel color
calibration instead of replacing it with a generic identity ramp.

## Features

- Live gamma adjustment from `0.50` to `3.00`
- Configurable dedicated key, including Right Shift, that toggles the selected display between two saved gamma values, regardless of held modifiers
- Optional Rust auto mode that samples median game brightness and switches between neutral and the configured night gamma
- Separate saved setting for each active Windows display device
- Read-back verification to detect drivers that silently ignore gamma changes
- Notification-area mode that reapplies settings after display changes, resume, and unlock
- Per-monitor-DPI-aware native Win32 interface

## Build

The fastest option is Visual Studio 2022 Build Tools with the **Desktop development
with C++** workload:

1. Open **x64 Native Tools Command Prompt for VS 2022**.
2. Change to this project directory.
3. Run `build.bat`.
4. Start `build\GammaChanger.exe`.

Or use CMake:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

No installer or third-party runtime is required. Settings are stored for the current
user under `HKCU\Software\GammaChanger`.

Prebuilt x64 executables are published on the repository's **Releases** page. The
matching SHA-256 checksum is provided with each release.

## Use

1. Select a display.
2. Move the slider. The hardware ramp changes after a short debounce so dragging stays responsive.
3. Click the **Hotkey** box and press one key (for example **Right Shift**). It saves immediately. Left and right modifier keys are separate choices; key combinations are not supported. Use **Clear** to disable the hotkey.
4. Enter your two **Toggle values** (`0.50`–`3.00`, in steps of `0.01`). Press Enter, Tab, or click elsewhere to save each value. These values are shared across displays and remembered after restarting.
5. Press the hotkey to switch the selected display between those values. From another slider value, the first press selects the second value. The key works even while Shift, Ctrl, or Alt is held; its normal action is suppressed while the app runs.
6. Tick **Auto-brighten Rust when the game gets dark** to enable the automatic mode. It checks the center of the foreground Rust window twice per second and switches between `1.00` in daylight and the brighter of your two toggle values at night. It measures median scene brightness, smooths several samples, and requires the scene to stay dark or bright before switching, so flashes, HUD elements, and brief lights do not cause rapid changes. When Rust is not in front, gamma returns to `1.00`.

Holding the default Rust map key (`G`) temporarily returns gamma to `1.00`; releasing it waits 0.1 seconds before restoring the night value to match the map fade. Change the map binding in Rust or request a configurable map key if you do not use `G`.

Set gamma to `1.00` to restore the captured Windows calibration. Gamma or settings errors appear as notification-area alerts.

For a moderate automatic result, use toggle values such as `1.00` and `2.20`. The factory default second value is `3.00`, which is the brightest possible night endpoint. Moving the gamma slider or pressing the gamma hotkey while auto mode is active turns auto mode off so manual input is not overwritten.

The title-bar **X closes Gamma Changer completely** and resets every display to `1.00`.
The notification-area menu uses the same reset-and-exit behavior.

## Important Windows limitations

This is the correct legacy Windows calibration mechanism for ordinary SDR desktops,
but the operating system and display driver control the final hardware state:

- Turn **HDR off** for the selected display. Microsoft documents GDI gamma-ramp behavior as undefined in HDR mode.
- A GPU driver can expose one shared ramp for multiple outputs, even when Windows lists separate monitors.
- Windows, a game, another calibration loader, or a graphics driver can overwrite the ramp. Keeping Gamma Changer running lets it reapply after common reset events.
- Rust auto mode relies on foreground-window screen sampling. It works best in borderless or windowed mode; exclusive fullscreen or capture-protected surfaces can prevent Windows from returning readable pixels, in which case the app leaves gamma unchanged.
- Rust auto mode detects the rendered scene instead of a server clock, so it works with custom server day/night lengths. Bright UI or a dark indoor scene can affect the measured brightness.
- Facepunch has described gamma boosting in Rust as an unfair advantage and works with Easy Anti-Cheat on detection. No third-party gamma utility can guarantee account safety; use automatic mode only on servers where it is permitted.
- Some drivers report success but ignore the request. Gamma Changer reads the ramp back and reports that case.
- Full colorimetric calibration (measured primaries, white point, HDR, and persistent ICC/MHC profiles) is broader than a gamma adjustment. Use Windows Display Color Calibration or a hardware colorimeter for that job.

[Microsoft's `SetDeviceGammaRamp` documentation](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-setdevicegammaramp)
recommends ICC profiles for professional, persistent color calibration. The
[Windows hardware calibration documentation](https://learn.microsoft.com/en-us/windows/win32/wcs/display-calibration-mhc)
also describes the newer MHC pipeline used by capable HDR/Advanced Color systems.
Gamma Changer intentionally limits itself to the interactive SDR gamma-ramp behavior
requested here.

## Tests

Run the ramp-math, automatic-gamma, configurable-toggle, and single-key regression tests with CMake on Windows or Linux:

```bash
cmake -S . -B build-local/tests -DBUILD_TESTING=ON
cmake --build build-local/tests --config Release
ctest --test-dir build-local/tests -C Release --output-on-failure
```

The Windows run also checks left/right modifier resolution using the Windows API.
To compile just the platform-independent ramp math directly:

On any C++17 system:

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Isrc \
  src/gamma_math.cpp tests/gamma_math_tests.cpp -o gamma_math_tests
./gamma_math_tests
```
