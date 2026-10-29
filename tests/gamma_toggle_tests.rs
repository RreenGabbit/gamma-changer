use gamma_changer::gamma_toggle::*;

#[test]
fn rejects_invalid_text() {
    for text in [
        "", " ", "abc", "1.5x", "1 2", "nan", "inf", "0.49", "3.01", "-1", "1,5", "1e999",
        "0x1p0", "+", ".", "1e", "1.5e+", "--1", "1\u{0663}",
    ] {
        assert_eq!(parse_toggle_gamma(text), None, "{text:?} accepted");
    }
}

#[test]
fn accepts_valid_text() {
    assert_eq!(parse_toggle_gamma(" 1.756 "), Some(1.76));
    assert_eq!(parse_toggle_gamma("0.50"), Some(0.5));
    assert_eq!(parse_toggle_gamma("3.00"), Some(3.0));
    assert_eq!(parse_toggle_gamma("+1.5"), Some(1.5));
    assert_eq!(parse_toggle_gamma(".5"), Some(0.5));
    assert_eq!(parse_toggle_gamma("2."), Some(2.0));
    assert_eq!(parse_toggle_gamma("1e0"), Some(1.0));
}

#[test]
fn toggling() {
    assert_eq!(next_toggle_gamma(1.0, 0.8, 2.2), 2.2);
    assert_eq!(next_toggle_gamma(2.2, 0.8, 2.2), 0.8);
    assert_eq!(next_toggle_gamma(0.8, 0.8, 2.2), 2.2);
    assert_eq!(next_toggle_gamma(0.8, 2.2, 0.8), 2.2);
    assert_eq!(next_toggle_gamma(1.5, 1.5, 1.5), 1.5);
    assert_eq!(next_toggle_gamma(3.0, 1.0, 3.0), 1.0);
}
