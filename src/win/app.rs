//! The Win32 application: window, tray icon, hotkey hook, per-display gamma
//! state and the automatic Rust day/night mode.
//!
//! All state lives on the UI thread. Window procedures and hooks re-enter each
//! other through Win32 messages, so state is only borrowed for short stretches
//! and never across a call that can send a message back to this thread.

use std::cell::{Cell, RefCell};
use std::sync::OnceLock;
use std::time::{Duration, Instant};

use windows::core::{w, PCWSTR, PWSTR};
use windows::Win32::Foundation::{
    CloseHandle, GetLastError, COLORREF, SetLastError, ERROR_ALREADY_EXISTS, ERROR_INSUFFICIENT_BUFFER,
    ERROR_SUCCESS, HANDLE, HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, WPARAM,
};
use windows::Win32::Graphics::Gdi::*;
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::System::RemoteDesktop::{
    WTSRegisterSessionNotification, WTSUnRegisterSessionNotification, NOTIFY_FOR_THIS_SESSION,
};
use windows::Win32::System::Threading::{
    CreateMutexW, OpenProcess, QueryFullProcessImageNameW, PROCESS_NAME_FORMAT,
    PROCESS_QUERY_LIMITED_INFORMATION,
};
use windows::Win32::UI::Accessibility::{SetWinEventHook, UnhookWinEvent, HWINEVENTHOOK};
use windows::Win32::UI::Controls::*;
use windows::Win32::UI::HiDpi::*;
use windows::Win32::UI::Input::KeyboardAndMouse::*;
use windows::Win32::UI::Shell::*;
use windows::Win32::UI::WindowsAndMessaging::*;

use gamma_changer::auto_gamma::{automatic_gamma_target, AutoGammaDetector};
use gamma_changer::gamma_math::{
    apply_relative_gamma, clamp_gamma, ramps_approximately_equal, GammaRamp, RAMP_ENTRIES,
};
use gamma_changer::gamma_toggle::{next_toggle_gamma, parse_toggle_gamma};
use gamma_changer::single_key::{is_single_key_allowed, SingleKeyAction, SingleKeyInput};

use super::gdi::{format_system_error, read_device_ramp, write_device_ramp};
use super::gpu::{CollectResult, DesktopDuplicationSampler, SubmitResult};
use super::store::{
    self, display_path, load_display_settings, open_app_key_for_read, open_app_key_for_write, Key,
};
use super::{copy_wide, from_wide, wide};

// ---------------------------------------------------------------- constants

const WINDOW_CLASS: PCWSTR = w!("GammaChanger.MainWindow");
const WINDOW_TITLE: &str = "Gamma Changer";
const SINGLE_INSTANCE_NAME: PCWSTR = w!("Local\\GammaChanger.SingleInstance.v1");
const IDI_GAMMA_CHANGER: usize = 101;

const TRAY_ICON_ID: u32 = 1;
const MSG_TRAY: u32 = WM_APP + 1;
const MSG_SHOW_WINDOW: u32 = WM_APP + 2;
const MSG_SINGLE_KEY: u32 = WM_APP + 3;
const MSG_KEY_CAPTURED: u32 = WM_APP + 4;
const MSG_MAP_KEY_STATE: u32 = WM_APP + 5;
const MSG_FOREGROUND_CHANGED: u32 = WM_APP + 6;

const TIMER_APPLY: usize = 1;
const TIMER_DISPLAY_REFRESH: usize = 2;
const TIMER_AUTO_GAMMA: usize = 3;
const TIMER_MAP_RELEASE: usize = 4;
const TIMER_AUTO_COLLECT: usize = 5;
const TIMER_RAMP_WATCHDOG: usize = 6;

const AUTO_INTERVAL_MS: u32 = 500;
const AUTO_IDLE_INTERVAL_MS: u32 = 2000;
const MAP_RELEASE_DELAY_MS: u32 = 100;
const AUTO_COLLECT_INTERVAL_MS: u32 = 12;
const RAMP_WATCHDOG_INTERVAL_MS: u32 = 250;
/// Rust has to stay out of front this long before auto mode lets go; a toast or
/// overlay taking focus for a moment must not flash neutral gamma.
const FOREGROUND_GRACE: Duration = Duration::from_millis(1500);
/// Consecutive failed samples before auto mode gives up and returns to neutral.
const AUTO_FAILURE_TOLERANCE: u32 = 6;

const RUST_EXECUTABLE: &str = "RustClient.exe";
const RUST_MAP_VIRTUAL_KEY: u32 = b'G' as u32;

const CONTROL_DISPLAY_LABEL: i32 = 1003;
const CONTROL_DISPLAY_COMBO: i32 = 1004;
const CONTROL_GAMMA_LABEL: i32 = 1005;
const CONTROL_GAMMA_VALUE: i32 = 1006;
const CONTROL_GAMMA_SLIDER: i32 = 1007;
const CONTROL_HOTKEY_LABEL: i32 = 1015;
const CONTROL_HOTKEY: i32 = 1016;
const CONTROL_CLEAR_HOTKEY: i32 = 1018;
const CONTROL_TOGGLE_LABEL: i32 = 1019;
const CONTROL_TOGGLE_FIRST: i32 = 1020;
const CONTROL_TOGGLE_SECOND: i32 = 1021;
const CONTROL_AUTO_RUST: i32 = 1022;

// Window styles and messages the bindings do not expose as constants.
const SS_RIGHT: u32 = 0x0002;
const SS_NOPREFIX: u32 = 0x0080;
const SS_CENTERIMAGE: u32 = 0x0200;
const TBM_GETPOS: u32 = WM_USER;

const MENU_OPEN: usize = 2001;
const MENU_RESET_ALL_AND_EXIT: usize = 2002;

// ------------------------------------------------------------------- types

#[derive(Clone, Default)]
struct DisplayState {
    device_name: String,
    label: String,
    primary: bool,
    width: i32,
    height: i32,

    gamma: f64,
    enabled: bool,
    baseline: GammaRamp,
    last_applied: GammaRamp,
    has_baseline: bool,
    has_last_applied: bool,

    ramp_readable: bool,
    automatic_reapply_paused: bool,
    auto_controlled: bool,
    // The baseline ramp rarely changes; only rewrite it to the registry when it does.
    baseline_dirty: bool,
    last_error: String,
}

impl DisplayState {
    fn new(device_name: String) -> Self {
        Self { device_name, gamma: 1.0, ..Default::default() }
    }

    /// The gamma the display is currently showing.
    fn effective_gamma(&self) -> f64 {
        if self.enabled {
            self.gamma
        } else {
            1.0
        }
    }
}

struct SavedAutoState {
    device_name: String,
    gamma: f64,
    enabled: bool,
}

#[derive(PartialEq, Eq)]
enum SampleStatus {
    Ready,   // luminance holds a fresh measurement
    Pending, // a sample is in flight on the GPU; collect it shortly
    Failed,  // capture is unavailable or the frame was black
}

struct State {
    displays: Vec<DisplayState>,
    selected: i32,
    toggle_first: f64,
    toggle_second: f64,
    updating_controls: bool,
    tray_added: bool,
    slider_apply_pending: bool,
    ui_dirty: bool,

    auto_rust: bool,
    auto_timer_interval: u32,
    saved_auto: Option<SavedAutoState>,
    detector: AutoGammaDetector,
    foreground_hook: Option<HWINEVENTHOOK>,
    consecutive_failures: u32,
    not_foreground_since: Option<Instant>,
    // (process id, window handle, is Rust). Keyed on both so a recycled
    // process id can never inherit another process's answer.
    foreground_cache: Option<(u32, isize, bool)>,
    gpu: DesktopDuplicationSampler,
    gpu_luminance: Option<f64>,
    reported_unavailable: bool,
}

#[derive(Clone, Copy, Default)]
struct Handles {
    main: HWND,
    display_label: HWND,
    display_combo: HWND,
    gamma_label: HWND,
    gamma_value: HWND,
    gamma_slider: HWND,
    hotkey_label: HWND,
    hotkey_control: HWND,
    clear_hotkey: HWND,
    toggle_label: HWND,
    toggle_first: HWND,
    toggle_second: HWND,
    auto_rust: HWND,
    body_font: HFONT,
    value_font: HFONT,
    background: HBRUSH,
    instance: HINSTANCE,
    taskbar_created: u32,
}

struct HookState {
    input: SingleKeyInput,
    pending_key: u32,
    map_held: bool,
    hook: Option<HHOOK>,
}

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State::new());
    static HOOK: RefCell<HookState> = RefCell::new(HookState {
        input: SingleKeyInput::new(),
        pending_key: 0,
        map_held: false,
        hook: None,
    });
    static UI: Cell<Handles> = Cell::new(Handles::default());
    static LAST_BALLOON: RefCell<(String, Option<Instant>)> = RefCell::new((String::new(), None));
}

fn with_state<R>(f: impl FnOnce(&mut State) -> R) -> R {
    STATE.with(|state| f(&mut state.borrow_mut()))
}

fn with_hook<R>(f: impl FnOnce(&mut HookState) -> R) -> R {
    HOOK.with(|hook| f(&mut hook.borrow_mut()))
}

fn ui() -> Handles {
    UI.with(Cell::get)
}

fn update_ui(f: impl FnOnce(&mut Handles)) {
    UI.with(|cell| {
        let mut handles = cell.get();
        f(&mut handles);
        cell.set(handles);
    });
}

/// Runs any selected-display UI refresh queued by a state change.
fn flush_ui() {
    if with_state(|s| std::mem::take(&mut s.ui_dirty)) {
        update_selected_display_ui();
    }
}

// ------------------------------------------------------------------ helpers

fn format_gamma(gamma: f64) -> String {
    let hundredths = (gamma * 100.0).round() as i64;
    let sign = if hundredths < 0 { "-" } else { "" };
    let magnitude = hundredths.abs();
    format!("{sign}{}.{:02}", magnitude / 100, magnitude % 100)
}

fn is_neutral(gamma: f64) -> bool {
    (gamma - 1.0).abs() < 0.0001
}

fn set_text(window: HWND, text: &str) {
    let text = wide(text);
    unsafe {
        let _ = SetWindowTextW(window, PCWSTR(text.as_ptr()));
    }
}

fn get_text(window: HWND) -> String {
    let mut buffer = [0u16; 64];
    let length = unsafe { GetWindowTextW(window, &mut buffer) };
    String::from_utf16_lossy(&buffer[..length.max(0) as usize])
}

fn send(window: HWND, message: u32, wparam: usize, lparam: isize) -> LRESULT {
    unsafe { SendMessageW(window, message, WPARAM(wparam), LPARAM(lparam)) }
}

fn loword(value: usize) -> u32 {
    (value & 0xFFFF) as u32
}

fn hiword(value: usize) -> u32 {
    ((value >> 16) & 0xFFFF) as u32
}

fn rgb(r: u8, g: u8, b: u8) -> COLORREF {
    COLORREF(u32::from(r) | u32::from(g) << 8 | u32::from(b) << 16)
}

// ------------------------------------------------------------- notifications

fn notify_data() -> NOTIFYICONDATAW {
    NOTIFYICONDATAW {
        cbSize: std::mem::size_of::<NOTIFYICONDATAW>() as u32,
        hWnd: ui().main,
        uID: TRAY_ICON_ID,
        ..Default::default()
    }
}

fn report_error(message: &str) {
    let mut data = notify_data();
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_ERROR;
    copy_wide(&mut data.szInfoTitle, WINDOW_TITLE);
    copy_wide(&mut data.szInfo, message);
    unsafe {
        let _ = Shell_NotifyIconW(NIM_MODIFY, &data);
    }
}

/// A persistent driver error is re-evaluated on every UI refresh, including each
/// automatic gamma switch; show the same balloon at most once every few seconds.
fn report_display_error(message: &str) {
    let show = LAST_BALLOON.with(|last| {
        let mut last = last.borrow_mut();
        let now = Instant::now();
        if last.0 == message && last.1.is_some_and(|t| now.duration_since(t) < Duration::from_secs(5)) {
            return false;
        }
        *last = (message.to_string(), Some(now));
        true
    });
    if show {
        report_error(message);
    }
}

fn add_tray_icon() {
    let main = ui().main;
    let mut data = notify_data();
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = MSG_TRAY;
    data.hIcon = HICON(unsafe { GetClassLongPtrW(main, GCLP_HICONSM) } as *mut _);
    copy_wide(&mut data.szTip, WINDOW_TITLE);
    let added = unsafe { Shell_NotifyIconW(NIM_ADD, &data) }.as_bool();
    with_state(|s| s.tray_added = added);
}

fn remove_tray_icon() {
    if !with_state(|s| std::mem::take(&mut s.tray_added)) {
        return;
    }
    let data = notify_data();
    unsafe {
        let _ = Shell_NotifyIconW(NIM_DELETE, &data);
    }
}

// ----------------------------------------------------------- settings storage

fn save_display_settings(display: &mut DisplayState) -> Result<(), String> {
    let key = Key::open(
        &display_path(&display.device_name),
        windows::Win32::System::Registry::KEY_SET_VALUE,
        true,
    )
    .ok_or_else(|| format!("Could not save settings for {}.", display.device_name))?;

    key.write_dword("GammaMilli", (display.gamma * 1000.0).round() as u32)?;
    key.write_dword("Enabled", u32::from(display.enabled))?;
    if display.baseline_dirty || !display.has_baseline {
        key.write_string("DeviceName", &display.device_name)?;
    }
    if display.has_baseline && display.baseline_dirty {
        key.write_ramp("BaselineRamp", &display.baseline)?;
        display.baseline_dirty = false;
    }
    if display.has_last_applied {
        key.write_ramp("LastAppliedRamp", &display.last_applied)?;
    }
    Ok(())
}

/// Saves and surfaces a failure instead of silently losing the setting.
fn save_display_settings_reporting(display: &mut DisplayState) {
    if let Err(error) = save_display_settings(display) {
        report_display_error(&format!("Settings could not be saved: {error}"));
    }
}

fn save_selected_device(device_name: &str) {
    let result = open_app_key_for_write().and_then(|key| key.write_string("SelectedDisplay", device_name));
    if let Err(error) = result {
        report_display_error(&format!("Settings could not be saved: {error}"));
    }
}

fn save_app_dword(name: &str, value: u32) -> Result<(), String> {
    open_app_key_for_write()?.write_dword(name, value)
}

fn save_hotkey_setting(virtual_key: u32) -> Result<(), String> {
    let key = open_app_key_for_write().map_err(|_| "Could not save the hotkey for the current user.".to_string())?;
    key.write_dword("HotkeyVirtualKey", virtual_key)
        .map_err(|error| format!("Could not save the single key: {error}"))?;
    // Combination settings are obsolete; only the single virtual key is used.
    key.delete_value("HotkeyFlags");
    Ok(())
}

// --------------------------------------------------------------- gamma apply

fn apply_gamma_to_display(
    display: &mut DisplayState,
    requested: f64,
    enabled: bool,
    persist: bool,
) -> Result<(), String> {
    if !display.has_baseline {
        let message = "No readable baseline gamma ramp is available for this display.".to_string();
        display.last_error = message.clone();
        display.automatic_reapply_paused = true;
        return Err(message);
    }

    let gamma = if enabled { clamp_gamma(requested) } else { 1.0 };
    let target = apply_relative_gamma(&display.baseline, gamma);
    match write_device_ramp(&display.device_name, &target) {
        Err(message) => {
            // Readback can be delayed or overwritten. A second write to the baseline
            // can cause a bright flash followed by an immediate return.
            display.last_error = message.clone();
            display.automatic_reapply_paused = true;
            Err(message)
        }
        Ok(actual) => {
            display.gamma = gamma;
            display.enabled = enabled;
            display.last_applied = actual;
            display.has_last_applied = true;
            display.automatic_reapply_paused = false;
            display.ramp_readable = true;
            display.last_error.clear();
            if persist && !display.auto_controlled {
                save_display_settings_reporting(display);
            }
            Ok(())
        }
    }
}

fn enumerate_displays() -> Vec<DisplayState> {
    let mut displays = Vec::new();
    let mut adapter_index = 0;
    loop {
        let mut adapter = DISPLAY_DEVICEW { cb: std::mem::size_of::<DISPLAY_DEVICEW>() as u32, ..Default::default() };
        if !unsafe { EnumDisplayDevicesW(PCWSTR::null(), adapter_index, &mut adapter, 0) }.as_bool() {
            break;
        }
        adapter_index += 1;
        if adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP == 0
            || adapter.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER != 0
        {
            continue;
        }

        let adapter_name = from_wide(&adapter.DeviceName);
        let mut friendly = String::new();
        let mut monitor_index = 0;
        loop {
            let mut monitor =
                DISPLAY_DEVICEW { cb: std::mem::size_of::<DISPLAY_DEVICEW>() as u32, ..Default::default() };
            if !unsafe {
                EnumDisplayDevicesW(PCWSTR(adapter.DeviceName.as_ptr()), monitor_index, &mut monitor, 0)
            }
            .as_bool()
            {
                break;
            }
            monitor_index += 1;
            let active = monitor.StateFlags & DISPLAY_DEVICE_ACTIVE != 0;
            if friendly.is_empty() || active {
                friendly = from_wide(&monitor.DeviceString);
            }
            if active {
                break;
            }
        }
        if friendly.is_empty() {
            friendly = from_wide(&adapter.DeviceString);
        }
        if friendly.is_empty() {
            friendly = "Display".to_string();
        }

        let mut mode = DEVMODEW { dmSize: std::mem::size_of::<DEVMODEW>() as u16, ..Default::default() };
        let has_mode = unsafe {
            EnumDisplaySettingsExW(
                PCWSTR(adapter.DeviceName.as_ptr()),
                ENUM_CURRENT_SETTINGS,
                &mut mode,
                ENUM_DISPLAY_SETTINGS_FLAGS(0),
            )
        }
        .as_bool();

        let mut display = DisplayState::new(adapter_name);
        display.primary = adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE != 0;
        if has_mode {
            display.width = mode.dmPelsWidth as i32;
            display.height = mode.dmPelsHeight as i32;
        }

        let mut label = friendly;
        if display.width > 0 && display.height > 0 {
            label += &format!("  ·  {} × {}", display.width, display.height);
        }
        if display.primary {
            label += "  ·  Primary";
        }
        label += &format!("  ({})", display.device_name);
        display.label = label;
        displays.push(display);
    }
    // Stable: the primary display first, everything else in enumeration order.
    displays.sort_by_key(|display| !display.primary);
    displays
}

// -------------------------------------------------------------- state logic

impl State {
    fn new() -> Self {
        Self {
            displays: Vec::new(),
            selected: -1,
            toggle_first: 1.0,
            toggle_second: 3.0,
            updating_controls: false,
            tray_added: false,
            slider_apply_pending: false,
            ui_dirty: false,
            auto_rust: false,
            auto_timer_interval: 0,
            saved_auto: None,
            detector: AutoGammaDetector::new(),
            foreground_hook: None,
            consecutive_failures: 0,
            not_foreground_since: None,
            foreground_cache: None,
            gpu: DesktopDuplicationSampler::new(),
            gpu_luminance: None,
            reported_unavailable: false,
        }
    }

    fn selected_index(&self) -> Option<usize> {
        usize::try_from(self.selected).ok().filter(|&index| index < self.displays.len())
    }

    fn find_display(&self, device_name: &str) -> Option<usize> {
        self.displays.iter().position(|d| d.device_name.eq_ignore_ascii_case(device_name))
    }

    fn refresh_displays(&mut self, reapply_saved: bool) {
        let selected_name = self
            .selected_index()
            .map(|index| self.displays[index].device_name.clone())
            .or_else(store::read_selected_device)
            .unwrap_or_default();
        let mut refreshed = enumerate_displays();

        for display in &mut refreshed {
            let previous = self.displays.iter().find(|d| d.device_name == display.device_name);
            let current = read_device_ramp(&display.device_name);
            display.ramp_readable = current.is_ok();

            if let Some(previous) = previous {
                display.gamma = previous.gamma;
                display.enabled = previous.enabled;
                display.baseline = previous.baseline;
                display.last_applied = previous.last_applied;
                display.has_baseline = previous.has_baseline;
                display.baseline_dirty = previous.baseline_dirty;
                display.has_last_applied = previous.has_last_applied;
                display.last_error = previous.last_error.clone();
                display.automatic_reapply_paused = previous.automatic_reapply_paused;
                display.auto_controlled = previous.auto_controlled;
            } else {
                let saved = load_display_settings(&display.device_name);
                display.gamma = saved.gamma;
                display.enabled = saved.enabled;
                display.baseline = saved.baseline;
                display.last_applied = saved.last_applied;
                display.has_baseline = saved.has_baseline;
                display.has_last_applied = saved.has_last_applied;

                if let Ok(current) = &current {
                    // If the current ramp is ours (for example after a crash/relaunch), retain
                    // the saved Windows baseline. Otherwise treat Windows' current state as a
                    // new baseline, which also respects later ICC/profile changes.
                    let current_is_ours = display.has_baseline
                        && display.has_last_applied
                        && ramps_approximately_equal(current, &display.last_applied, 1024);
                    if !current_is_ours {
                        display.baseline = *current;
                        display.has_baseline = true;
                        display.baseline_dirty = true;
                        save_display_settings_reporting(display);
                    }
                }
            }

            match &current {
                Ok(current) if !display.has_baseline => {
                    display.baseline = *current;
                    display.has_baseline = true;
                    display.baseline_dirty = true;
                    save_display_settings_reporting(display);
                }
                _ => {}
            }
            match &current {
                Err(read_error) => display.last_error = read_error.clone(),
                Ok(current) if !display.automatic_reapply_paused => {
                    display.last_error.clear();
                    let still_applied = display.has_last_applied
                        && ramps_approximately_equal(current, &display.last_applied, 0);
                    if reapply_saved && display.enabled && !still_applied {
                        let (gamma, enabled) = (display.gamma, display.enabled);
                        let _ = apply_gamma_to_display(display, gamma, enabled, true);
                    }
                }
                Ok(_) => {}
            }
        }

        self.displays = refreshed;
        self.selected = self
            .displays
            .iter()
            .position(|d| d.device_name == selected_name)
            .map_or(-1, |index| index as i32);
        if self.selected < 0 && !self.displays.is_empty() {
            self.selected = 0;
        }
    }

    // Windows, games and drivers reset the hardware ramp on mode changes, Alt-Tab
    // and UAC prompts. Waiting for the delayed full refresh would leave the screen
    // at the wrong gamma for over a second, so cheaply re-check and restore it now.
    fn reapply_if_reset(&mut self) {
        let selected = self.selected_index();
        for index in 0..self.displays.len() {
            let display = &mut self.displays[index];
            if !display.enabled
                || !display.has_last_applied
                || !display.has_baseline
                || !display.ramp_readable
                || display.automatic_reapply_paused
            {
                continue;
            }
            let Ok(current) = read_device_ramp(&display.device_name) else {
                continue;
            };
            if ramps_approximately_equal(&current, &display.last_applied, 0) {
                continue;
            }
            let gamma = display.gamma;
            if apply_gamma_to_display(display, gamma, true, false).is_err() && Some(index) == selected {
                self.ui_dirty = true;
            }
        }
    }

    // ------------------------------------------------------- auto-mode state

    fn auto_controls(&self, device_name: &str) -> bool {
        self.saved_auto.as_ref().is_some_and(|s| s.device_name.eq_ignore_ascii_case(device_name))
    }

    fn auto_controlled_index(&self) -> Option<usize> {
        self.saved_auto.as_ref().and_then(|s| self.find_display(&s.device_name))
    }

    fn discard_saved_auto(&mut self) {
        if let Some(index) = self.auto_controlled_index() {
            self.displays[index].auto_controlled = false;
        }
        self.saved_auto = None;
        self.detector.reset();
    }

    fn restore_auto_controlled(&mut self) {
        let Some(saved) = self.saved_auto.take() else {
            return;
        };
        if let Some(index) = self.find_display(&saved.device_name) {
            self.displays[index].auto_controlled = false;
        }
        self.detector.reset();
        let Some(index) = self.find_display(&saved.device_name) else {
            return;
        };
        let display = &mut self.displays[index];
        if let Err(error) = apply_gamma_to_display(display, saved.gamma, saved.enabled, true) {
            report_error(&format!("Could not restore gamma after automatic Rust mode: {error}"));
        }
        if Some(index) == self.selected_index() {
            self.ui_dirty = true;
        }
    }

    fn neutralize_auto_controlled(&mut self) {
        let Some(index) = self.auto_controlled_index() else {
            return;
        };
        let display = &mut self.displays[index];
        if !display.enabled && is_neutral(display.gamma) {
            return;
        }
        if let Err(error) = apply_gamma_to_display(display, 1.0, false, false) {
            report_error(&format!("Could not return gamma to neutral outside Rust: {error}"));
        }
        if Some(index) == self.selected_index() {
            self.ui_dirty = true;
        }
    }

    fn capture_auto_state(&mut self, index: usize) {
        // Another display was under automatic control: give it back its own
        // settings instead of leaving it at neutral with the user's gamma lost.
        if self.saved_auto.is_some() {
            self.restore_auto_controlled();
        }
        let display = &mut self.displays[index];
        self.saved_auto = Some(SavedAutoState {
            device_name: display.device_name.clone(),
            gamma: display.gamma,
            enabled: display.enabled,
        });
        display.auto_controlled = true;
    }

    fn reset_auto_session(&mut self) {
        self.detector.reset();
        self.consecutive_failures = 0;
        self.not_foreground_since = None;
    }

    fn set_auto_timer_interval(&mut self, interval: u32) {
        if self.auto_timer_interval == interval {
            return;
        }
        if unsafe { SetTimer(ui().main, TIMER_AUTO_GAMMA, interval, None) } != 0 {
            self.auto_timer_interval = interval;
        }
    }

    // Polls quickly only while Rust is in front. The foreground hook reacts to
    // Alt-Tab immediately, so the idle poll is just a safety net.
    fn start_auto_gamma(&mut self) -> bool {
        self.set_auto_timer_interval(AUTO_INTERVAL_MS);
        if self.auto_timer_interval == 0 {
            return false;
        }
        if self.foreground_hook.is_none() {
            let hook = unsafe {
                SetWinEventHook(
                    EVENT_SYSTEM_FOREGROUND,
                    EVENT_SYSTEM_FOREGROUND,
                    None,
                    Some(foreground_event_proc),
                    0,
                    0,
                    WINEVENT_OUTOFCONTEXT,
                )
            };
            if !hook.is_invalid() {
                self.foreground_hook = Some(hook);
            }
        }
        true
    }

    fn reset_gpu_sampler(&mut self) {
        self.gpu.reset();
        self.gpu_luminance = None;
    }

    fn stop_auto_gamma(&mut self) {
        self.reset_gpu_sampler();
        let main = ui().main;
        unsafe {
            let _ = KillTimer(main, TIMER_AUTO_COLLECT);
            let _ = KillTimer(main, TIMER_AUTO_GAMMA);
        }
        self.auto_timer_interval = 0;
        if let Some(hook) = self.foreground_hook.take() {
            unsafe {
                let _ = UnhookWinEvent(hook);
            }
        }
    }

    fn rust_foreground_window(&mut self) -> Option<HWND> {
        let foreground = unsafe { GetForegroundWindow() };
        if foreground.0.is_null()
            || !unsafe { IsWindowVisible(foreground) }.as_bool()
            || unsafe { IsIconic(foreground) }.as_bool()
        {
            return None;
        }
        let mut process_id = 0u32;
        unsafe { GetWindowThreadProcessId(foreground, Some(&mut process_id)) };
        if process_id == 0 {
            return None;
        }

        // This runs on every auto-gamma tick. Resolve the image name only when the
        // foreground window or its process changes.
        let key = (process_id, foreground.0 as isize);
        let is_rust = match self.foreground_cache {
            Some((pid, window, is_rust)) if (pid, window) == key => is_rust,
            _ => {
                let is_rust = process_executable_name(process_id).eq_ignore_ascii_case(RUST_EXECUTABLE);
                self.foreground_cache = Some((key.0, key.1, is_rust));
                is_rust
            }
        };
        is_rust.then_some(foreground)
    }

    /// Never waits on the GPU. A tick submits a sample; the result is collected on
    /// a following short timer (or the next tick) once the GPU has finished it.
    fn sample_window_luminance(&mut self, window: HWND, collect_only: bool) -> (SampleStatus, f64) {
        let mut fresh = false;
        let mut failed = false;
        match self.gpu.collect() {
            CollectResult::Ready => {
                let (width, height, pixels) = self.gpu.image();
                self.gpu_luminance = median_luminance(pixels, width as usize * height as usize);
                fresh = true;
            }
            CollectResult::Failed => {
                self.gpu_luminance = None;
                failed = true;
            }
            CollectResult::None => {}
        }

        let mut unchanged = false;
        if !collect_only && !failed {
            let mut client = RECT::default();
            let mut origin = POINT { x: 0, y: 0 };
            if unsafe { GetClientRect(window, &mut client) }.is_err()
                || !unsafe { ClientToScreen(window, &mut origin) }.as_bool()
            {
                return (SampleStatus::Failed, 0.0);
            }
            let client_width = client.right - client.left;
            let client_height = client.bottom - client.top;
            if client_width < 200 || client_height < 150 {
                return (SampleStatus::Failed, 0.0);
            }

            let (crop_x, crop_y) = (client_width / 8, client_height / 8);
            let region = RECT {
                left: origin.x + crop_x,
                top: origin.y + crop_y,
                right: origin.x + client_width - crop_x,
                bottom: origin.y + client_height - crop_y,
            };
            // The frame is cropped and downscaled on the GPU and only a tiny image
            // comes back. There is no CPU capture path: if the GPU cannot capture,
            // the sample fails and the user is told.
            match self.gpu.submit(&display_device_for_window(window), region) {
                SubmitResult::Submitted | SubmitResult::Busy => {}
                SubmitResult::Unchanged => unchanged = true,
                SubmitResult::Unavailable => {
                    self.gpu_luminance = None;
                    failed = true;
                }
            }
        }

        if failed {
            if !self.reported_unavailable {
                self.reported_unavailable = true;
                report_error(
                    "GPU screen capture is unavailable (rotated or HDR display, exclusive \
                     fullscreen, or a driver limit). Automatic gamma cannot measure brightness.",
                );
            }
            return (SampleStatus::Failed, 0.0);
        }
        self.reported_unavailable = false;

        if fresh {
            // A black frame means protected or exclusive-fullscreen content.
            return match self.gpu_luminance {
                Some(luminance) => (SampleStatus::Ready, luminance),
                None => (SampleStatus::Failed, 0.0),
            };
        }
        if unchanged {
            if let Some(luminance) = self.gpu_luminance {
                return (SampleStatus::Ready, luminance); // static scene: the last measurement holds
            }
        }
        (SampleStatus::Pending, 0.0)
    }

    fn update_automatic_gamma(&mut self, collect_only: bool) {
        if !self.auto_rust {
            return;
        }
        let main = ui().main;

        let rust_window = self.rust_foreground_window();
        self.set_auto_timer_interval(if rust_window.is_some() { AUTO_INTERVAL_MS } else { AUTO_IDLE_INTERVAL_MS });
        let Some(rust_window) = rust_window else {
            // A toast, overlay or launcher popup can take the foreground for a moment.
            // Only treat Rust as gone once that has lasted, otherwise returning to Rust
            // would restart the detector and flash neutral gamma for about a second.
            let since = *self.not_foreground_since.get_or_insert_with(Instant::now);
            if since.elapsed() < FOREGROUND_GRACE {
                self.set_auto_timer_interval(AUTO_INTERVAL_MS);
                return;
            }
            self.reset_gpu_sampler();
            self.reset_auto_session();
            with_hook(|h| h.map_held = false);
            self.neutralize_auto_controlled();
            return;
        };
        self.not_foreground_since = None;

        let mut display_index = self.find_display(&display_device_for_window(rust_window));
        if display_index.is_none() {
            display_index = self.selected_index();
        }
        let Some(display_index) = display_index else {
            return;
        };
        {
            let display = &self.displays[display_index];
            if !display.has_baseline || !display.ramp_readable {
                return;
            }
        }
        if !self.auto_controls(&self.displays[display_index].device_name) {
            self.capture_auto_state(display_index);
        }

        let night_gamma = 1.0f64.max(self.toggle_first.max(self.toggle_second));
        let map_held = unsafe { GetAsyncKeyState(RUST_MAP_VIRTUAL_KEY as i32) } as u16 & 0x8000 != 0;
        with_hook(|h| h.map_held = map_held);

        let mut target = 1.0;
        if !map_held {
            let (status, luminance) = self.sample_window_luminance(rust_window, collect_only);
            match status {
                SampleStatus::Failed => {
                    // One bad sample (black frame, window being moved) must not drop
                    // gamma; only a sustained failure means capture is really lost.
                    self.consecutive_failures += 1;
                    if self.consecutive_failures >= AUTO_FAILURE_TOLERANCE {
                        self.detector.reset();
                        self.neutralize_auto_controlled();
                    }
                    return;
                }
                SampleStatus::Pending => {
                    if self.gpu.has_pending() {
                        unsafe {
                            SetTimer(main, TIMER_AUTO_COLLECT, AUTO_COLLECT_INTERVAL_MS, None);
                        }
                    }
                    return;
                }
                SampleStatus::Ready => self.consecutive_failures = 0,
            }
            let night = self.detector.update(luminance);
            target = automatic_gamma_target(night, night_gamma);
        }

        let current = self.displays[display_index].effective_gamma();
        if (target - current).abs() < 0.005 {
            return;
        }

        let enabled = !is_neutral(target);
        let result = apply_gamma_to_display(&mut self.displays[display_index], target, enabled, false);
        if let Err(error) = result {
            self.auto_rust = false;
            self.stop_auto_gamma();
            if let Err(save_error) = save_app_dword("AutoRustEnabled", 0) {
                report_display_error(&save_error);
            }
            self.discard_saved_auto();
            set_auto_checkbox(false);
            report_error(&format!("Automatic Rust gamma was disabled: {error}"));
            self.ui_dirty = true;
            return;
        }
        if Some(display_index) == self.selected_index() {
            self.ui_dirty = true;
        }
    }

    fn set_auto_rust_enabled(&mut self, enabled: bool, restore_manual: bool) {
        if self.auto_rust == enabled {
            set_auto_checkbox(self.auto_rust);
            return;
        }

        self.auto_rust = enabled;
        self.reset_auto_session();
        if !enabled {
            with_hook(|h| h.map_held = false);
        }
        if let Err(error) = save_app_dword("AutoRustEnabled", u32::from(enabled)) {
            report_error(&format!("Could not save the automatic Rust setting: {error}"));
        }
        set_auto_checkbox(self.auto_rust);

        if enabled {
            if !self.start_auto_gamma() {
                self.auto_rust = false;
                let _ = save_app_dword("AutoRustEnabled", 0);
                set_auto_checkbox(false);
                report_error("Windows could not start automatic Rust gamma.");
            }
        } else {
            self.stop_auto_gamma();
            unsafe {
                let _ = KillTimer(ui().main, TIMER_MAP_RELEASE);
            }
            if restore_manual {
                self.restore_auto_controlled();
            } else {
                self.discard_saved_auto();
            }
        }
        self.ui_dirty = true;
    }

    /// Manual input wins over automatic mode. A display the user is not touching
    /// gets its own settings back; the one being changed is simply handed over.
    fn disable_auto_for_manual_input(&mut self) {
        if !self.auto_rust {
            return;
        }
        let touches_auto_display = self
            .selected_index()
            .is_some_and(|index| self.auto_controls(&self.displays[index].device_name));
        self.set_auto_rust_enabled(false, !touches_auto_display);
    }

    // -------------------------------------------------------- manual changes

    fn cancel_pending_slider_apply(&mut self) {
        unsafe {
            let _ = KillTimer(ui().main, TIMER_APPLY);
        }
        self.slider_apply_pending = false;
    }

    fn apply_slider_value(&mut self) {
        self.disable_auto_for_manual_input();
        let Some(index) = self.selected_index() else {
            return;
        };
        let gamma = slider_gamma();
        let enabled = !is_neutral(gamma);
        let display = &mut self.displays[index];
        if apply_gamma_to_display(display, gamma, enabled, true).is_err() {
            self.ui_dirty = true;
            return;
        }
        set_text(ui().gamma_value, &format_gamma(display.gamma));
        let name = display.device_name.clone();
        save_selected_device(&name);
    }

    fn flush_pending_slider_apply(&mut self) {
        if !self.slider_apply_pending {
            return;
        }
        self.cancel_pending_slider_apply();
        self.apply_slider_value();
    }

    fn toggle_gamma(&mut self) {
        self.disable_auto_for_manual_input();
        self.cancel_pending_slider_apply();
        let Some(index) = self.selected_index() else {
            report_error("No display is selected for the gamma hotkey.");
            return;
        };
        let display = &mut self.displays[index];
        let gamma = next_toggle_gamma(display.effective_gamma(), self.toggle_first, self.toggle_second);
        let _ = apply_gamma_to_display(display, gamma, !is_neutral(gamma), true);
        self.ui_dirty = true;
    }

    fn reset_all_displays(&mut self) {
        let main = ui().main;
        unsafe {
            let _ = KillTimer(main, TIMER_RAMP_WATCHDOG);
            let _ = KillTimer(main, TIMER_MAP_RELEASE);
        }
        self.cancel_pending_slider_apply();
        self.stop_auto_gamma();
        self.discard_saved_auto();
        for display in &mut self.displays {
            display.auto_controlled = false;
        }
        let mut first_error = String::new();
        for display in &mut self.displays {
            if !display.has_baseline {
                continue;
            }
            if let Err(error) = apply_gamma_to_display(display, 1.0, false, true) {
                if first_error.is_empty() {
                    first_error = format!("{}: {error}", display.device_name);
                }
            }
        }
        if !first_error.is_empty() {
            report_error(&first_error);
        }
    }
}

// --------------------------------------------------------------- measurement

fn process_executable_name(process_id: u32) -> String {
    let Ok(process) = (unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, process_id) }) else {
        return String::new();
    };
    let mut path = vec![0u16; 260];
    let name = loop {
        let mut length = path.len() as u32;
        let result = unsafe {
            QueryFullProcessImageNameW(process, PROCESS_NAME_FORMAT(0), PWSTR(path.as_mut_ptr()), &mut length)
        };
        match result {
            Ok(()) => break from_wide(&path[..length as usize]),
            Err(error) => {
                let too_small = error.code() == ERROR_INSUFFICIENT_BUFFER.to_hresult();
                if !too_small || path.len() >= 32768 {
                    break String::new();
                }
                path = vec![0u16; path.len() * 2];
            }
        }
    };
    unsafe {
        let _ = CloseHandle(process);
    }
    name.rsplit(['\\', '/']).next().unwrap_or_default().to_string()
}

fn display_device_for_window(window: HWND) -> String {
    let monitor = unsafe { MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) };
    let mut info = MONITORINFOEXW::default();
    info.monitorInfo.cbSize = std::mem::size_of::<MONITORINFOEXW>() as u32;
    if monitor.is_invalid()
        || !unsafe { GetMonitorInfoW(monitor, (&mut info as *mut MONITORINFOEXW).cast()) }.as_bool()
    {
        return String::new();
    }
    from_wide(&info.szDevice)
}

fn linear_srgb_table() -> &'static [f64; 256] {
    static TABLE: OnceLock<[f64; 256]> = OnceLock::new();
    TABLE.get_or_init(|| {
        let mut values = [0.0; 256];
        for (index, value) in values.iter_mut().enumerate() {
            let channel = index as f64 / 255.0;
            *value = if channel <= 0.04045 { channel / 12.92 } else { ((channel + 0.055) / 1.055).powf(2.4) };
        }
        values
    })
}

/// Median scene brightness of a BGRA image. `None` for an all-black image,
/// which is what protected or exclusive-fullscreen surfaces produce.
fn median_luminance(bytes: &[u8], pixel_count: usize) -> Option<f64> {
    const BINS: usize = 1024;
    let mut histogram = [0u32; BINS];
    let mut non_black = 0usize;
    let linear = linear_srgb_table();
    for pixel in bytes.chunks_exact(4).take(pixel_count) {
        let (b, g, r) = (pixel[0], pixel[1], pixel[2]);
        if b != 0 || g != 0 || r != 0 {
            non_black += 1;
        }
        let luminance =
            linear[r as usize] * 0.2126 + linear[g as usize] * 0.7152 + linear[b as usize] * 0.0722;
        histogram[((luminance * BINS as f64) as usize).min(BINS - 1)] += 1;
    }

    // Do not mistake a black capture for an in-game night.
    if pixel_count == 0 || non_black < pixel_count / 100 {
        return None;
    }
    // The median is resistant to HUD elements, muzzle flashes, and small lamps.
    let median_target = pixel_count / 2;
    let mut accumulated = 0usize;
    for (bin, &count) in histogram.iter().enumerate() {
        accumulated += count as usize;
        if accumulated > median_target {
            return Some((bin as f64 + 0.5) / BINS as f64);
        }
    }
    None
}

unsafe extern "system" fn foreground_event_proc(
    _hook: HWINEVENTHOOK,
    _event: u32,
    _window: HWND,
    _object: i32,
    _child: i32,
    _thread: u32,
    _time: u32,
) {
    let main = ui().main;
    if !main.0.is_null() {
        let _ = PostMessageW(main, MSG_FOREGROUND_CHANGED, WPARAM(0), LPARAM(0));
    }
}

// ----------------------------------------------------------------- hotkey

fn single_key_from_hook(keyboard: &KBDLLHOOKSTRUCT) -> u32 {
    match VIRTUAL_KEY(keyboard.vkCode as u16) {
        VK_SHIFT | VK_CONTROL | VK_MENU => {
            let extended = if keyboard.flags.0 & LLKHF_EXTENDED.0 != 0 { 0xE000 } else { 0 };
            unsafe { MapVirtualKeyW(keyboard.scanCode | extended, MAPVK_VSC_TO_VK_EX) }
        }
        _ => keyboard.vkCode,
    }
}

fn single_key_name(key: u32) -> String {
    let fixed = match VIRTUAL_KEY(key as u16) {
        _ if key == 0 => Some("Not set"),
        VK_LSHIFT => Some("Left Shift"),
        VK_RSHIFT => Some("Right Shift"),
        VK_LCONTROL => Some("Left Ctrl"),
        VK_RCONTROL => Some("Right Ctrl"),
        VK_LMENU => Some("Left Alt"),
        VK_RMENU => Some("Right Alt"),
        VK_LWIN => Some("Left Windows"),
        VK_RWIN => Some("Right Windows"),
        _ => None,
    };
    if let Some(name) = fixed {
        return name.to_string();
    }
    let scan = unsafe { MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX) };
    let flags = (((scan & 0xFF) << 16) | if scan & 0xFF00 != 0 { 1 << 24 } else { 0 }) as i32;
    let mut name = [0u16; 128];
    if unsafe { GetKeyNameTextW(flags, &mut name) } > 0 {
        return from_wide(&name);
    }
    format!("Key {key}")
}

unsafe extern "system" fn keyboard_proc(code: i32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    if code == HC_ACTION as i32 {
        let keyboard = &*(lparam.0 as *const KBDLLHOOKSTRUCT);
        let message = wparam.0 as u32;
        let is_down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        let is_up = message == WM_KEYUP || message == WM_SYSKEYUP;
        if is_down || is_up {
            let handles = ui();
            let map_changed = with_hook(|h| {
                let changed = keyboard.vkCode == RUST_MAP_VIRTUAL_KEY && h.map_held != is_down;
                if changed {
                    h.map_held = is_down;
                }
                changed
            });
            if map_changed {
                let _ = PostMessageW(handles.main, MSG_MAP_KEY_STATE, WPARAM(usize::from(is_down)), LPARAM(0));
            }

            let key_box_focused =
                GetForegroundWindow() == handles.main && GetFocus() == handles.hotkey_control;
            let key = single_key_from_hook(keyboard);
            let result = with_hook(|h| {
                if !key_box_focused {
                    h.input.cancel_capture();
                }
                let result = h.input.process(key, is_down, !key_box_focused);
                if result.action == SingleKeyAction::Captured {
                    h.pending_key = key;
                }
                result
            });
            match result.action {
                SingleKeyAction::Captured => {
                    let _ = PostMessageW(handles.main, MSG_KEY_CAPTURED, WPARAM(0), LPARAM(0));
                }
                SingleKeyAction::Toggle => {
                    let _ = PostMessageW(handles.main, MSG_SINGLE_KEY, WPARAM(0), LPARAM(0));
                }
                SingleKeyAction::None => {}
            }
            if result.suppress {
                return LRESULT(1);
            }
        }
    }
    CallNextHookEx(HHOOK::default(), code, wparam, lparam)
}

fn remove_single_key_hook() {
    with_hook(|h| {
        if let Some(hook) = h.hook.take() {
            unsafe {
                let _ = UnhookWindowsHookEx(hook);
            }
        }
        h.input.cancel_capture();
        h.map_held = false;
    });
}

fn install_single_key_hook() -> Result<(), String> {
    if with_hook(|h| h.hook.is_some()) {
        return Ok(());
    }
    unsafe { SetLastError(ERROR_SUCCESS) };
    match unsafe { SetWindowsHookExW(WH_KEYBOARD_LL, Some(keyboard_proc), ui().instance, 0) } {
        Ok(hook) => {
            with_hook(|h| h.hook = Some(hook));
            Ok(())
        }
        Err(error) => Err(format!(
            "Windows could not activate the single-key shortcut: {}",
            format_system_error((error.code().0 & 0xFFFF) as u32)
        )),
    }
}

fn set_hotkey_control_value(virtual_key: u32) {
    with_hook(|h| h.pending_key = virtual_key);
    let control = ui().hotkey_control;
    if !control.0.is_null() {
        set_text(control, &single_key_name(virtual_key));
    }
}

fn begin_single_key_capture(window: HWND) {
    if let Err(error) = install_single_key_hook() {
        report_error(&error);
        return;
    }
    with_hook(|h| h.input.begin_capture());
    set_text(window, "Press one key...");
}

unsafe extern "system" fn single_key_control_proc(
    window: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
    subclass_id: usize,
    _data: usize,
) -> LRESULT {
    match message {
        WM_SETFOCUS => begin_single_key_capture(window),
        WM_LBUTTONDOWN => {
            if GetFocus() == window {
                begin_single_key_capture(window);
            }
        }
        WM_KILLFOCUS => {
            with_hook(|h| h.input.cancel_capture());
            set_hotkey_control_value(with_hook(|h| h.pending_key));
        }
        WM_GETDLGCODE => {
            if with_hook(|h| h.input.is_capturing()) {
                return LRESULT(DLGC_WANTALLKEYS as isize);
            }
        }
        WM_CHAR | WM_SYSCHAR => return LRESULT(0),
        WM_NCDESTROY => {
            with_hook(|h| h.input.cancel_capture());
            let _ = RemoveWindowSubclass(window, Some(single_key_control_proc), subclass_id);
        }
        _ => {}
    }
    DefSubclassProc(window, message, wparam, lparam)
}

fn save_hotkey_from_control() {
    let requested = with_hook(|h| h.pending_key);
    if !is_single_key_allowed(requested) {
        report_error("Click the key box and press one key, such as Right Shift.");
        return;
    }
    if let Err(error) = install_single_key_hook().and_then(|()| save_hotkey_setting(requested)) {
        set_hotkey_control_value(with_hook(|h| h.input.bound_key()));
        report_error(&error);
        return;
    }
    with_hook(|h| {
        h.input.set_binding(requested);
        h.input.cancel_capture();
    });
    set_hotkey_control_value(requested);
    unsafe {
        let _ = SetFocus(ui().gamma_slider);
    }
}

fn clear_saved_hotkey() {
    if let Err(error) = save_hotkey_setting(0) {
        report_error(&error);
        return;
    }
    with_hook(|h| {
        h.input.set_binding(0);
        h.input.cancel_capture();
    });
    set_hotkey_control_value(0);
}

fn load_settings() {
    let Some(key) = open_app_key_for_read() else {
        return;
    };
    with_state(|s| {
        if let Some(milli) = key.read_dword("ToggleFirstGammaMilli") {
            s.toggle_first = clamp_gamma(f64::from(milli) / 1000.0);
        }
        if let Some(milli) = key.read_dword("ToggleSecondGammaMilli") {
            s.toggle_second = clamp_gamma(f64::from(milli) / 1000.0);
        }
        if let Some(auto) = key.read_dword("AutoRustEnabled") {
            s.auto_rust = auto != 0;
        }
    });
    if let Some(virtual_key) = key.read_dword("HotkeyVirtualKey") {
        with_hook(|h| h.input.set_binding(virtual_key));
    }
}

fn save_toggle_value(control: HWND) {
    let handles = ui();
    let first = control == handles.toggle_first;
    let parsed = parse_toggle_gamma(&get_text(control));
    let mut shown = with_state(|s| if first { s.toggle_first } else { s.toggle_second });

    match parsed {
        None => report_error("Enter a gamma value between 0.50 and 3.00."),
        Some(value) if value != shown => {
            let name = if first { "ToggleFirstGammaMilli" } else { "ToggleSecondGammaMilli" };
            match save_app_dword(name, (value * 1000.0).round() as u32) {
                Ok(()) => {
                    shown = value;
                    with_state(|s| {
                        if first {
                            s.toggle_first = value;
                        } else {
                            s.toggle_second = value;
                        }
                    });
                }
                Err(_) => report_error("Could not save the hotkey gamma values."),
            }
        }
        Some(_) => {}
    }
    set_text(control, &format_gamma(shown));
}

// --------------------------------------------------------------------- UI

fn mul_div(value: i32, numerator: i32, denominator: i32) -> i32 {
    ((i64::from(value) * i64::from(numerator) + i64::from(denominator) / 2) / i64::from(denominator)) as i32
}

fn scale_for_dpi(window: HWND, value: i32) -> i32 {
    let dpi = unsafe {
        if window.0.is_null() {
            GetDpiForSystem()
        } else {
            GetDpiForWindow(window)
        }
    };
    mul_div(value, dpi as i32, 96)
}

fn create_ui_font(window: HWND, point_size: i32, weight: i32) -> HFONT {
    let dpi = if window.0.is_null() { 96 } else { unsafe { GetDpiForWindow(window) } };
    unsafe {
        CreateFontW(
            -mul_div(point_size, dpi as i32, 72),
            0,
            0,
            0,
            weight,
            0,
            0,
            0,
            u32::from(DEFAULT_CHARSET.0),
            u32::from(OUT_DEFAULT_PRECIS.0),
            u32::from(CLIP_DEFAULT_PRECIS.0),
            u32::from(CLEARTYPE_QUALITY.0),
            (DEFAULT_PITCH.0 | FF_DONTCARE.0) as u32,
            w!("Segoe UI"),
        )
    }
}

fn recreate_fonts() {
    let handles = ui();
    let body = create_ui_font(handles.main, 10, FW_NORMAL.0 as i32);
    let value = create_ui_font(handles.main, 16, FW_SEMIBOLD.0 as i32);
    for control in [
        handles.display_label,
        handles.display_combo,
        handles.gamma_label,
        handles.hotkey_label,
        handles.hotkey_control,
        handles.clear_hotkey,
        handles.toggle_label,
        handles.toggle_first,
        handles.toggle_second,
        handles.auto_rust,
    ] {
        if !control.0.is_null() {
            send(control, WM_SETFONT, body.0 as usize, 1);
        }
    }
    if !handles.gamma_value.0.is_null() {
        send(handles.gamma_value, WM_SETFONT, value.0 as usize, 1);
    }
    unsafe {
        if !handles.body_font.is_invalid() {
            let _ = DeleteObject(handles.body_font);
        }
        if !handles.value_font.is_invalid() {
            let _ = DeleteObject(handles.value_font);
        }
    }
    update_ui(|h| {
        h.body_font = body;
        h.value_font = value;
    });
}

fn layout_controls() {
    let handles = ui();
    let mut client = RECT::default();
    unsafe {
        let _ = GetClientRect(handles.main, &mut client);
    }
    let scale = |value: i32| scale_for_dpi(handles.main, value);
    let margin = scale(20);
    let width = (client.right - 2 * margin).max(1);
    let place = |control: HWND, x: i32, y: i32, w: i32, h: i32| unsafe {
        let _ = MoveWindow(control, x, scale(y), w, scale(h), true);
    };
    place(handles.display_label, margin, 18, width, 22);
    place(handles.display_combo, margin, 44, width, 220);
    place(handles.gamma_label, margin, 88, width / 2, 26);
    place(handles.gamma_value, margin + width / 2, 82, width / 2, 32);
    place(handles.gamma_slider, margin, 118, width, 36);
    place(handles.hotkey_label, margin, 174, scale(100), 28);
    place(handles.hotkey_control, margin + scale(104), 174, width - scale(176), 28);
    place(handles.clear_hotkey, margin + width - scale(64), 174, scale(64), 28);
    place(handles.toggle_label, margin, 218, scale(100), 28);
    let field_width = (width - scale(116)) / 2;
    place(handles.toggle_first, margin + scale(104), 218, field_width, 28);
    place(handles.toggle_second, margin + scale(116) + field_width, 218, field_width, 28);
    place(handles.auto_rust, margin, 262, width, 28);
}

fn populate_display_combo() {
    let combo = ui().display_combo;
    let (labels, selected) = with_state(|s| {
        s.updating_controls = true;
        (s.displays.iter().map(|d| d.label.clone()).collect::<Vec<_>>(), s.selected)
    });
    send(combo, CB_RESETCONTENT, 0, 0);
    for (index, label) in labels.iter().enumerate() {
        let text = wide(label);
        let item = send(combo, CB_ADDSTRING, 0, text.as_ptr() as isize).0;
        if item != CB_ERR as isize && item != CB_ERRSPACE as isize {
            send(combo, CB_SETITEMDATA, item as usize, index as isize);
        }
    }
    if selected >= 0 {
        send(combo, CB_SETCURSEL, selected as usize, 0);
    }
    with_state(|s| s.updating_controls = false);
}

fn update_selected_display_ui() {
    struct Snapshot {
        gamma: f64,
        paused: bool,
        error: String,
        usable: bool,
    }
    let (snapshot, handles) = with_state(|s| {
        s.updating_controls = true;
        let snapshot = s.selected_index().map(|index| {
            let d = &s.displays[index];
            Snapshot {
                gamma: d.effective_gamma(),
                paused: d.automatic_reapply_paused,
                error: d.last_error.clone(),
                usable: d.ramp_readable && d.has_baseline,
            }
        });
        (snapshot, ui())
    });

    let has_display = snapshot.is_some();
    unsafe {
        let _ = EnableWindow(handles.display_combo, has_display);
        let _ = EnableWindow(handles.gamma_slider, has_display);
    }
    match snapshot {
        None => {
            set_text(handles.gamma_value, "—");
            report_display_error("No active desktop display was found.");
        }
        Some(snapshot) => {
            send(handles.gamma_slider, TBM_SETPOS, 1, (snapshot.gamma * 100.0).round() as isize);
            let shown = if snapshot.paused { "—".to_string() } else { format_gamma(snapshot.gamma) };
            set_text(handles.gamma_value, &shown);
            if !snapshot.error.is_empty() {
                report_display_error(&snapshot.error);
                if !snapshot.usable {
                    unsafe {
                        let _ = EnableWindow(handles.gamma_slider, false);
                    }
                }
            } else if !snapshot.usable {
                report_display_error("This display driver does not provide a usable SDR hardware gamma ramp.");
                unsafe {
                    let _ = EnableWindow(handles.gamma_slider, false);
                }
            }
        }
    }
    with_state(|s| s.updating_controls = false);
}

fn set_auto_checkbox(checked: bool) {
    let checkbox = ui().auto_rust;
    if !checkbox.0.is_null() {
        send(checkbox, BM_SETCHECK, if checked { BST_CHECKED.0 } else { BST_UNCHECKED.0 } as usize, 0);
    }
}

fn slider_gamma() -> f64 {
    let position = send(ui().gamma_slider, TBM_GETPOS, 0, 0).0;
    clamp_gamma(position as f64 / 100.0)
}

fn show_main_window() {
    let main = ui().main;
    unsafe {
        let _ = ShowWindow(main, SW_SHOWNORMAL);
        let _ = SetForegroundWindow(main);
    }
}

fn show_tray_menu() {
    let main = ui().main;
    let Ok(menu) = (unsafe { CreatePopupMenu() }) else {
        return;
    };
    unsafe {
        let _ = AppendMenuW(menu, MF_STRING, MENU_OPEN, w!("Open Gamma Changer"));
        let _ = AppendMenuW(menu, MF_SEPARATOR, 0, PCWSTR::null());
        let _ = AppendMenuW(menu, MF_STRING, MENU_RESET_ALL_AND_EXIT, w!("Exit and reset gamma to 1.00"));
        let _ = SetMenuDefaultItem(menu, MENU_OPEN as u32, 0);

        let mut cursor = POINT::default();
        let _ = GetCursorPos(&mut cursor);
        let _ = SetForegroundWindow(main);
        let command = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
            cursor.x,
            cursor.y,
            0,
            main,
            None,
        );
        let _ = DestroyMenu(menu);
        if command.0 != 0 {
            let _ = PostMessageW(main, WM_COMMAND, WPARAM(command.0 as usize), LPARAM(0));
        }
        // Lets the menu dismiss correctly when the user clicks elsewhere.
        let _ = PostMessageW(main, WM_NULL, WPARAM(0), LPARAM(0));
    }
}

fn create_control(class: PCWSTR, text: &str, style: u32, id: i32, ex_style: u32) -> HWND {
    let handles = ui();
    let text = wide(text);
    unsafe {
        CreateWindowExW(
            WINDOW_EX_STYLE(ex_style),
            class,
            PCWSTR(text.as_ptr()),
            WINDOW_STYLE(WS_CHILD.0 | WS_VISIBLE.0 | style),
            0,
            0,
            1,
            1,
            handles.main,
            HMENU(id as isize as *mut _),
            handles.instance,
            None,
        )
    }
    .unwrap_or_default()
}

fn create_main_controls() {
    let (first, second) = with_state(|s| (s.toggle_first, s.toggle_second));
    let edge = WS_EX_CLIENTEDGE.0;
    let tab = WS_TABSTOP.0;

    let display_label = create_control(w!("STATIC"), "Display", SS_NOPREFIX, CONTROL_DISPLAY_LABEL, 0);
    let display_combo = create_control(
        WC_COMBOBOXW,
        "",
        CBS_DROPDOWNLIST as u32 | CBS_HASSTRINGS as u32 | WS_VSCROLL.0 | tab,
        CONTROL_DISPLAY_COMBO,
        edge,
    );
    let gamma_label = create_control(w!("STATIC"), "Gamma", SS_NOPREFIX, CONTROL_GAMMA_LABEL, 0);
    let gamma_value = create_control(w!("STATIC"), "1.00", SS_RIGHT, CONTROL_GAMMA_VALUE, 0);
    let gamma_slider = create_control(
        TRACKBAR_CLASSW,
        "",
        TBS_HORZ | TBS_NOTICKS | TBS_DOWNISLEFT | tab,
        CONTROL_GAMMA_SLIDER,
        0,
    );
    send(gamma_slider, TBM_SETRANGE, 1, ((300u32 << 16) | 50) as isize);
    send(gamma_slider, TBM_SETPAGESIZE, 0, 10);
    send(gamma_slider, TBM_SETLINESIZE, 0, 1);
    send(gamma_slider, TBM_SETPOS, 1, 100);

    let hotkey_label = create_control(w!("STATIC"), "Hotkey", SS_CENTERIMAGE, CONTROL_HOTKEY_LABEL, 0);
    let hotkey_control = create_control(
        w!("EDIT"),
        "Not set",
        ES_READONLY as u32 | ES_CENTER as u32 | ES_AUTOHSCROLL as u32 | tab,
        CONTROL_HOTKEY,
        edge,
    );
    unsafe {
        let _ = SetWindowSubclass(hotkey_control, Some(single_key_control_proc), 1, 0);
    }
    let clear_hotkey = create_control(w!("BUTTON"), "Clear", BS_PUSHBUTTON as u32 | tab, CONTROL_CLEAR_HOTKEY, 0);
    let toggle_label = create_control(w!("STATIC"), "Toggle values", SS_CENTERIMAGE, CONTROL_TOGGLE_LABEL, 0);
    let toggle_first = create_control(
        w!("EDIT"),
        &format_gamma(first),
        ES_CENTER as u32 | ES_AUTOHSCROLL as u32 | tab,
        CONTROL_TOGGLE_FIRST,
        edge,
    );
    let toggle_second = create_control(
        w!("EDIT"),
        &format_gamma(second),
        ES_CENTER as u32 | ES_AUTOHSCROLL as u32 | tab,
        CONTROL_TOGGLE_SECOND,
        edge,
    );
    let auto_rust = create_control(
        w!("BUTTON"),
        "Auto-brighten Rust when the game gets dark",
        BS_AUTOCHECKBOX as u32 | tab,
        CONTROL_AUTO_RUST,
        0,
    );
    send(toggle_first, EM_SETLIMITTEXT, 16, 0);
    send(toggle_second, EM_SETLIMITTEXT, 16, 0);

    update_ui(|h| {
        h.display_label = display_label;
        h.display_combo = display_combo;
        h.gamma_label = gamma_label;
        h.gamma_value = gamma_value;
        h.gamma_slider = gamma_slider;
        h.hotkey_label = hotkey_label;
        h.hotkey_control = hotkey_control;
        h.clear_hotkey = clear_hotkey;
        h.toggle_label = toggle_label;
        h.toggle_first = toggle_first;
        h.toggle_second = toggle_second;
        h.auto_rust = auto_rust;
    });
    set_auto_checkbox(with_state(|s| s.auto_rust));
    recreate_fonts();
    layout_controls();
}

fn schedule_display_refresh(window: HWND) {
    unsafe {
        let _ = KillTimer(window, TIMER_DISPLAY_REFRESH);
        SetTimer(window, TIMER_DISPLAY_REFRESH, 1200, None);
    }
}

// ------------------------------------------------------------ window procedure

unsafe extern "system" fn main_window_proc(
    window: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    let taskbar_created = ui().taskbar_created;
    if taskbar_created != 0 && message == taskbar_created {
        with_state(|s| s.tray_added = false);
        add_tray_icon();
        return LRESULT(0);
    }

    match message {
        WM_CREATE => {
            update_ui(|h| h.main = window);
            load_settings();
            create_main_controls();
            add_tray_icon();
            let _ = WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
            with_state(|s| s.refresh_displays(true));
            populate_display_combo();
            update_selected_display_ui();
            SetTimer(window, TIMER_RAMP_WATCHDOG, RAMP_WATCHDOG_INTERVAL_MS, None);

            set_hotkey_control_value(with_hook(|h| h.input.bound_key()));
            // The hook also records modifier-only keys when no shortcut is saved yet.
            if let Err(error) = install_single_key_hook() {
                report_error(&error);
            }
            if with_state(|s| s.auto_rust) {
                with_state(|s| s.start_auto_gamma());
            }
            LRESULT(0)
        }

        WM_SIZE => {
            layout_controls();
            let _ = InvalidateRect(window, None, false);
            LRESULT(0)
        }

        WM_DPICHANGED => {
            let suggested = &*(lparam.0 as *const RECT);
            let _ = SetWindowPos(
                window,
                None,
                suggested.left,
                suggested.top,
                suggested.right - suggested.left,
                suggested.bottom - suggested.top,
                SWP_NOZORDER | SWP_NOACTIVATE,
            );
            recreate_fonts();
            layout_controls();
            LRESULT(0)
        }

        WM_GETMINMAXINFO => {
            let info = &mut *(lparam.0 as *mut MINMAXINFO);
            info.ptMinTrackSize.x = scale_for_dpi(window, 400);
            info.ptMinTrackSize.y = scale_for_dpi(window, 350);
            LRESULT(0)
        }

        // WM_PAINT fills the whole client area; erasing first only flickers.
        WM_ERASEBKGND => LRESULT(1),

        WM_PAINT => {
            let mut paint = PAINTSTRUCT::default();
            let dc = BeginPaint(window, &mut paint);
            let mut client = RECT::default();
            let _ = GetClientRect(window, &mut client);
            FillRect(dc, &client, ui().background);
            let _ = EndPaint(window, &paint);
            LRESULT(0)
        }

        WM_CTLCOLORSTATIC => {
            let dc = HDC(wparam.0 as *mut _);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, rgb(28, 33, 41));
            LRESULT(ui().background.0 as isize)
        }

        WM_HSCROLL => {
            let handles = ui();
            if lparam.0 as usize == handles.gamma_slider.0 as usize && !with_state(|s| s.updating_controls) {
                with_state(|s| s.disable_auto_for_manual_input());
                flush_ui();
                let code = loword(wparam.0);
                if code == TB_ENDTRACK {
                    // A release can follow THUMBPOSITION or a completed debounce.
                    // Only apply if there is still an outstanding change.
                    with_state(|s| s.flush_pending_slider_apply());
                    flush_ui();
                    return LRESULT(0);
                }
                let gamma = slider_gamma();
                set_text(handles.gamma_value, &format_gamma(gamma));
                let already_applied = with_state(|s| {
                    s.cancel_pending_slider_apply();
                    s.selected_index().is_some_and(|index| {
                        let d = &s.displays[index];
                        d.last_error.is_empty() && gamma == d.effective_gamma()
                    })
                });
                if already_applied {
                    return LRESULT(0);
                }
                with_state(|s| s.slider_apply_pending = true);
                if code == TB_THUMBPOSITION || SetTimer(window, TIMER_APPLY, 90, None) == 0 {
                    with_state(|s| s.flush_pending_slider_apply());
                    flush_ui();
                }
            }
            LRESULT(0)
        }

        WM_TIMER => {
            match wparam.0 {
                TIMER_APPLY => {
                    with_state(|s| s.flush_pending_slider_apply());
                    flush_ui();
                }
                TIMER_DISPLAY_REFRESH => {
                    let _ = KillTimer(window, TIMER_DISPLAY_REFRESH);
                    with_state(|s| {
                        s.flush_pending_slider_apply();
                        s.reset_gpu_sampler();
                        s.refresh_displays(true);
                    });
                    populate_display_combo();
                    update_selected_display_ui();
                }
                TIMER_RAMP_WATCHDOG => {
                    with_state(|s| s.reapply_if_reset());
                    flush_ui();
                }
                TIMER_AUTO_GAMMA => {
                    with_state(|s| s.update_automatic_gamma(false));
                    flush_ui();
                }
                TIMER_AUTO_COLLECT => {
                    let _ = KillTimer(window, TIMER_AUTO_COLLECT);
                    with_state(|s| s.update_automatic_gamma(true));
                    flush_ui();
                }
                TIMER_MAP_RELEASE => {
                    let _ = KillTimer(window, TIMER_MAP_RELEASE);
                    with_state(|s| s.update_automatic_gamma(false));
                    flush_ui();
                }
                _ => return DefWindowProcW(window, message, wparam, lparam),
            }
            LRESULT(0)
        }

        WM_COMMAND => {
            let id = loword(wparam.0) as i32;
            let notification = hiword(wparam.0);
            let handles = ui();
            if id == CONTROL_DISPLAY_COMBO && notification == CBN_SELCHANGE && !with_state(|s| s.updating_controls) {
                let selection = send(handles.display_combo, CB_GETCURSEL, 0, 0).0;
                if selection != CB_ERR as isize {
                    let data = send(handles.display_combo, CB_GETITEMDATA, selection as usize, 0).0;
                    let name = with_state(|s| {
                        if data >= 0 && (data as usize) < s.displays.len() {
                            s.flush_pending_slider_apply();
                            s.selected = data as i32;
                            Some(s.displays[data as usize].device_name.clone())
                        } else {
                            None
                        }
                    });
                    if let Some(name) = name {
                        save_selected_device(&name);
                        update_selected_display_ui();
                    }
                }
                return LRESULT(0);
            }
            if id == CONTROL_CLEAR_HOTKEY && notification == BN_CLICKED {
                clear_saved_hotkey();
                return LRESULT(0);
            }
            if id == CONTROL_AUTO_RUST && notification == BN_CLICKED {
                with_state(|s| s.cancel_pending_slider_apply());
                let enabled = send(handles.auto_rust, BM_GETCHECK, 0, 0).0 == BST_CHECKED.0 as isize;
                with_state(|s| s.set_auto_rust_enabled(enabled, true));
                flush_ui();
                return LRESULT(0);
            }
            if (id == CONTROL_TOGGLE_FIRST || id == CONTROL_TOGGLE_SECOND) && notification == EN_KILLFOCUS {
                save_toggle_value(HWND(lparam.0 as *mut _));
                return LRESULT(0);
            }
            if id as usize == MENU_OPEN {
                show_main_window();
                return LRESULT(0);
            }
            if id as usize == MENU_RESET_ALL_AND_EXIT {
                with_state(|s| s.reset_all_displays());
                let _ = DestroyWindow(window);
                return LRESULT(0);
            }
            DefWindowProcW(window, message, wparam, lparam)
        }

        WM_DISPLAYCHANGE | WM_DEVICECHANGE => {
            schedule_display_refresh(window);
            LRESULT(0)
        }

        WM_POWERBROADCAST => {
            if wparam.0 as u32 == PBT_APMRESUMEAUTOMATIC || wparam.0 as u32 == PBT_APMRESUMESUSPEND {
                schedule_display_refresh(window);
            }
            LRESULT(1)
        }

        WM_WTSSESSION_CHANGE => {
            if matches!(wparam.0 as u32, WTS_SESSION_UNLOCK | WTS_CONSOLE_CONNECT | WTS_REMOTE_CONNECT) {
                schedule_display_refresh(window);
            }
            LRESULT(0)
        }

        MSG_SINGLE_KEY => {
            let focus = GetFocus();
            let handles = ui();
            if focus == handles.toggle_first || focus == handles.toggle_second {
                save_toggle_value(focus);
            }
            with_state(|s| s.toggle_gamma());
            flush_ui();
            LRESULT(0)
        }

        MSG_KEY_CAPTURED => {
            save_hotkey_from_control();
            LRESULT(0)
        }

        MSG_MAP_KEY_STATE => {
            if with_state(|s| s.auto_rust) {
                let _ = KillTimer(window, TIMER_MAP_RELEASE);
                if wparam.0 == 0 {
                    if SetTimer(window, TIMER_MAP_RELEASE, MAP_RELEASE_DELAY_MS, None) == 0 {
                        with_state(|s| s.update_automatic_gamma(false));
                    }
                } else {
                    with_state(|s| s.update_automatic_gamma(false));
                }
                flush_ui();
            }
            LRESULT(0)
        }

        MSG_FOREGROUND_CHANGED => {
            if with_state(|s| s.auto_rust) {
                with_state(|s| s.update_automatic_gamma(false));
                flush_ui();
            }
            LRESULT(0)
        }

        MSG_TRAY => {
            let event = lparam.0 as u32;
            if event == WM_LBUTTONDBLCLK || event == WM_LBUTTONUP {
                show_main_window();
            } else if event == WM_RBUTTONUP || event == WM_CONTEXTMENU {
                show_tray_menu();
            }
            LRESULT(0)
        }

        MSG_SHOW_WINDOW => {
            show_main_window();
            LRESULT(0)
        }

        WM_CLOSE => {
            let handles = ui();
            save_toggle_value(handles.toggle_first);
            save_toggle_value(handles.toggle_second);
            with_state(|s| s.reset_all_displays());
            let _ = DestroyWindow(window);
            LRESULT(0)
        }

        WM_DESTROY => {
            let _ = KillTimer(window, TIMER_DISPLAY_REFRESH);
            let _ = KillTimer(window, TIMER_RAMP_WATCHDOG);
            let _ = KillTimer(window, TIMER_MAP_RELEASE);
            with_state(|s| {
                s.cancel_pending_slider_apply();
                s.stop_auto_gamma();
            });
            let _ = WTSUnRegisterSessionNotification(window);
            remove_single_key_hook();
            remove_tray_icon();
            PostQuitMessage(0);
            LRESULT(0)
        }

        _ => DefWindowProcW(window, message, wparam, lparam),
    }
}

// -------------------------------------------------------------------- entry

fn load_app_icon(instance: HINSTANCE, width_metric: SYSTEM_METRICS_INDEX, height_metric: SYSTEM_METRICS_INDEX) -> HICON {
    unsafe {
        LoadImageW(
            instance,
            PCWSTR(IDI_GAMMA_CHANGER as *const u16),
            IMAGE_ICON,
            GetSystemMetrics(width_metric),
            GetSystemMetrics(height_metric),
            LR_DEFAULTCOLOR,
        )
    }
    .map(|handle| HICON(handle.0))
    .unwrap_or_default()
}

pub fn run() -> i32 {
    let start_in_background = std::env::args().skip(1).any(|a| a.eq_ignore_ascii_case("--background"));

    unsafe {
        let Ok(instance_mutex) = CreateMutexW(None, false, SINGLE_INSTANCE_NAME) else {
            return 1;
        };
        if GetLastError() == ERROR_ALREADY_EXISTS {
            if !start_in_background {
                if let Ok(existing) = FindWindowW(WINDOW_CLASS, PCWSTR::null()) {
                    let _ = PostMessageW(existing, MSG_SHOW_WINDOW, WPARAM(0), LPARAM(0));
                }
            }
            let _ = CloseHandle(instance_mutex);
            return 0;
        }
        let _guard = MutexGuard(instance_mutex);

        let _ = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        let controls = INITCOMMONCONTROLSEX {
            dwSize: std::mem::size_of::<INITCOMMONCONTROLSEX>() as u32,
            dwICC: ICC_BAR_CLASSES | ICC_STANDARD_CLASSES,
        };
        let _ = InitCommonControlsEx(&controls);

        let Ok(module) = GetModuleHandleW(None) else {
            return 1;
        };
        let instance = HINSTANCE(module.0);
        let background = CreateSolidBrush(rgb(247, 249, 252));
        update_ui(|h| {
            h.instance = instance;
            h.background = background;
            h.taskbar_created = RegisterWindowMessageW(w!("TaskbarCreated"));
        });

        let icon = load_app_icon(instance, SM_CXICON, SM_CYICON);
        let small_icon = load_app_icon(instance, SM_CXSMICON, SM_CYSMICON);
        if icon.is_invalid() || small_icon.is_invalid() {
            return 1;
        }
        let class = WNDCLASSEXW {
            cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(main_window_proc),
            hInstance: instance,
            hIcon: icon,
            hIconSm: small_icon,
            hCursor: LoadCursorW(None, IDC_ARROW).unwrap_or_default(),
            hbrBackground: background,
            lpszClassName: WINDOW_CLASS,
            ..Default::default()
        };
        if RegisterClassExW(&class) == 0 {
            return 1;
        }

        let title = wide(WINDOW_TITLE);
        let Ok(window) = CreateWindowExW(
            WS_EX_CONTROLPARENT,
            WINDOW_CLASS,
            PCWSTR(title.as_ptr()),
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            scale_for_dpi(HWND::default(), 460),
            scale_for_dpi(HWND::default(), 350),
            None,
            None,
            instance,
            None,
        ) else {
            return 1;
        };

        if !start_in_background {
            let _ = ShowWindow(window, SW_SHOWDEFAULT);
            let _ = UpdateWindow(window);
        }

        let mut message = MSG::default();
        while GetMessageW(&mut message, None, 0, 0).0 > 0 {
            let handles = ui();
            if message.message == WM_KEYDOWN && message.wParam.0 as u16 == VK_RETURN.0 {
                let focus = GetFocus();
                if focus == handles.toggle_first || focus == handles.toggle_second {
                    let _ = SetFocus(handles.gamma_slider);
                    continue;
                }
            }
            if !IsDialogMessageW(window, &message).as_bool() {
                let _ = TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        let handles = ui();
        let _ = DeleteObject(handles.value_font);
        let _ = DeleteObject(handles.body_font);
        let _ = DeleteObject(handles.background);
        message.wParam.0 as i32
    }
}

struct MutexGuard(HANDLE);

impl Drop for MutexGuard {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseHandle(self.0);
        }
    }
}

// Keeps the 256-entry ramp size tied to the one GDI expects.
const _: () = assert!(RAMP_ENTRIES == 256);
