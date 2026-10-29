use gamma_changer::single_key::{SingleKeyAction as A, SingleKeyInput, SingleKeyResult};

const LSHIFT: u32 = 0xA0;
const RSHIFT: u32 = 0xA1;
const LCTRL: u32 = 0xA2;
const KEY_A: u32 = 0x41;

fn expect(r: SingleKeyResult, suppress: bool, action: A, msg: &str) {
    assert!(r.suppress == suppress && r.action == action, "{msg}: got {r:?}");
}

#[test]
fn right_shift_and_repeats() {
    let mut i = SingleKeyInput::new();
    i.set_binding(RSHIFT);
    expect(i.process(LSHIFT, true, true), false, A::None, "left shift ignored");
    expect(i.process(RSHIFT, true, true), true, A::Toggle, "toggle");
    for _ in 0..10 {
        expect(i.process(RSHIFT, true, true), true, A::None, "repeat");
    }
    expect(i.process(RSHIFT, false, true), true, A::None, "release consumed");
    expect(i.process(LSHIFT, false, true), false, A::None, "unbound release");
    expect(i.process(RSHIFT, true, true), true, A::Toggle, "toggle again");
}

#[test]
fn single_modifier_capture() {
    for key in [0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0x5B, 0x5C] {
        let mut i = SingleKeyInput::new();
        i.set_binding(KEY_A);
        i.begin_capture();
        expect(i.process(key, true, false), true, A::Captured, "captured");
        assert!(!i.is_capturing());
        assert_eq!(i.bound_key(), KEY_A);
        expect(i.process(key, true, false), true, A::None, "repeat suppressed");
        expect(i.process(KEY_A, true, false), false, A::None, "second key ignored");
        expect(i.process(key, false, false), true, A::None, "release consumed");
        expect(i.process(KEY_A, false, false), false, A::None, "second release passes");
        i.set_binding(key);
        expect(i.process(key, true, true), true, A::Toggle, "binding works");
    }
}

#[test]
fn recording_existing_binding() {
    let mut i = SingleKeyInput::new();
    i.set_binding(RSHIFT);
    i.begin_capture();
    expect(i.process(RSHIFT, true, false), true, A::Captured, "no toggle while recording");
    i.set_binding(RSHIFT);
    expect(i.process(RSHIFT, true, true), true, A::None, "held key does not toggle");
    expect(i.process(RSHIFT, false, true), true, A::None, "release suppressed");
    expect(i.process(RSHIFT, true, true), true, A::Toggle, "next press toggles");
}

#[test]
fn rebind_and_clear_while_held() {
    let mut i = SingleKeyInput::new();
    i.set_binding(RSHIFT);
    i.process(RSHIFT, true, true);
    i.set_binding(LSHIFT);
    expect(i.process(RSHIFT, true, true), true, A::None, "no leaked repeats");
    expect(i.process(RSHIFT, false, true), true, A::None, "release consumed");
    expect(i.process(RSHIFT, true, true), false, A::None, "old key normal");
    expect(i.process(LSHIFT, true, true), true, A::Toggle, "new binding");
    i.set_binding(0);
    expect(i.process(LSHIFT, false, true), true, A::None, "cleared release consumed");
    expect(i.process(LSHIFT, true, true), false, A::None, "cleared key normal");
}

#[test]
fn held_modifiers_and_cancellation() {
    let mut i = SingleKeyInput::new();
    i.set_binding(RSHIFT);
    expect(i.process(LCTRL, true, true), false, A::None, "unbound modifier");
    i.begin_capture();
    i.cancel_capture();
    expect(i.process(RSHIFT, true, true), true, A::Toggle, "toggle after cancel");
    expect(i.process(RSHIFT, false, true), true, A::None, "release suppressed");
    expect(i.process(LCTRL, false, true), false, A::None, "ctrl release passes");
}

#[test]
fn disabled_and_invalid_keys() {
    let mut i = SingleKeyInput::new();
    expect(i.process(RSHIFT, true, true), false, A::None, "no binding");
    i.begin_capture();
    for key in [0u32, 1, 0x10, 0x11, 0x12, 0xFF, 0x100] {
        i.set_binding(key);
        assert_eq!(i.bound_key(), 0);
        expect(i.process(key, true, true), false, A::None, "invalid ignored");
    }
    expect(i.process(RSHIFT, true, false), true, A::Captured, "still capturing");
}
