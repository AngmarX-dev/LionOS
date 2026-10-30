# LAVA integration

LAVA repository: https://github.com/Linaro/lava  
Project site: https://www.lavasoftware.org/

LAVA schedules jobs on registered physical devices. Its interactive test action can drive the LionOS command shell without requiring a POSIX userspace.

## LionOS serial console implemented on this branch

The kernel now includes a polled 16550-compatible COM1 driver:

- UART ports: COM1 at `0x03F8`, then COM2 at `0x02F8` as fallback
- Baud: `115200`
- Framing: `8 data bits, no parity, 1 stop bit (8N1)`
- Flow control: none
- Driver mode: polling; UART interrupts are disabled
- Detection: UART internal loopback self-test before enabling the console

Console output is mirrored to the active display and COM1. Serial input is fed into the same shell command processor as the PS/2/USB keyboard. CR, LF, CRLF and terminal backspace are normalized. The `serial` shell command reports UART detection and RX/TX/error counters.

When COM1 or COM2 passes loopback detection, LionOS skips the graphical desktop and enters the interactive shell directly. This makes the `lion:/ >` prompt available to LAVA. If no verified COM1 is found, normal desktop startup is unchanged.

The internal loopback only verifies the UART itself. It does not verify the cable, adapter, remote serial server, electrical levels, or the registered LAVA device.

## LAVA server and device setup

1. Register a physical x86 UEFI device type and a device on a LAVA worker.
2. Connect the worker's serial interface to the target's documented COM1/UART interface. Configure the worker-side serial connection for 115200 8N1, no flow control.
3. Make the serial interface available to the worker's LAVA dispatcher using the connection method configured for that device (for example, a local serial concentrator or serial server). The connection command and port are lab-specific; do not copy a placeholder device path.
4. Configure the device's UEFI boot-menu mapping named `lionos-usb` to select a pre-prepared removable LionOS USB device.
5. Replace `REPLACE_WITH_REGISTERED_X86_UEFI_DEVICE_TYPE` in the job template with the exact registered LAVA device type.
6. Submit the job from the LAVA server. The template does not partition, format, install to, or write to internal storage.

The device connection and UEFI-menu mapping are LAVA-lab configuration, not properties that the job YAML can invent. A schema-valid job cannot run until those resources exist.

## Job coverage

The interactive job waits for `lion:/ >` and runs:

- `serial` — expects the serial diagnostic banner
- `uname` — confirms the LionOS shell responds
- `keyboard` — reports PS/2 state and counters
- `ls` — exercises another shell command

The keyboard diagnostic banner proves only that the command ran. Keyboard PASS still requires actual physical key presses and observed scancode/character changes.

## What CI validates

The branch workflow clones upstream LAVA and validates the job with `lava_common.schemas.validate(..., strict=True)`. It also runs the LionOS serial-console source contract checks. CI does not submit a live job, connect a serial cable, or boot a registered device.

## Hardware limitation

The Lenovo LOQ 15IAX9 has not been verified to expose a documented legacy COM1/COM2 UART connector. A normal USB port is not a UART port. A USB-to-TTL adapter only helps when its serial pins are connected to an actual documented target UART; RS-232 and TTL-level UART must not be connected directly to each other. Do not probe unknown motherboard pins.

If the LOQ has no accessible UART, this legacy COM1/COM2 implementation alone cannot provide a LAVA console for that laptop. A supported firmware serial redirection path or a different target with accessible UART hardware would be required.
