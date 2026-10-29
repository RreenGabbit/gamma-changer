use gamma_changer::auto_gamma::*;

#[test]
fn detector_hysteresis() {
    let mut d = AutoGammaDetector::new();
    assert!(!d.update(0.004), "one dark sample is not enough");
    assert!(d.update(0.004), "consecutive dark samples switch to night");
    assert!(d.is_night());
    assert!(d.update(0.030), "dead zone holds night mode");
    assert!(d.update(0.500), "a brief flash does not leave night mode");
    assert!(d.update(0.004));

    for sample in 0..DAY_CONFIRMATION_SAMPLES {
        let night = d.update(0.500);
        if sample + 1 < DAY_CONFIRMATION_SAMPLES {
            assert!(night, "day switch waits for sustained brightness");
        } else {
            assert!(!night, "sustained brightness returns to day");
        }
    }
    assert!(!d.is_night());

    d.reset();
    assert!(!d.update(0.030));
    assert!(!d.update(0.030), "dead zone prevents switching to night");
}

#[test]
fn targets_and_invalid_input() {
    assert_eq!(automatic_gamma_target(true, 2.4), 2.4);
    assert_eq!(automatic_gamma_target(false, 2.4), 1.0);
    assert_eq!(automatic_gamma_target(true, 0.8), 1.0);
    assert_eq!(automatic_gamma_target(true, f64::NAN), 1.0);

    let mut d = AutoGammaDetector::new();
    d.update(0.004);
    d.update(0.004);
    assert!(!d.update(f64::NAN));
    assert!(!d.is_night());
}
