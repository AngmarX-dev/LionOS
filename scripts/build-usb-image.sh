#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

for tool in cargo nasm grub-file grub-mkrescue xorriso; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing required tool: $tool" >&2
        echo "Install: build-essential gcc-multilib nasm grub-common grub-pc-bin xorriso" >&2
        exit 1
    fi
done

make usb-image
echo
echo "Image ready: $ROOT/dist/lionos-bios-usb.iso"
echo "This image currently targets legacy BIOS boot only."
echo "Use Rufus on Windows and select DD mode when prompted."
