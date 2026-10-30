# LionOS Hardware Test Lab

This branch is an integration scaffold for Labgrid, LAVA, and KernelCI. It does not claim LionOS has booted on physical hardware. No emulator or virtual machine is used by this hardware-lab workflow.

| Component | Intended role | Current status |
|---|---|---|
| GitHub-hosted runner | Compile LionOS and validate ISO/image structure | Build/structure only |
| Labgrid | Control a real x86 test machine, serial console, power/reset and pytest | Configuration template; requires a lab controller and target |
| LAVA | Schedule boot/test jobs on a registered physical device | Job template; requires server, registered device and boot method |
| KernelCI | Linux kernel hardware test ecosystem | Not a direct LionOS execution backend |
| Lenovo LOQ 15IAX9 | Final target for internal keyboard and laptop-specific validation | Requires physical access and key actuation |

## Safety rules

- Boot only from a dedicated removable test medium.
- Do not install LionOS to the internal SSD.
- Do not repartition, format, or overwrite internal storage.
- Do not run unattended destructive disk tests.
- Configure power control explicitly.
- A successful build/configuration check is not proof of a successful hardware boot.
- Keyboard PASS requires actual key presses on the built-in keyboard and observed input in LionOS.

## Labgrid

See hardware/labgrid/README.md and hardware/labgrid/environment.yaml.example. Labgrid is the most suitable starting point for a small private hardware bench. It can expose serial, power and USB/SD resources to pytest. LionOS serial console support is not yet verified, so automated serial shell tests are not currently operational.

## LAVA

See hardware/lava/README.md and hardware/lava/lionos-job-template.yaml. LAVA is appropriate when a server and device type exist. The template is not submitted automatically: boot method, device type, image deployment and bootloader commands are lab-specific.

## KernelCI

See hardware/kernelci/README.md. KernelCI's standard pipeline tests Linux kernels. LionOS is a Multiboot2 OS, not a Linux kernel. A custom integration would need Labgrid/LAVA as the physical execution backend and a custom result producer; this is not native KernelCI support.

## Proposed stages

1. GitHub-hosted CI builds the kernel, userland, ISO and disk image.
2. Structural checks inspect ELF, Multiboot2, ISO and filesystem metadata.
3. A separately managed physical lab controller obtains the ISO artifact.
4. Labgrid or LAVA boots the target from removable media non-destructively.
5. The operator captures boot output and runs the keyboard diagnostic.
6. The operator presses keys on the built-in keyboard.
7. Logs/results are uploaded to GitHub. Only observed hardware results are marked PASS.

## Current truth

This branch adds integration documentation and configuration templates. It does not create remote access to the user's laptop, provision a physical test bench, or prove keyboard functionality. Those require hardware and controller access outside GitHub.
