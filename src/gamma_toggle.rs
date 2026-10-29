use crate::gamma_math::{MAXIMUM_GAMMA, MINIMUM_GAMMA};

/// Accepts `[ws][+-]digits[.digits][e[+-]digits][ws]` only; Rust's float parser
/// would also take "inf" and "nan". The result is rounded to hundredths.
pub fn parse_toggle_gamma(text: &str) -> Option<f64> {
    let trimmed = text.trim();
    if trimmed.is_empty() || !trimmed.is_ascii() {
        return None;
    }
    let bytes = trimmed.as_bytes();
    let mut i = 0;
    let digits = |i: &mut usize| {
        let start = *i;
        while *i < bytes.len() && bytes[*i].is_ascii_digit() {
            *i += 1;
        }
        *i - start
    };
    if matches!(bytes[i], b'+' | b'-') {
        i += 1;
    }
    let mut mantissa = digits(&mut i);
    if i < bytes.len() && bytes[i] == b'.' {
        i += 1;
        mantissa += digits(&mut i);
    }
    if mantissa == 0 {
        return None;
    }
    if i < bytes.len() && matches!(bytes[i], b'e' | b'E') {
        i += 1;
        if i < bytes.len() && matches!(bytes[i], b'+' | b'-') {
            i += 1;
        }
        if digits(&mut i) == 0 {
            return None;
        }
    }
    if i != bytes.len() {
        return None;
    }

    let value: f64 = trimmed.parse().ok()?;
    if !value.is_finite() || !(MINIMUM_GAMMA..=MAXIMUM_GAMMA).contains(&value) {
        return None;
    }
    Some((value * 100.0).round() / 100.0)
}

/// From any other slider value, the first press selects the second endpoint.
pub fn next_toggle_gamma(current: f64, first: f64, second: f64) -> f64 {
    if (current - second).abs() < 0.0001 {
        first
    } else {
        second
    }
}
