#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ISO="${1:-dist/lionos-bios-usb.iso}"
QEMU="${QEMU:-qemu-system-i386}"
TIME_LIMIT="${QEMU_BOOT_TIMEOUT:-30}"

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "Missing QEMU executable: $QEMU" >&2
    exit 1
fi
if [[ ! -s "$ISO" ]]; then
    echo "ISO is missing or empty: $ISO" >&2
    exit 1
fi

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

set +e
timeout --signal=TERM --kill-after=5s "${TIME_LIMIT}s" \
    "$QEMU" \
    -machine pc \
    -cpu qemu32 \
    -m 512M \
    -cdrom "$ISO" \
    -boot order=d,strict=on \
    -display none \
    -monitor none \
    -serial "file:$LOG" \
    -no-reboot \
    -no-shutdown
QEMU_STATUS=$?
set -e

# The kernel deliberately idles forever after successful bring-up, so timeout
# is expected. A QEMU error/early exit is accepted only if the boot markers
# prove the guest reached the kernel's completed subsystem bring-up point.
if ! grep -Fq "[ OK ] Multiboot2 entry verified" "$LOG"; then
    echo "QEMU did not reach the Rust kernel entry point." >&2
    cat "$LOG" >&2
    exit 1
fi
if ! grep -Fq "LionOS Rust kernel reached subsystem bring-up." "$LOG"; then
    echo "Kernel did not reach the completed subsystem bring-up marker." >&2
    cat "$LOG" >&2
    exit 1
fi

if [[ "$QEMU_STATUS" -ne 124 && "$QEMU_STATUS" -ne 0 ]]; then
    echo "QEMU exited unexpectedly with status $QEMU_STATUS after boot." >&2
    cat "$LOG" >&2
    exit 1
fi

echo "[ OK ] ISO booted through GRUB and entered the Multiboot2 Rust kernel"
echo "[ OK ] Kernel reached subsystem bring-up under QEMU"
echo "[INFO] QEMU remained running until the expected timeout (or exited cleanly)"
