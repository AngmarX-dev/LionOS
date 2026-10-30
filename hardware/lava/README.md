# LAVA integration

LAVA repository: https://github.com/Linaro/lava  
Project site: https://www.lavasoftware.org/

LAVA schedules jobs on registered devices and supports interactive tests for non-POSIX command interfaces. That interactive mode is a better conceptual match for the LionOS shell than LAVA's normal test-definition shell, which expects a POSIX environment.

## What was validated

The branch validation clones the upstream LAVA source and runs its `lava_common.schemas.validate(..., strict=True)` validator against `lionos-job-template.yaml`. This checks the current LAVA job/action schema, including the UEFI-menu boot action and interactive test definition. It does not submit a job or contact a LAVA server.

## Job assumptions

- A LAVA server is controlled by the lab administrator.
- An x86 UEFI device type is registered.
- The device type defines a UEFI menu selector named `lionos-usb` for an already prepared removable USB device.
- The device's serial console exposes the LionOS prompt `lion:/ >`.
- The removable medium is prepared outside the LAVA job. The template deliberately does not repartition or write the laptop's internal SSD.

The job runs `uname`, `keyboard`, and `ls` over LAVA's interactive console. The keyboard command's diagnostic banner confirms the command ran; it does **not** prove that physical key input works. A true keyboard PASS still requires pressing keys on the built-in keyboard and observing the counters/characters.

## LionOS-specific blocker

The current shell is framebuffer-only. There is no verified bidirectional UART console for the shell, so LAVA cannot currently see the prompt or send shell commands to LionOS. The job is schema-valid, but it is not operational on the present LionOS build until a serial console is implemented and wired to the device.

No LAVA server or registered LOQ device is available to this repository workflow, so no live job can be submitted here.
