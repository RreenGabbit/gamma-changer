//! Windows front end: GDI ramp access, registry persistence, GPU brightness
//! sampling and the Win32 user interface.

mod app;
mod gdi;
mod gpu;
mod store;

pub fn run() -> i32 {
    app::run()
}

/// NUL-terminated UTF-16 for passing to Win32.
pub(crate) fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

/// UTF-16 buffer up to the first NUL.
pub(crate) fn from_wide(buffer: &[u16]) -> String {
    let end = buffer.iter().position(|&c| c == 0).unwrap_or(buffer.len());
    String::from_utf16_lossy(&buffer[..end])
}

/// Copies `text` into a fixed buffer, truncating and NUL-terminating.
pub(crate) fn copy_wide(destination: &mut [u16], text: &str) {
    let mut length = 0;
    for unit in text.encode_utf16().take(destination.len().saturating_sub(1)) {
        destination[length] = unit;
        length += 1;
    }
    destination[length] = 0;
}
