pub const RAMP_ENTRIES: usize = 256;
pub const RAMP_CHANNELS: usize = 3;
pub const MINIMUM_GAMMA: f64 = 0.50;
pub const MAXIMUM_GAMMA: f64 = 3.00;

pub type GammaChannel = [u16; RAMP_ENTRIES];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GammaRamp {
    pub channels: [GammaChannel; RAMP_CHANNELS],
}

impl Default for GammaRamp {
    fn default() -> Self {
        Self { channels: [[0; RAMP_ENTRIES]; RAMP_CHANNELS] }
    }
}

pub fn clamp_gamma(gamma: f64) -> f64 {
    if !gamma.is_finite() {
        return 1.0;
    }
    gamma.clamp(MINIMUM_GAMMA, MAXIMUM_GAMMA)
}

pub fn make_identity_ramp() -> GammaRamp {
    let mut ramp = GammaRamp::default();
    for channel in &mut ramp.channels {
        for (index, value) in channel.iter_mut().enumerate() {
            // 257 maps the 8-bit endpoints exactly onto the full 16-bit range.
            *value = (index * 257) as u16;
        }
    }
    ramp
}

/// Composes a power curve with an existing calibration ramp. A gamma of 1.0
/// returns the baseline byte-for-byte. Values above 1.0 brighten midtones;
/// values below 1.0 darken them.
pub fn apply_relative_gamma(baseline: &GammaRamp, requested: f64) -> GammaRamp {
    let gamma = clamp_gamma(requested);
    if (gamma - 1.0).abs() < f64::EPSILON {
        return *baseline;
    }

    let mut result = GammaRamp::default();
    let last_index = (RAMP_ENTRIES - 1) as f64;
    let inverse_gamma = 1.0 / gamma;

    // All channels sample the same positions. Compute the power curve and
    // interpolation weights once per entry, preserving each channel's baseline.
    for index in 0..RAMP_ENTRIES {
        let normalized = index as f64 / last_index;
        let source_position = normalized.powf(inverse_gamma) * last_index;
        let lower = source_position.floor() as usize;
        let upper = (lower + 1).min(RAMP_ENTRIES - 1);
        let fraction = source_position - lower as f64;

        for channel in 0..RAMP_CHANNELS {
            let source = &baseline.channels[channel];
            let interpolated =
                f64::from(source[lower]) * (1.0 - fraction) + f64::from(source[upper]) * fraction;
            result.channels[channel][index] = interpolated.round().clamp(0.0, 65535.0) as u16;
        }
    }
    result
}

pub const DEFAULT_TOLERANCE: u16 = 1024;

pub fn ramps_approximately_equal(left: &GammaRamp, right: &GammaRamp, tolerance: u16) -> bool {
    left.channels.iter().zip(&right.channels).all(|(a, b)| {
        a.iter()
            .zip(b)
            .all(|(&x, &y)| (i32::from(x) - i32::from(y)).abs() <= i32::from(tolerance))
    })
}

pub fn is_monotonic(ramp: &GammaRamp) -> bool {
    ramp.channels.iter().all(|channel| channel.windows(2).all(|pair| pair[1] >= pair[0]))
}
