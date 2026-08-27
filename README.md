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
- Configurable dedicated key that toggles the selected display between `3.00` and `1.00`, regardless of held modifiers
- Separate saved setting for each active Windows display device
- Read-back verification to detect drivers that silently ignore gamma changes
- One-click restoration of the captured Windows calibration
- Optional current-user Startup entry; no administrator rights required
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
3. Press one non-modifier key beside **Single-key toggle**, then select **Set key**. Modifiers shown while recording are discarded.
4. Press that key once for maximum gamma (`3.00`) and again to restore the captured Windows baseline (`1.00`). It works even while Shift, Ctrl, or Alt is held. The chosen key is dedicated to Gamma Changer and its normal action is suppressed while the app runs.
5. Use **Reset this display** to restore the ramp that Windows had before Gamma Changer changed it.
6. Enable **Start with Windows** if the correction should survive sign-in and display resets.

The title-bar **X closes Gamma Changer completely** while leaving the current ramp in
place. Its notification-area menu can also exit, or reset every display and exit.

## Important Windows limitations

This is the correct legacy Windows calibration mechanism for ordinary SDR desktops,
but the operating system and display driver control the final hardware state:

- Turn **HDR off** for the selected display. Microsoft documents GDI gamma-ramp behavior as undefined in HDR mode.
- A GPU driver can expose one shared ramp for multiple outputs, even when Windows lists separate monitors.
- Windows, a game, another calibration loader, or a graphics driver can overwrite the ramp. Keeping Gamma Changer running lets it reapply after common reset events.
- Some drivers report success but ignore the request. Gamma Changer reads the ramp back and reports that case.
- Full colorimetric calibration (measured primaries, white point, HDR, and persistent ICC/MHC profiles) is broader than a gamma adjustment. Use Windows Display Color Calibration or a hardware colorimeter for that job.

[Microsoft's `SetDeviceGammaRamp` documentation](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-setdevicegammaramp)
recommends ICC profiles for professional, persistent color calibration. The
[Windows hardware calibration documentation](https://learn.microsoft.com/en-us/windows/win32/wcs/display-calibration-mhc)
also describes the newer MHC pipeline used by capable HDR/Advanced Color systems.
Gamma Changer intentionally limits itself to the interactive SDR gamma-ramp behavior
requested here.

## Test the platform-independent ramp math

On any C++17 system:

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Isrc \
  src/gamma_math.cpp tests/gamma_math_tests.cpp -o gamma_math_tests
./gamma_math_tests
```
