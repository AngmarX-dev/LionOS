# KernelCI compatibility

Repository: https://github.com/kernelci/kernelci-core  
Documentation: https://docs.kernelci.org/

KernelCI describes its mission as testing the upstream Linux kernel. Its core repository includes pipeline/configuration code and integrations with hardware labs (including LAVA). This makes KernelCI useful as a reference for pipeline orchestration and result collection, but the standard KernelCI kernel-build/boot pipeline is not a native runner for LionOS.

LionOS is an independent 32-bit x86 Multiboot2 operating system. It is not a Linux kernel tree or Linux boot image. A regular KernelCI Linux-kernel build/test job therefore cannot compile and boot LionOS as if it were a Linux kernel.

## What was tested

The branch workflow clones the upstream `kernelci-core` repository, parses its runtime and legacy test configuration files, and confirms that its configured lab runtimes include LAVA backends. This is an upstream configuration smoke test, not a LionOS job submission or a physical hardware test.

## What can be reused

- The build -> deploy -> boot -> collect logs -> report pipeline model.
- The result metadata/reporting concepts.
- LAVA as a hardware execution backend, where the board, boot method and console are supported.
- A future custom bridge that publishes a separate LionOS test result into an external dashboard.

## Result

<em>KernelCI native LionOS execution: NOT SUPPORTED by its standard Linux-kernel pipeline.</em>

A separate custom integration could be written around the KernelCI pipeline APIs, but that would be a new custom runner and result producer, not native support. For this project, GitHub Actions is the build/structural validator and Labgrid or LAVA is the possible physical-device controller.
