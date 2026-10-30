# LionOS Hardware Test Lab

This branch integrates Labgrid, LAVA, and KernelCI scaffolding and now adds a polled COM1 serial console for real test benches. It does not claim LionOS has booted on physical hardware. No emulator or virtual machine is used by these hardware-lab checks.

| Component | Automated check performed | Actual LionOS hardware status |
|---|---|---|
| GitHub Actions | Native build, Multiboot2/ISO/filesystem structure, serial-console source contract | Build/structure only |
| Labgrid | Upstream package loads target; RawSerialPort and SerialDriver bind | No physical serial device connected |
| LAVA | Upstream strict schema validates UEFI-menu + interactive job | No live LAVA server/device/job submitted |
| KernelCI | Upstream core runtime/test YAML parses; LAVA lab runtime config inspected | Standard KernelCI Linux-kernel pipeline does not natively build/boot LionOS |
| Lenovo LOQ 15IAX9 | Built-in keyboard acceptance test planned | External/documented COM1 UART availability is unverified |

## Safety rules

- Boot only from a dedicated removable test medium.
- Do not install LionOS to the internal SSD.
- Do not repartition, format, or overwrite internal storage.
- Do not run unattended destructive disk tests.
- Configure power control explicitly.
- A successful build/configuration check is not proof of a successful hardware boot.
- Keyboard PASS requires actual key presses on the built-in keyboard and observed input in LionOS.

## Serial console

See `include/serial.h`, `src/serial.c`, and `hardware/lava/README.md`.

LionOS initializes 16550-compatible COM1 (`0x03F8`) with COM2 (`0x02F8`) fallback, 115200 8N1, with no flow control and polling rather than UART IRQs. An internal loopback test must pass before the UART is enabled. Console output is mirrored to COM1, serial input is routed to the shell, and UART-equipped targets enter the shell instead of the GUI. The `serial` command reports detection and transfer counters.

This is an implementation for a legacy COM1-style UART, not a generic USB serial driver. A standard laptop USB port is not a UART. The LOQ 15IAX9 has not been verified to expose a documented COM1/UART connector, so this change does not establish that LAVA can control that laptop. Do not connect adapters to undocumented pins; electrical levels must match (TTL UART versus RS-232).

## Labgrid

See `hardware/labgrid/README.md` and `hardware/labgrid/environment.yaml.example`. Labgrid is still the best fit for a privately controlled physical bench. Its CI check validates the actual Labgrid config parser and target factory. The example serial resource is a placeholder; it does not open a real port or contact a target. Use a real worker-side serial connection to a documented UART.

## LAVA

See `hardware/lava/README.md` and `hardware/lava/lionos-job-template.yaml`. The CI check runs the official LAVA strict job-schema validator. The job uses LAVA interactive mode, waits for `lion:/ >`, then runs `serial`, `uname`, `keyboard`, and `ls`. It assumes a registered device, worker-side serial connection, UEFI menu mapping, and pre-prepared removable USB. It does not write to internal storage. No live LAVA server/device is currently connected to this repository.

## KernelCI

See `hardware/kernelci/README.md`. The CI check clones and parses the current upstream KernelCI core configurations. Its standard pipeline targets upstream Linux kernels, so this is not a native LionOS execution backend.

## Required physical stage

Connect a matching electrical serial interface to a documented target UART, boot LionOS from removable USB, and submit the job from a configured LAVA server. A LAVA PASS must include a detected serial diagnostic, prompt interaction, successful shell commands, and actual physical keyboard input evidence. Until a real LAVA worker and target are registered, those hardware results remain untested.
