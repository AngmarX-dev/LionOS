# Labgrid integration

Labgrid: https://github.com/labgrid-project/labgrid  
Documentation: https://labgrid.readthedocs.io/

Labgrid is the best fit of these three for a small, privately controlled physical test bench. It provides resource/driver abstractions and pytest integration for serial consoles, power/reset controllers and removable-media equipment.

## What was validated

The branch validation installs the upstream Labgrid Python package, loads the example environment with Labgrid's actual `Environment` and target factory, creates a `RawSerialPort`, and verifies that `SerialDriver` binds to it without opening the port. This is a configuration/API test only; it does not require or pretend that `/dev/ttyUSB0` exists.

## Setup requirements

1. Use a separate Linux controller; the LOQ cannot control its own power cycle while booted into LionOS.
2. Connect a real serial interface that is electrically and logically connected to a LionOS console.
3. Replace `/dev/ttyUSB0` and 115200 with the actual controller device and baud rate.
4. Configure power/reset/removable-media resources only when the corresponding physical equipment is present.
5. Run pytest tests through Labgrid after the target console is verified.

## LionOS-specific blocker

The current LionOS shell is rendered through the framebuffer. Its prompt is `lion:/ >`, but the source does not provide a verified UART console path for shell input/output. The current debug output is not a substitute for a normal bidirectional serial console. Therefore the example's serial driver can be constructed and bound, but it cannot yet interact with the running LionOS shell on the LOQ.

## Keyboard acceptance

- Boot LionOS from a dedicated removable medium.
- Run the `keyboard` diagnostic command.
- Press keys on the laptop's built-in keyboard.
- Run `keyboard` again and compare raw-input, scancode and IRQ counters.
- Confirm characters are accepted by the LionOS shell.

A detected PS/2 controller alone is not a PASS. Labgrid cannot automate this test until a real console/control path is available; the physical key presses still have to occur on the target laptop.
