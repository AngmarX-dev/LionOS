//! Ethernet frame validation and parsing primitives; no NIC driver is included.
#[derive(Clone, Copy, Debug)]
pub struct EthernetHeader { pub destination: [u8;6], pub source: [u8;6], pub ether_type: u16 }
pub fn parse_ethernet(frame: &[u8]) -> Option<EthernetHeader> {
    if frame.len() < 14 { return None; }
    Some(EthernetHeader {
        destination: [frame[0],frame[1],frame[2],frame[3],frame[4],frame[5]],
        source: [frame[6],frame[7],frame[8],frame[9],frame[10],frame[11]],
        ether_type: u16::from_be_bytes([frame[12],frame[13]]),
    })
}
