//! Bounded USB device-enumeration model and descriptor parser.
//! This module is software-only: it does not submit xHCI commands or transfers.

pub const MAX_ROOT_PORTS: usize = 32;
pub const MAX_USB_DEVICES: usize = 32;
pub const MAX_INTERFACES: usize = 16;
pub const MAX_ENDPOINTS: usize = 32;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DescriptorError {
    TooShort,
    InvalidLength,
    WrongDescriptorType,
    InvalidTotalLength,
    TruncatedDescriptor,
    CapacityExceeded,
    MissingConfiguration,
    InvalidEndpoint,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DeviceDescriptor {
    pub usb_version: u16,
    pub device_class: u8,
    pub device_subclass: u8,
    pub device_protocol: u8,
    pub max_packet_size_ep0: u8,
    pub vendor_id: u16,
    pub product_id: u16,
    pub device_version: u16,
    pub manufacturer_index: u8,
    pub product_index: u8,
    pub serial_index: u8,
    pub configuration_count: u8,
}

impl DeviceDescriptor {
    pub fn parse(bytes: &[u8]) -> Result<Self, DescriptorError> {
        if bytes.len() < 18 { return Err(DescriptorError::TooShort); }
        if bytes[0] != 18 { return Err(DescriptorError::InvalidLength); }
        if bytes[1] != 1 { return Err(DescriptorError::WrongDescriptorType); }
        Ok(Self {
            usb_version: u16::from_le_bytes([bytes[2], bytes[3]]),
            device_class: bytes[4],
            device_subclass: bytes[5],
            device_protocol: bytes[6],
            max_packet_size_ep0: bytes[7],
            vendor_id: u16::from_le_bytes([bytes[8], bytes[9]]),
            product_id: u16::from_le_bytes([bytes[10], bytes[11]]),
            device_version: u16::from_le_bytes([bytes[12], bytes[13]]),
            manufacturer_index: bytes[14],
            product_index: bytes[15],
            serial_index: bytes[16],
            configuration_count: bytes[17],
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct InterfaceDescriptor {
    pub number: u8,
    pub alternate_setting: u8,
    pub endpoint_count: u8,
    pub class: u8,
    pub subclass: u8,
    pub protocol: u8,
    pub string_index: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EndpointDescriptor {
    pub address: u8,
    pub attributes: u8,
    pub max_packet_size: u16,
    pub interval: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ConfigurationSummary {
    pub total_length: u16,
    pub configuration_value: u8,
    pub attributes: u8,
    pub max_power: u8,
    pub interface_count: usize,
    pub endpoint_count: usize,
    pub interfaces: [Option<InterfaceDescriptor>; MAX_INTERFACES],
    pub endpoints: [Option<EndpointDescriptor>; MAX_ENDPOINTS],
}

impl ConfigurationSummary {
    pub const fn empty() -> Self {
        Self {
            total_length: 0,
            configuration_value: 0,
            attributes: 0,
            max_power: 0,
            interface_count: 0,
            endpoint_count: 0,
            interfaces: [None; MAX_INTERFACES],
            endpoints: [None; MAX_ENDPOINTS],
        }
    }
}

/// Parse one complete configuration descriptor blob. The input must contain
/// at least wTotalLength bytes; extra bytes are ignored.
pub fn parse_configuration(bytes: &[u8]) -> Result<ConfigurationSummary, DescriptorError> {
    if bytes.len() < 9 { return Err(DescriptorError::TooShort); }
    if bytes[0] < 9 { return Err(DescriptorError::InvalidLength); }
    if bytes[1] != 2 { return Err(DescriptorError::WrongDescriptorType); }
    let total = u16::from_le_bytes([bytes[2], bytes[3]]) as usize;
    if total < 9 || total > bytes.len() { return Err(DescriptorError::InvalidTotalLength); }

    let mut summary = ConfigurationSummary::empty();
    summary.total_length = total as u16;
    summary.configuration_value = bytes[5];
    summary.attributes = bytes[7];
    summary.max_power = bytes[8];

    let mut offset = 0usize;
    let mut current_interface: Option<usize> = None;
    let mut expected_endpoints = 0u8;
    let mut observed_endpoints = 0u8;
    while offset < total {
        if total - offset < 2 { return Err(DescriptorError::TruncatedDescriptor); }
        let length = bytes[offset] as usize;
        let kind = bytes[offset + 1];
        if length < 2 || offset.checked_add(length).map_or(true, |end| end > total) {
            return Err(DescriptorError::TruncatedDescriptor);
        }
        match kind {
            2 => {
                if offset != 0 || length < 9 { return Err(DescriptorError::InvalidLength); }
            }
            4 => {
                if length < 9 { return Err(DescriptorError::InvalidLength); }
                if current_interface.is_some() && observed_endpoints != expected_endpoints {
                    return Err(DescriptorError::InvalidEndpoint);
                }
                if summary.interface_count == MAX_INTERFACES {
                    return Err(DescriptorError::CapacityExceeded);
                }
                let item = InterfaceDescriptor {
                    number: bytes[offset + 2],
                    alternate_setting: bytes[offset + 3],
                    endpoint_count: bytes[offset + 4],
                    class: bytes[offset + 5],
                    subclass: bytes[offset + 6],
                    protocol: bytes[offset + 7],
                    string_index: bytes[offset + 8],
                };
                current_interface = Some(summary.interface_count);
                expected_endpoints = item.endpoint_count;
                observed_endpoints = 0;
                summary.interfaces[summary.interface_count] = Some(item);
                summary.interface_count += 1;
            }
            5 => {
                if length < 7 || current_interface.is_none() {
                    return Err(DescriptorError::InvalidEndpoint);
                }
                if summary.endpoint_count == MAX_ENDPOINTS {
                    return Err(DescriptorError::CapacityExceeded);
                }
                let endpoint = EndpointDescriptor {
                    address: bytes[offset + 2],
                    attributes: bytes[offset + 3],
                    max_packet_size: u16::from_le_bytes([bytes[offset + 4], bytes[offset + 5]]),
                    interval: bytes[offset + 6],
                };
                if endpoint.address & 0x0F == 0 || endpoint.max_packet_size & 0x07FF == 0 {
                    return Err(DescriptorError::InvalidEndpoint);
                }
                if observed_endpoints >= expected_endpoints {
                    return Err(DescriptorError::InvalidEndpoint);
                }
                observed_endpoints += 1;
                summary.endpoints[summary.endpoint_count] = Some(endpoint);
                summary.endpoint_count += 1;
            }
            _ => {}
        }
        offset += length;
    }
    if current_interface.is_some() && observed_endpoints != expected_endpoints {
        return Err(DescriptorError::InvalidEndpoint);
    }
    Ok(summary)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EnumerationPhase {
    Empty,
    Connected,
    ResetComplete,
    EnableSlotPending,
    SlotEnabled,
    AddressDevicePending,
    Addressed,
    DeviceDescriptorPending,
    DeviceDescriptorReady,
    ConfigurationDescriptorPending,
    ConfigurationReady,
    SetConfigurationPending,
    Enumerated,
    Failed,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EnumerationError {
    InvalidPort,
    NotConnected,
    InvalidTransition,
    NoFreeSlot,
    InvalidSlot,
    Descriptor(DescriptorError),
    NoConfiguration,
    Payload(crate::xhci_dma::UsbPayloadError),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct UsbDeviceRecord {
    pub port: u8,
    pub slot_id: u8,
    pub speed: u8,
    pub vendor_id: u16,
    pub product_id: u16,
    pub device_class: u8,
    pub device_subclass: u8,
    pub device_protocol: u8,
    pub configuration_value: u8,
    pub interface_count: u8,
    pub endpoint_count: u8,
}

#[derive(Clone, Copy)]
struct PortEntry {
    connected: bool,
    speed: u8,
    phase: EnumerationPhase,
    slot_id: u8,
    device: Option<DeviceDescriptor>,
    configuration: Option<ConfigurationSummary>,
}

impl PortEntry {
    const EMPTY: Self = Self {
        connected: false,
        speed: 0,
        phase: EnumerationPhase::Empty,
        slot_id: 0,
        device: None,
        configuration: None,
    };
}

/// Fixed-capacity root-port/device enumeration coordinator.
/// All methods update software metadata only; hardware command/transfer
/// submission remains the responsibility of a future validated xHCI layer.
pub struct UsbDeviceManager {
    ports: [PortEntry; MAX_ROOT_PORTS],
    slot_used: [bool; MAX_USB_DEVICES + 1],
    device_count: usize,
}

impl UsbDeviceManager {
    pub const fn new() -> Self {
        Self {
            ports: [PortEntry::EMPTY; MAX_ROOT_PORTS],
            slot_used: [false; MAX_USB_DEVICES + 1],
            device_count: 0,
        }
    }

    pub fn device_count(&self) -> usize { self.device_count }

    pub fn phase(&self, port: u8) -> Result<EnumerationPhase, EnumerationError> {
        let index = Self::port_index(port)?;
        Ok(self.ports[index].phase)
    }

    fn port_index(port: u8) -> Result<usize, EnumerationError> {
        if port == 0 || port as usize > MAX_ROOT_PORTS {
            Err(EnumerationError::InvalidPort)
        } else {
            Ok(port as usize - 1)
        }
    }

    pub fn observe_port(&mut self, port: u8, connected: bool, speed: u8)
        -> Result<EnumerationPhase, EnumerationError>
    {
        let index = Self::port_index(port)?;
        let entry = &mut self.ports[index];
        if !connected {
            if entry.slot_id != 0 {
                self.slot_used[entry.slot_id as usize] = false;
                entry.slot_id = 0;
                self.device_count = self.device_count.saturating_sub(1);
            }
            *entry = PortEntry::EMPTY;
            return Ok(EnumerationPhase::Empty);
        }
        if speed == 0 || speed > 15 { return Err(EnumerationError::InvalidTransition); }
        if !entry.connected || entry.speed != speed {
            if entry.slot_id != 0 {
                self.slot_used[entry.slot_id as usize] = false;
                self.device_count = self.device_count.saturating_sub(1);
            }
            *entry = PortEntry {
                connected: true,
                speed,
                phase: EnumerationPhase::Connected,
                slot_id: 0,
                device: None,
                configuration: None,
            };
        }
        Ok(entry.phase)
    }

    pub fn note_reset_complete(&mut self, port: u8) -> Result<(), EnumerationError> {
        let entry = &mut self.ports[Self::port_index(port)?];
        if !entry.connected { return Err(EnumerationError::NotConnected); }
        if entry.phase != EnumerationPhase::Connected { return Err(EnumerationError::InvalidTransition); }
        entry.phase = EnumerationPhase::ResetComplete;
        Ok(())
    }

    pub fn request_enable_slot(&mut self, port: u8) -> Result<(), EnumerationError> {
        let entry = &mut self.ports[Self::port_index(port)?];
        if !entry.connected { return Err(EnumerationError::NotConnected); }
        if entry.phase != EnumerationPhase::ResetComplete { return Err(EnumerationError::InvalidTransition); }
        entry.phase = EnumerationPhase::EnableSlotPending;
        Ok(())
    }

    pub fn note_slot_enabled(&mut self, port: u8, slot_id: u8) -> Result<(), EnumerationError> {
        let index = Self::port_index(port)?;
        if slot_id == 0 || slot_id as usize >= self.slot_used.len() {
            return Err(EnumerationError::InvalidSlot);
        }
        let entry = &mut self.ports[index];
        if !entry.connected || entry.phase != EnumerationPhase::EnableSlotPending {
            return Err(EnumerationError::InvalidTransition);
        }
        if self.slot_used[slot_id as usize] { return Err(EnumerationError::InvalidSlot); }
        self.slot_used[slot_id as usize] = true;
        entry.slot_id = slot_id;
        entry.phase = EnumerationPhase::SlotEnabled;
        Ok(())
    }

    pub fn request_address_device(&mut self, port: u8) -> Result<(), EnumerationError> {
        let entry = &mut self.ports[Self::port_index(port)?];
        if entry.phase != EnumerationPhase::SlotEnabled { return Err(EnumerationError::InvalidTransition); }
        entry.phase = EnumerationPhase::AddressDevicePending;
        Ok(())
    }

    pub fn note_addressed(&mut self, port: u8) -> Result<(), EnumerationError> {
        let entry = &mut self.ports[Self::port_index(port)?];
        if entry.phase != EnumerationPhase::AddressDevicePending { return Err(EnumerationError::InvalidTransition); }
        entry.phase = EnumerationPhase::Addressed;
        Ok(())
    }

    pub fn request_device_descriptor(&mut self, port: u8) -> Result<(), EnumerationError> {
        let entry = &mut self.ports[Self::port_index(port)?];
        if entry.phase != EnumerationPhase::Addressed { return Err(EnumerationError::InvalidTransition); }
        entry.phase = EnumerationPhase::DeviceDescriptorPending;
        Ok(())
    }

    /// Consume a completed DMA payload as the standard Device Descriptor.
    /// The caller must first reclaim the payload using the matching TransferResult.
    pub fn accept_device_descriptor_payload(&mut self, port: u8,
        payload: &crate::xhci_dma::UsbPayloadBuffer,
        result: &crate::xhci_dma::TransferResult)
        -> Result<DeviceDescriptor, EnumerationError>
    {
        let mut bytes = [0u8; 18];
        let copied = payload.copy_completed_to(result, &mut bytes)
            .map_err(EnumerationError::Payload)?;
        if copied < bytes.len() {
            return Err(EnumerationError::Descriptor(DescriptorError::TooShort));
        }
        self.accept_device_descriptor(port, &bytes)
    }

    /// Copy a completed Configuration Descriptor payload into caller-owned
    /// scratch storage, then pass its exact transferred length to the parser.
    pub fn accept_configuration_descriptor_payload(&mut self, port: u8,
        payload: &crate::xhci_dma::UsbPayloadBuffer,
        result: &crate::xhci_dma::TransferResult,
        scratch: &mut [u8])
        -> Result<ConfigurationSummary, EnumerationError>
    {
        let copied = payload.copy_completed_to(result, scratch)
            .map_err(EnumerationError::Payload)?;
        self.accept_configuration_descriptor(port, &scratch[..copied])
    }

    pub fn accept_device_descriptor(&mut self, port: u8, bytes: &[u8])
        -> Result<DeviceDescriptor, EnumerationError>
    {
        let entry = &mut self.ports[Self::port_index(port)?];
        if entry.phase != EnumerationPhase::DeviceDescriptorPending {
            return Err(EnumerationError::InvalidTransition);
        }
        let descriptor = DeviceDescriptor::parse(bytes).map_err(EnumerationError::Descriptor)?;
        if descriptor.configuration_count == 0 || descriptor.max_packet_size_ep0 == 0 {
            return Err(EnumerationError::NoConfiguration);
        }
        entry.device = Some(descriptor);
        entry.phase = EnumerationPhase::DeviceDescriptorReady;
        Ok(descriptor)
    }

    pub fn request_configuration_descriptor(&mut self, port: u8) -> Result<(), EnumerationError> {
        let entry = &mut self.ports[Self::port_index(port)?];
        if entry.phase != EnumerationPhase::DeviceDescriptorReady {
            return Err(EnumerationError::InvalidTransition);
        }
        entry.phase = EnumerationPhase::ConfigurationDescriptorPending;
        Ok(())
    }

    pub fn accept_configuration_descriptor(&mut self, port: u8, bytes: &[u8])
        -> Result<ConfigurationSummary, EnumerationError>
    {
        let entry = &mut self.ports[Self::port_index(port)?];
        if entry.phase != EnumerationPhase::ConfigurationDescriptorPending {
            return Err(EnumerationError::InvalidTransition);
        }
        let config = parse_configuration(bytes).map_err(EnumerationError::Descriptor)?;
        let device = entry.device.ok_or(EnumerationError::NoConfiguration)?;
        if config.interface_count == 0 || config.configuration_value == 0 {
            return Err(EnumerationError::NoConfiguration);
        }
        entry.configuration = Some(config);
        entry.phase = EnumerationPhase::ConfigurationReady;
        Ok(config)
    }

    pub fn request_set_configuration(&mut self, port: u8) -> Result<(), EnumerationError> {
        let entry = &mut self.ports[Self::port_index(port)?];
        if entry.phase != EnumerationPhase::ConfigurationReady {
            return Err(EnumerationError::InvalidTransition);
        }
        entry.phase = EnumerationPhase::SetConfigurationPending;
        Ok(())
    }

    pub fn note_configured(&mut self, port: u8) -> Result<UsbDeviceRecord, EnumerationError> {
        let entry = &mut self.ports[Self::port_index(port)?];
        if entry.phase != EnumerationPhase::SetConfigurationPending {
            return Err(EnumerationError::InvalidTransition);
        }
        let device = entry.device.ok_or(EnumerationError::NoConfiguration)?;
        let config = entry.configuration.ok_or(EnumerationError::NoConfiguration)?;
        entry.phase = EnumerationPhase::Enumerated;
        self.device_count = self.device_count.saturating_add(1);
        Ok(UsbDeviceRecord {
            port,
            slot_id: entry.slot_id,
            speed: entry.speed,
            vendor_id: device.vendor_id,
            product_id: device.product_id,
            device_class: device.device_class,
            device_subclass: device.device_subclass,
            device_protocol: device.device_protocol,
            configuration_value: config.configuration_value,
            interface_count: config.interface_count as u8,
            endpoint_count: config.endpoint_count as u8,
        })
    }

    pub fn fail_port(&mut self, port: u8) -> Result<(), EnumerationError> {
        let entry = &mut self.ports[Self::port_index(port)?];
        if entry.slot_id != 0 {
            self.slot_used[entry.slot_id as usize] = false;
            entry.slot_id = 0;
        }
        if entry.phase == EnumerationPhase::Enumerated {
            self.device_count = self.device_count.saturating_sub(1);
        }
        entry.device = None;
        entry.configuration = None;
        entry.phase = EnumerationPhase::Failed;
        Ok(())
    }

    pub fn device(&self, port: u8) -> Result<Option<UsbDeviceRecord>, EnumerationError> {
        let entry = &self.ports[Self::port_index(port)?];
        if entry.phase != EnumerationPhase::Enumerated { return Ok(None); }
        let device = entry.device.ok_or(EnumerationError::NoConfiguration)?;
        let config = entry.configuration.ok_or(EnumerationError::NoConfiguration)?;
        Ok(Some(UsbDeviceRecord {
            port,
            slot_id: entry.slot_id,
            speed: entry.speed,
            vendor_id: device.vendor_id,
            product_id: device.product_id,
            device_class: device.device_class,
            device_subclass: device.device_subclass,
            device_protocol: device.device_protocol,
            configuration_value: config.configuration_value,
            interface_count: config.interface_count as u8,
            endpoint_count: config.endpoint_count as u8,
        }))
    }
}
