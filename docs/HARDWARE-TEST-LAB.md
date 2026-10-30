# LionOS Hardware Test Lab

This branch is an integration scaffold and validation set for Labgrid, LAVA, and KernelCI. It does not claim LionOS has booted on physical hardware. No emulator or virtual machine is used by these hardware-lab checks.

| Component | Automated check performed | Actual LionOS hardware status |
|---|---|---|
| GitHub Actions | Native build, Multiboot2/ISO/filesystem structure | Build/structure only |
| Labgrid | Upstream Python package loads target; RawSerialPort and SerialDriver bind | No physical serial device; LionOS shell has no verified UART console |
| LAVA | Upstream strict schema validates UEFI-menu + interactive job | No LAVA server/device; framebuffer-only console blocks interactive use |
| KernelCI | Upstream core runtime/test YAML parses; LAVA lab runtime config inspected | Standard KernelCI Linux-kernel pipeline does not natively build/boot LionOS |
| Lenovo LOQ 15IAX9 | Built-in keyboard acceptance test planned | Requires actual laptop boot and physical key presses |

## Safety rules

- Boot only from a dedicated removable test medium.
- Do not install LionOS to the internal SSD.
- Do not repartition, format, or overwrite internal storage.
- Do not run unattended destructive disk tests.
- Configure power control explicitly.
- A successful build/configuration check is not proof of a successful hardware boot.
- Keyboard PASS requires actual key presses on the built-in keyboard and observed input in LionOS.

## Labgrid

See `hardware/labgrid/README.md` and `hardware/labgrid/environment.yaml.example`. Labgrid is the best first choice for a small private hardware bench. The CI check validates the actual Labgrid config parser and target factory. The example uses a placeholder serial device and does not open it. LionOS currently renders its shell through the framebuffer; a verified UART console path is still required.

## LAVA

See `hardware/lava/README.md` and `hardware/lava/lionos-job-template.yaml`. The CI check runs the official LAVA strict job-schema validator. The job uses LAVA's interactive test mode, which is suitable for a non-POSIX command line, and assumes a pre-prepared removable USB device selected through a configured UEFI menu entry. It does not write to internal storage. A LAVA server, registered device, serial console and UEFI menu configuration are still required.

## KernelCI

See `hardware/kernelci/README.md`. The CI check clones and parses the current upstream KernelCI core configurations. Its standard pipeline targets upstream Linux kernels, so this is not a native LionOS execution backend.

## Required hardware stage

The current LionOS shell prompt is `lion:/ >` and is rendered via framebuffer. Labgrid and LAVA need a console transport that can read and write that prompt. A UART/serial console path must be implemented and physically connected before those tools can interact with the shell. Then a physical operator must boot from removable USB and press keys on the built-in keyboard. Only recorded input increments and visible shell characters can mark keyboard PASS.
