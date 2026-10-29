use windows::core::{Error, HRESULT, PCWSTR};
use windows::Win32::Foundation::{GetLastError, SetLastError, ERROR_SUCCESS, WIN32_ERROR};
use windows::Win32::Graphics::Gdi::{
    CreateDCW, DeleteDC, HDC,
};

use windows::Win32::UI::ColorSystem::{GetDeviceGammaRamp, SetDeviceGammaRamp};

use gamma_changer::gamma_math::{ramps_approximately_equal, GammaRamp, RAMP_ENTRIES};

use super::wide;

#[repr(C)]
struct NativeRamp {
    red: [u16; RAMP_ENTRIES],
    green: [u16; RAMP_ENTRIES],
    blue: [u16; RAMP_ENTRIES],
}

pub fn to_native(ramp: &GammaRamp) -> [u16; 3 * RAMP_ENTRIES] {
    let mut out = [0u16; 3 * RAMP_ENTRIES];
    for (channel, chunk) in ramp.channels.iter().zip(out.chunks_mut(RAMP_ENTRIES)) {
        chunk.copy_from_slice(channel);
    }
    out
}

pub fn from_native(native: &[u16]) -> GammaRamp {
    let mut ramp = GammaRamp::default();
    for (channel, chunk) in ramp.channels.iter_mut().zip(native.chunks(RAMP_ENTRIES)) {
        channel.copy_from_slice(chunk);
    }
    ramp
}

pub fn format_system_error(code: u32) -> String {
    if code == 0 {
        return "Unknown error".to_string();
    }
    let message = Error::from(HRESULT::from_win32(code)).message();
    let message = message.trim();
    if message.is_empty() {
        format!("Windows error {code}")
    } else {
        message.to_string()
    }
}

struct DeviceContext(HDC);

impl DeviceContext {
    fn open(device_name: &str) -> Result<Self, String> {
        let name = wide(device_name);
        let dc = unsafe {
            CreateDCW(
                PCWSTR(wide("DISPLAY").as_ptr()),
                PCWSTR(name.as_ptr()),
                PCWSTR::null(),
                None,
            )
        };
        if dc.is_invalid() {
            let error = unsafe { GetLastError() };
            return Err(format!("Could not open {device_name}: {}", format_system_error(error.0)));
        }
        Ok(Self(dc))
    }
}

impl Drop for DeviceContext {
    fn drop(&mut self) {
        unsafe {
            let _ = DeleteDC(self.0);
        }
    }
}

fn read_ramp_from_dc(dc: HDC) -> Result<GammaRamp, String> {
    let mut native = NativeRamp {
        red: [0; RAMP_ENTRIES],
        green: [0; RAMP_ENTRIES],
        blue: [0; RAMP_ENTRIES],
    };
    unsafe {
        SetLastError(ERROR_SUCCESS);
        let succeeded = GetDeviceGammaRamp(dc, (&mut native as *mut NativeRamp).cast());
        let error: WIN32_ERROR = GetLastError();
        if !succeeded.as_bool() {
            let mut message = "The display driver does not expose a readable gamma ramp".to_string();
            if error != ERROR_SUCCESS {
                message += &format!(": {}", format_system_error(error.0));
            }
            return Err(message);
        }
    }
    let mut ramp = GammaRamp::default();
    ramp.channels[0] = native.red;
    ramp.channels[1] = native.green;
    ramp.channels[2] = native.blue;
    Ok(ramp)
}

pub fn read_device_ramp(device_name: &str) -> Result<GammaRamp, String> {
    let dc = DeviceContext::open(device_name)?;
    read_ramp_from_dc(dc.0)
}

/// Writes the ramp and returns what the driver reports back, verifying that it
/// was really applied.
pub fn write_device_ramp(device_name: &str, requested: &GammaRamp) -> Result<GammaRamp, String> {
    let dc = DeviceContext::open(device_name)?;
    let mut native = NativeRamp {
        red: requested.channels[0],
        green: requested.channels[1],
        blue: requested.channels[2],
    };
    unsafe {
        SetLastError(ERROR_SUCCESS);
        let succeeded = SetDeviceGammaRamp(dc.0, (&mut native as *mut NativeRamp).cast());
        let error = GetLastError();
        if !succeeded.as_bool() {
            let mut message = "The display driver rejected the gamma ramp".to_string();
            if error != ERROR_SUCCESS {
                message += &format!(": {}", format_system_error(error.0));
            }
            return Err(message);
        }
    }

    // Verify through the context we already opened for this write.
    let actual = read_ramp_from_dc(dc.0).map_err(|read_error| {
        format!("Windows accepted the gamma ramp, but it could not be verified. {read_error}")
    })?;

    // GDI can report success while its safety heuristics silently reject a ramp.
    // A 1024-unit allowance covers ordinary 8/10-bit LUT quantization on readback.
    if !ramps_approximately_equal(requested, &actual, 1024) {
        return Err("Windows reported success, but the display driver did not apply the \
                    requested ramp. Turn HDR off and check the graphics driver."
            .to_string());
    }
    Ok(actual)
}
