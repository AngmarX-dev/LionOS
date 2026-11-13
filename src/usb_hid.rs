//! USB HID boot-protocol classification and bounded report decoding.
//! This module does not submit xHCI transfers or claim a live USB device.
use crate::usb::ConfigurationSummary;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HidKind { Keyboard, Mouse }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HidBinding {
    pub kind: HidKind,
    pub interface_number: u8,
    pub endpoint_address: u8,
    pub max_packet_size: u16,
    pub interval: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HidError { NoBootInterface, NoInterruptInEndpoint, ShortReport, KeyboardRollover }

/// Find the first boot keyboard or mouse interface and its own interrupt-IN endpoint.
pub fn find_boot_binding(config: &ConfigurationSummary, wanted: HidKind)
    -> Result<HidBinding, HidError>
{
    for interface in config.interfaces.iter().flatten() {
        if interface.alternate_setting != 0 { continue; }
        let kind = if interface.class != 0x03 || interface.subclass != 0x01 {
            continue;
        } else if interface.protocol == 0x01 {
            HidKind::Keyboard
        } else if interface.protocol == 0x02 {
            HidKind::Mouse
        } else {
            continue;
        };
        if kind != wanted { continue; }
        for endpoint in config.endpoints.iter().flatten() {
            if endpoint.interface_number == interface.number
                && endpoint.address & 0x80 != 0
                && endpoint.attributes & 0x03 == 0x03
                && endpoint.max_packet_size & 0x07FF >= if kind == HidKind::Keyboard { 8 } else { 3 }
            {
                return Ok(HidBinding {
                    kind,
                    interface_number: interface.number,
                    endpoint_address: endpoint.address,
                    max_packet_size: endpoint.max_packet_size & 0x07FF,
                    interval: endpoint.interval,
                });
            }
        }
        return Err(HidError::NoInterruptInEndpoint);
    }
    Err(HidError::NoBootInterface)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HidSetupRequest {
    pub request_type: u8,
    pub request: u8,
    pub value: u16,
    pub index: u16,
    pub length: u16,
}

/// Standard class/interface requests required to select HID boot protocol.
pub fn boot_protocol_requests(interface_number: u8) -> [HidSetupRequest; 2] {
    [
        HidSetupRequest { request_type: 0x21, request: 0x0B, value: 0, index: interface_number as u16, length: 0 },
        HidSetupRequest { request_type: 0x21, request: 0x0A, value: 0, index: interface_number as u16, length: 0 },
    ]
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BootKeyboardReport {
    pub modifiers: u8,
    pub keys: [u8; 6],
}
impl BootKeyboardReport {
    pub fn parse(bytes: &[u8]) -> Result<Self, HidError> {
        if bytes.len() < 8 { return Err(HidError::ShortReport); }
        let keys = [bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]];
        if keys.iter().any(|key| *key == 1) { return Err(HidError::KeyboardRollover); }
        Ok(Self { modifiers: bytes[0], keys })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KeyTransition { pub usage: u8, pub pressed: bool }

/// Compares successive boot reports, including modifier usages E0-E7.
pub struct KeyboardTracker {
    previous: BootKeyboardReport,
    initialized: bool,
}
impl KeyboardTracker {
    pub const fn new() -> Self {
        Self {
            previous: BootKeyboardReport { modifiers: 0, keys: [0; 6] },
            initialized: false,
        }
    }

    pub fn update(&mut self, current: BootKeyboardReport) -> [Option<KeyTransition>; 20] {
        let mut output = [None; 20];
        let mut count = 0usize;
        for bit in 0..8 {
            let old = self.previous.modifiers & (1 << bit) != 0;
            let new = current.modifiers & (1 << bit) != 0;
            if old != new || (!self.initialized && new) {
                output[count] = Some(KeyTransition { usage: 0xE0 + bit, pressed: new });
                count += 1;
            }
        }
        for usage in self.previous.keys {
            if usage != 0 && !current.keys.contains(&usage) {
                output[count] = Some(KeyTransition { usage, pressed: false });
                count += 1;
            }
        }
        for usage in current.keys {
            if usage != 0 && !self.previous.keys.contains(&usage) {
                output[count] = Some(KeyTransition { usage, pressed: true });
                count += 1;
            }
        }
        self.previous = current;
        self.initialized = true;
        output
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BootMouseReport {
    pub buttons: u8,
    pub x: i8,
    pub y: i8,
    pub wheel: i8,
}
impl BootMouseReport {
    pub fn parse(bytes: &[u8]) -> Result<Self, HidError> {
        if bytes.len() < 3 { return Err(HidError::ShortReport); }
        Ok(Self {
            buttons: bytes[0] & 0x07,
            x: bytes[1] as i8,
            y: bytes[2] as i8,
            wheel: if bytes.len() >= 4 { bytes[3] as i8 } else { 0 },
        })
    }
}

/// Translate common USB HID keyboard usages to unshifted ASCII.
pub fn usage_to_ascii(usage: u8) -> Option<u8> {
    Some(match usage {
        0x04..=0x1D => b'a' + (usage - 0x04),
        0x1E..=0x26 => b'1' + (usage - 0x1E),
        0x27 => b'0',
        0x28 => b'\n', 0x2A => 8, 0x2B => b'\t', 0x2C => b' ',
        0x2D => b'-', 0x2E => b'=', 0x2F => b'[', 0x30 => b']',
        0x31 => b'\\', 0x33 => b';', 0x34 => b'\'', 0x35 => 0x60,
        0x36 => b',', 0x37 => b'.', 0x38 => b'/', _ => return None,
    })
}
