# Labgrid integration

Labgrid: https://github.com/labgrid-project/labgrid  
Documentation: https://labgrid.readthedocs.io/

Labgrid is the preferred starting point for a privately controlled LionOS hardware bench. It coordinates targets with serial-console, power, reset and removable-media resources, and integrates with pytest. It does not provide remote access to a laptop unless a lab controller/exporter is physically connected and configured.

## Setup outline

1. Use a separate Linux computer as the lab controller. Do not make the LOQ itself the only controller if it must reboot into LionOS.
2. Install Labgrid on the controller using the upstream installation guide.
3. Connect a supported serial adapter and dedicated removable boot medium.
4. Configure power/reset control only if supported by the hardware.
5. Copy the example environment and replace device paths with actual lab resources.
6. Verify that LionOS emits boot logs through the selected serial port before writing automated assertions.

## Important LionOS limitation

The current shell is rendered through LionOS console/framebuffer paths. This repository snapshot does not establish that kernel shell output is available through UART. The RawSerialPort example is a placeholder, not a verified LionOS serial interface. Automated shell interaction requires a serial-console output/input path and target-specific validation.

## Keyboard acceptance

- Boot LionOS from removable media.
- Run the keyboard command.
- Press several keys on the laptop's built-in keyboard.
- Run keyboard again and compare raw-input, scancode and IRQ counters.
- Verify typed characters appear in the shell.

A detected PS/2 controller alone is not a PASS.
