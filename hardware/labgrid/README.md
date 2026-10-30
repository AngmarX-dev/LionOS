# Labgrid integration

Labgrid: https://github.com/labgrid-project/labgrid  
Documentation: https://labgrid.readthedocs.io/

Labgrid is the best fit of these three for a small, privately controlled physical test bench. It provides resource/driver abstractions and pytest integration for serial consoles, power/reset controllers and removable-media equipment.

## What was validated

The branch validation installs the upstream Labgrid Python package, loads the example environment with Labgrid's actual `Environment` and target factory, creates a `RawSerialPort`, and verifies that `SerialDriver` binds to it without opening the port. This is a configuration/API test only; it does not require or pretend that `/dev/ttyUSB0` exists.

## LionOS serial transport

LionOS now provides a polled 16550-compatible COM1 console on legacy COM1–COM4 (I/O bases `0x03F8`, `0x02F8`, `0x03E8`, `0x02E8`), 115200 8N1, no flow control. It tests the UART with internal loopback, mirrors console output to COM1, and routes serial input to the interactive shell. When any detected legacy UART is found, LionOS skips the desktop and enters the shell. This makes the existing Labgrid serial driver useful once a real physical UART link is provided.

The internal loopback test does not verify the external cable, adapter or serial server. CI only validates that the Labgrid resource/driver can be configured; it does not open the placeholder device or interact with hardware.

## Setup requirements

1. Use a separate Linux controller; the LOQ cannot control its own power cycle while booted into LionOS.
2. Connect a real worker-side serial interface to the target's documented legacy UART interface and configure 115200 8N1, no flow control.
3. Replace `/dev/ttyUSB0` and the example baud rate with the actual controller device and settings.
4. Configure power/reset/removable-media resources only when the corresponding physical equipment is present.
5. Run pytest tests through Labgrid after the target displays the `lion:/ >` prompt over serial.

Do not connect a TTL adapter directly to RS-232 levels, and do not probe undocumented motherboard pins. The Lenovo LOQ 15IAX9 has not been verified to expose a usable UART connector; an ordinary USB port is not a UART.

## Keyboard acceptance

- Boot LionOS from a dedicated removable medium.
- Run the `keyboard` diagnostic command through the shell.
- Press keys on the laptop's built-in keyboard.
- Run `keyboard` again and compare raw-input, scancode and IRQ counters.
- Confirm characters are accepted by the LionOS shell.

A detected PS/2 controller alone is not a PASS. Labgrid can send shell commands through the UART, but the physical key presses still have to occur on the target laptop.
