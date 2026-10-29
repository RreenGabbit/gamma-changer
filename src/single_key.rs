/// Windows virtual-key codes, with left/right modifier keys resolved before use.
pub const fn is_single_key_allowed(key: u32) -> bool {
    key >= 0x08 && key <= 0xFE && key != 0x10 && key != 0x11 && key != 0x12
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum SingleKeyAction {
    #[default]
    None,
    Captured,
    Toggle,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct SingleKeyResult {
    pub suppress: bool,
    pub action: SingleKeyAction,
}

pub struct SingleKeyInput {
    bound_key: u32,
    capturing: bool,
    // Keep consuming repeats and the matching release even if the binding is
    // changed or cleared while a captured/bound key is still held.
    suppressed_keys: [bool; 256],
}

impl Default for SingleKeyInput {
    fn default() -> Self {
        Self { bound_key: 0, capturing: false, suppressed_keys: [false; 256] }
    }
}

impl SingleKeyInput {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn set_binding(&mut self, key: u32) {
        self.bound_key = if is_single_key_allowed(key) { key } else { 0 };
    }

    pub fn bound_key(&self) -> u32 {
        self.bound_key
    }

    pub fn begin_capture(&mut self) {
        self.capturing = true;
    }

    pub fn cancel_capture(&mut self) {
        self.capturing = false;
    }

    pub fn is_capturing(&self) -> bool {
        self.capturing
    }

    pub fn process(&mut self, key: u32, key_down: bool, allow_toggle: bool) -> SingleKeyResult {
        if !is_single_key_allowed(key) {
            return SingleKeyResult::default();
        }
        let slot = key as usize;
        if !key_down {
            let suppressed = std::mem::take(&mut self.suppressed_keys[slot]);
            return SingleKeyResult { suppress: suppressed, action: SingleKeyAction::None };
        }
        if self.suppressed_keys[slot] {
            return SingleKeyResult { suppress: true, action: SingleKeyAction::None };
        }
        if self.capturing {
            self.capturing = false;
            self.suppressed_keys[slot] = true;
            return SingleKeyResult { suppress: true, action: SingleKeyAction::Captured };
        }
        if allow_toggle && key == self.bound_key {
            self.suppressed_keys[slot] = true;
            return SingleKeyResult { suppress: true, action: SingleKeyAction::Toggle };
        }
        SingleKeyResult::default()
    }
}
