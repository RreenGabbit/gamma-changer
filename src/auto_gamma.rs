// The gap between these thresholds prevents a scene hovering near the switch
// point from repeatedly changing the display gamma.
pub const SWITCH_TO_NIGHT_LUMINANCE: f64 = 0.020;
pub const SWITCH_TO_DAY_LUMINANCE: f64 = 0.055;
pub const LUMINANCE_SMOOTHING: f64 = 0.35;
pub const NIGHT_CONFIRMATION_SAMPLES: i32 = 2;
pub const DAY_CONFIRMATION_SAMPLES: i32 = 3;

#[derive(Clone, Debug, Default)]
pub struct AutoGammaDetector {
    initialized: bool,
    night: bool,
    confirmations: i32,
    filtered_luminance: f64,
}

impl AutoGammaDetector {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn reset(&mut self) {
        *self = Self::default();
    }

    pub fn update(&mut self, luminance: f64) -> bool {
        if !luminance.is_finite() {
            self.reset();
            return false;
        }

        if !self.initialized {
            self.filtered_luminance = luminance;
            self.initialized = true;
        } else {
            self.filtered_luminance += (luminance - self.filtered_luminance) * LUMINANCE_SMOOTHING;
        }

        if !self.night {
            if self.filtered_luminance <= SWITCH_TO_NIGHT_LUMINANCE
                && luminance <= SWITCH_TO_NIGHT_LUMINANCE
            {
                self.confirmations += 1;
                if self.confirmations >= NIGHT_CONFIRMATION_SAMPLES {
                    self.night = true;
                    self.confirmations = 0;
                }
            } else {
                self.confirmations = 0;
            }
        } else if self.filtered_luminance >= SWITCH_TO_DAY_LUMINANCE
            && luminance >= SWITCH_TO_DAY_LUMINANCE
        {
            self.confirmations += 1;
            if self.confirmations >= DAY_CONFIRMATION_SAMPLES {
                self.night = false;
                self.confirmations = 0;
            }
        } else {
            self.confirmations = 0;
        }
        self.night
    }

    pub fn is_night(&self) -> bool {
        self.night
    }
}

pub fn automatic_gamma_target(use_night_gamma: bool, maximum_gamma: f64) -> f64 {
    if !use_night_gamma || !maximum_gamma.is_finite() {
        return 1.0;
    }
    maximum_gamma.max(1.0)
}
