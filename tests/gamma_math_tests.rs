use gamma_changer::gamma_math::*;

fn custom_ramp() -> GammaRamp {
    let mut r = make_identity_ramp();
    r.channels[0][63] = 12345;
    r.channels[1][127] = 30001;
    r.channels[2][191] = 50002;
    r
}

#[test]
fn identity_ramp() {
    let identity = make_identity_ramp();
    for channel in &identity.channels {
        assert_eq!(channel[0], 0);
        assert_eq!(channel[255], 65535);
        assert_eq!(channel[128], 32896);
    }
    assert!(is_monotonic(&identity));
}

#[test]
fn neutral_is_byte_exact() {
    let custom = custom_ramp();
    assert_eq!(apply_relative_gamma(&custom, 1.0), custom);
}

#[test]
fn direction_and_endpoints() {
    let identity = make_identity_ramp();
    let brighter = apply_relative_gamma(&identity, 2.0);
    let darker = apply_relative_gamma(&identity, 0.5);
    for c in 0..3 {
        assert_eq!(brighter.channels[c][0], 0);
        assert_eq!(brighter.channels[c][255], 65535);
        assert_eq!(darker.channels[c][0], 0);
        assert_eq!(darker.channels[c][255], 65535);
        assert!(brighter.channels[c][128] > identity.channels[c][128]);
        assert!(darker.channels[c][128] < identity.channels[c][128]);
    }
    assert!(is_monotonic(&brighter) && is_monotonic(&darker));
}

#[test]
fn known_midpoint() {
    let two = apply_relative_gamma(&make_identity_ramp(), 2.0);
    let expected = (128.0f64 / 255.0).sqrt() * 65535.0;
    assert!((f64::from(two.channels[0][128]) - expected).abs() < 2.0);
}

#[test]
fn clamping() {
    assert_eq!(clamp_gamma(0.01), MINIMUM_GAMMA);
    assert_eq!(clamp_gamma(99.0), MAXIMUM_GAMMA);
    assert_eq!(clamp_gamma(f64::NAN), 1.0);
}

#[test]
fn calibrated_interpolation() {
    let mut baseline = GammaRamp::default();
    for index in 0..RAMP_ENTRIES {
        baseline.channels[0][index] = (100 + index.min(128) * 400) as u16;
        baseline.channels[1][index] = (3000 + index * 200) as u16;
        baseline.channels[2][index] =
            (4096 + if index > 96 { (index - 96) * 300 } else { 0 }) as u16;
    }
    for step in 50..=300 {
        let gamma = f64::from(step) / 100.0;
        let adjusted = apply_relative_gamma(&baseline, gamma);
        assert!(is_monotonic(&adjusted));
        for c in 0..RAMP_CHANNELS {
            assert_eq!(adjusted.channels[c][0], baseline.channels[c][0]);
            assert_eq!(adjusted.channels[c][255], baseline.channels[c][255]);
        }
        for index in 0..RAMP_ENTRIES {
            let position = (index as f64 / 255.0).powf(1.0 / gamma) * 255.0;
            let expected = [
                100.0 + position.min(128.0) * 400.0,
                3000.0 + position * 200.0,
                4096.0 + (position - 96.0).max(0.0) * 300.0,
            ];
            for c in 0..3 {
                let diff = (i64::from(adjusted.channels[c][index]) - expected[c].round() as i64).abs();
                assert!(diff <= 1, "gamma {gamma} channel {c} index {index}");
            }
        }
    }
}

#[test]
fn invalid_gamma_preserves_baseline() {
    let baseline = custom_ramp();
    for gamma in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        assert_eq!(apply_relative_gamma(&baseline, gamma), baseline);
    }
    assert_eq!(
        apply_relative_gamma(&baseline, 0.01),
        apply_relative_gamma(&baseline, MINIMUM_GAMMA)
    );
    assert_eq!(
        apply_relative_gamma(&baseline, 99.0),
        apply_relative_gamma(&baseline, MAXIMUM_GAMMA)
    );
}

#[test]
fn readback_tolerance() {
    let baseline = make_identity_ramp();
    let mut actual = baseline;
    actual.channels[2][254] -= 1024;
    assert!(ramps_approximately_equal(&baseline, &actual, DEFAULT_TOLERANCE));
    assert!(ramps_approximately_equal(&actual, &baseline, DEFAULT_TOLERANCE));
    assert!(!ramps_approximately_equal(&baseline, &actual, 0));
    actual.channels[2][254] -= 1;
    assert!(!ramps_approximately_equal(&baseline, &actual, DEFAULT_TOLERANCE));
    assert!(!ramps_approximately_equal(&actual, &baseline, DEFAULT_TOLERANCE));
}

#[test]
fn windows_safety_envelope() {
    let identity = make_identity_ramp();
    for step in 50..=300 {
        let adjusted = apply_relative_gamma(&identity, f64::from(step) / 100.0);
        assert!(is_monotonic(&adjusted));
        for c in 0..3 {
            for i in 0..256 {
                let diff = (i32::from(adjusted.channels[c][i]) - i32::from(identity.channels[c][i])).abs();
                // GDI requires each value to remain within 32768 of identity.
                assert!(diff <= 32768);
            }
        }
    }
}
