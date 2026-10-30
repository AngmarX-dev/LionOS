# LAVA integration

LAVA repository mirror: https://github.com/Linaro/lava  
Project site: https://www.lavasoftware.org/

LAVA schedules jobs on devices registered with a LAVA server. It is useful for a shared hardware lab with known device types, boot methods and deployment infrastructure.

## Requirements

- A LAVA server controlled by the lab owner.
- A registered x86 device and matching device-type definition.
- A tested method to deploy/boot a Multiboot2 ISO from removable media or network boot.
- A test definition that captures a console channel LionOS actually emits.
- A non-destructive target configuration.

The job file here is a template, not a ready-to-submit job. Device type and deployment/boot actions must be supplied by the administrator. No job is submitted from GitHub automatically.

## LionOS-specific blocker

The current repository does not prove that LionOS provides a serial console usable by LAVA for boot prompts and shell interaction. LAVA cannot infer framebuffer text. A serial output/input path must be implemented and verified, or another supported console transport configured.

Do not use a generic Linux boot action for LionOS. LionOS is a Multiboot2 operating system, not a Linux kernel image.
