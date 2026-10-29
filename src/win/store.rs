//! Registry persistence. The layout matches earlier releases so existing
//! settings carry over.

use windows::core::PCWSTR;
use windows::Win32::Foundation::ERROR_SUCCESS;
use windows::Win32::System::Registry::{
    RegCloseKey, RegCreateKeyExW, RegDeleteValueW, RegOpenKeyExW, RegQueryValueExW,
    RegSetValueExW, HKEY, HKEY_CURRENT_USER, KEY_READ, KEY_SET_VALUE, REG_BINARY, REG_DWORD,
    REG_OPTION_NON_VOLATILE, REG_SAM_FLAGS, REG_SZ, REG_VALUE_TYPE,
};

use gamma_changer::gamma_math::{clamp_gamma, GammaRamp};

use super::gdi::{format_system_error, from_native, to_native};
use super::{from_wide, wide};

pub const APP_PATH: &str = "Software\\GammaChanger";

pub struct Key(HKEY);

impl Drop for Key {
    fn drop(&mut self) {
        unsafe {
            let _ = RegCloseKey(self.0);
        }
    }
}

impl Key {
    pub fn open(path: &str, access: REG_SAM_FLAGS, create: bool) -> Option<Key> {
        let path = wide(path);
        let mut key = HKEY::default();
        let status = unsafe {
            if create {
                RegCreateKeyExW(
                    HKEY_CURRENT_USER,
                    PCWSTR(path.as_ptr()),
                    0,
                    PCWSTR::null(),
                    REG_OPTION_NON_VOLATILE,
                    access,
                    None,
                    &mut key,
                    None,
                )
            } else {
                RegOpenKeyExW(HKEY_CURRENT_USER, PCWSTR(path.as_ptr()), 0, access, &mut key)
            }
        };
        (status == ERROR_SUCCESS).then_some(Key(key))
    }

    pub fn read_dword(&self, name: &str) -> Option<u32> {
        let name = wide(name);
        let mut value = 0u32;
        let mut size = 4u32;
        let mut kind = REG_VALUE_TYPE(0);
        let status = unsafe {
            RegQueryValueExW(
                self.0,
                PCWSTR(name.as_ptr()),
                None,
                Some(&mut kind),
                Some((&mut value as *mut u32).cast()),
                Some(&mut size),
            )
        };
        (status == ERROR_SUCCESS && kind == REG_DWORD && size == 4).then_some(value)
    }

    pub fn write_dword(&self, name: &str, value: u32) -> Result<(), String> {
        self.set(name, REG_DWORD, &value.to_le_bytes())
    }

    pub fn read_ramp(&self, name: &str) -> Option<GammaRamp> {
        let name = wide(name);
        let mut native = [0u16; 768];
        let mut size = std::mem::size_of_val(&native) as u32;
        let mut kind = REG_VALUE_TYPE(0);
        let status = unsafe {
            RegQueryValueExW(
                self.0,
                PCWSTR(name.as_ptr()),
                None,
                Some(&mut kind),
                Some(native.as_mut_ptr().cast()),
                Some(&mut size),
            )
        };
        (status == ERROR_SUCCESS && kind == REG_BINARY && size as usize == 768 * 2)
            .then(|| from_native(&native))
    }

    pub fn write_ramp(&self, name: &str, ramp: &GammaRamp) -> Result<(), String> {
        let bytes: Vec<u8> = to_native(ramp).iter().flat_map(|v| v.to_le_bytes()).collect();
        self.set(name, REG_BINARY, &bytes)
    }

    pub fn read_string(&self, name: &str) -> Option<String> {
        let name = wide(name);
        let mut size = 0u32;
        let mut kind = REG_VALUE_TYPE(0);
        unsafe {
            if RegQueryValueExW(self.0, PCWSTR(name.as_ptr()), None, Some(&mut kind), None, Some(&mut size))
                != ERROR_SUCCESS
                || kind != REG_SZ
                || size < 2
            {
                return None;
            }
            let mut buffer = vec![0u16; size as usize / 2 + 1];
            let status = RegQueryValueExW(
                self.0,
                PCWSTR(name.as_ptr()),
                None,
                Some(&mut kind),
                Some(buffer.as_mut_ptr().cast()),
                Some(&mut size),
            );
            (status == ERROR_SUCCESS).then(|| from_wide(&buffer))
        }
    }

    pub fn write_string(&self, name: &str, value: &str) -> Result<(), String> {
        let bytes: Vec<u8> = wide(value).iter().flat_map(|v| v.to_le_bytes()).collect();
        self.set(name, REG_SZ, &bytes)
    }

    pub fn delete_value(&self, name: &str) {
        let name = wide(name);
        unsafe {
            let _ = RegDeleteValueW(self.0, PCWSTR(name.as_ptr()));
        }
    }

    fn set(&self, name: &str, kind: REG_VALUE_TYPE, data: &[u8]) -> Result<(), String> {
        let wide_name = wide(name);
        let status =
            unsafe { RegSetValueExW(self.0, PCWSTR(wide_name.as_ptr()), 0, kind, Some(data)) };
        if status == ERROR_SUCCESS {
            Ok(())
        } else {
            Err(format!("Could not write '{name}': {}", format_system_error(status.0)))
        }
    }
}

pub fn open_app_key_for_write() -> Result<Key, String> {
    Key::open(APP_PATH, KEY_SET_VALUE, true)
        .ok_or_else(|| "Could not open the settings registry key.".to_string())
}

pub fn open_app_key_for_read() -> Option<Key> {
    Key::open(APP_PATH, KEY_READ, false)
}

fn hash_device_name(name: &str) -> u64 {
    let mut hash: u64 = 1469598103934665603;
    for unit in name.encode_utf16() {
        let folded = if unit < 128 { u16::from((unit as u8).to_ascii_lowercase()) } else { unit };
        hash ^= u64::from(folded & 0xFF);
        hash = hash.wrapping_mul(1099511628211);
        hash ^= u64::from(folded >> 8);
        hash = hash.wrapping_mul(1099511628211);
    }
    hash
}

pub fn display_path(device_name: &str) -> String {
    format!("{APP_PATH}\\Displays\\{:016x}", hash_device_name(device_name))
}

/// What is remembered for one display between runs.
#[derive(Clone, Default)]
pub struct PersistentDisplaySettings {
    pub gamma: f64,
    pub enabled: bool,
    pub baseline: GammaRamp,
    pub last_applied: GammaRamp,
    pub has_baseline: bool,
    pub has_last_applied: bool,
}

pub fn load_display_settings(device_name: &str) -> PersistentDisplaySettings {
    let mut settings = PersistentDisplaySettings { gamma: 1.0, ..Default::default() };
    let Some(key) = Key::open(&display_path(device_name), KEY_READ, false) else {
        return settings;
    };
    if let Some(milli) = key.read_dword("GammaMilli") {
        settings.gamma = clamp_gamma(f64::from(milli) / 1000.0);
    }
    if let Some(enabled) = key.read_dword("Enabled") {
        settings.enabled = enabled != 0;
    }
    if let Some(ramp) = key.read_ramp("BaselineRamp") {
        settings.baseline = ramp;
        settings.has_baseline = true;
    }
    if let Some(ramp) = key.read_ramp("LastAppliedRamp") {
        settings.last_applied = ramp;
        settings.has_last_applied = true;
    }
    settings
}

pub fn read_selected_device() -> Option<String> {
    open_app_key_for_read()?.read_string("SelectedDisplay")
}
