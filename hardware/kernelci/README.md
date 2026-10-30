# KernelCI compatibility

Repository: https://github.com/kernelci/kernelci-core  
Documentation: https://docs.kernelci.org/

KernelCI is a community hardware-testing ecosystem centered on the Linux kernel. Its standard pipelines build and boot Linux kernel revisions on registered hardware. LionOS is an independent 32-bit Multiboot2 operating system, not a Linux kernel, so it cannot be submitted as an ordinary KernelCI kernel build.

## What can be reused

- The pipeline model: build -> deploy -> boot -> collect logs -> report.
- Hardware-lab result concepts and test metadata.
- Labgrid/LAVA as possible physical device-control backends.
- Per-board reproducible test reporting.

## What cannot be claimed

- Native KernelCI support for LionOS.
- That existing Linux boot tests can boot the LionOS ISO.
- That a KernelCI result exists for the Lenovo LOQ.
- That a Linux kernel passing on a board proves LionOS drivers work there.

A future custom bridge could publish Labgrid/LAVA results into a separate result dashboard, but that is a custom integration, not the normal KernelCI pipeline. GitHub Actions artifacts and a structured hardware report are the truthful initial result store.
