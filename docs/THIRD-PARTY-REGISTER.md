# Third-Party Component Register

This register tracks components and unresolved provenance work. It is not a license opinion. `PENDING_AUDIT` means not approved for source import or public release.

| ID | Component | Role | Status | Shipped in ISO/USB? | Main unresolved evidence |
|---|---|---|---|---|---|
| grub | GRUB bootloader/modules | Runtime boot component | PENDING_AUDIT | Yes / Yes | Exact package version, embedded module inventory, corresponding source and notices |
| build-tools | GCC/binutils/NASM/xorriso/mtools/syslinux-utils/QEMU/Python | Build/CI only | BUILD_ONLY_NOT_SHIPPED | No / No | Exact package versions should be captured for reproducible builds |
| project-assets | Existing PNG branding/icons/wallpapers | Embedded runtime assets | PENDING_AUDIT | Yes / Yes | Creator, source and redistribution license per asset |
| source-audit | Existing source-tree provenance | Audit task | PENDING_AUDIT | N/A | File-level origin and license inventory is incomplete |

The machine-readable source of truth is [`THIRD_PARTY_COMPONENTS.csv`](THIRD_PARTY_COMPONENTS.csv).

## Adding a component

1. Add a distinct CSV row for each independently licensed component.
2. Preserve original license/copyright files in the source tree.
3. Use `UNKNOWN` for facts that have not been verified.
4. Do not mark a row `APPROVED` until all evidence is present and reviewed.
5. Run `make third-party-check`.
6. Include provenance, license, compatibility and test review in the pull request.

**Current scope:** this stage establishes intake controls only. It does not import third-party source, establish complete repository-wide provenance, or certify distribution compliance.
