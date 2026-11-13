#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ISO="${1:-dist/lionos-bios-usb.iso}"
EXPECTED_KERNEL="${2:-target/i686-unknown-linux-gnu/release/lionos-rust}"
EXPECTED_GRUB_CFG="${3:-iso/boot/grub/grub.cfg}"
GRUB_FILE="${GRUB_FILE:-grub-file}"
XORRISO="${XORRISO:-xorriso}"

for tool in "$GRUB_FILE" "$XORRISO" cmp; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing required verification tool: $tool" >&2
        exit 1
    fi
done

for input in "$ISO" "$EXPECTED_KERNEL" "$EXPECTED_GRUB_CFG"; do
    if [[ ! -s "$input" ]]; then
        echo "Required file is missing or empty: $input" >&2
        exit 1
    fi
done

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

"$XORRISO" -indev "$ISO" -report_el_torito plain >"$TMP_DIR/el-torito.txt" 2>&1
if ! grep -qi 'BIOS' "$TMP_DIR/el-torito.txt"; then
    cat "$TMP_DIR/el-torito.txt" >&2
    echo "ISO does not report a BIOS El Torito boot entry." >&2
    exit 1
fi

"$XORRISO" -osirrox on -indev "$ISO" \
    -extract /boot/lionos-rust "$TMP_DIR/lionos-rust"
"$XORRISO" -osirrox on -indev "$ISO" \
    -extract /boot/grub/grub.cfg "$TMP_DIR/grub.cfg"

"$GRUB_FILE" --is-x86-multiboot2 "$TMP_DIR/lionos-rust"
cmp "$EXPECTED_KERNEL" "$TMP_DIR/lionos-rust"
cmp "$EXPECTED_GRUB_CFG" "$TMP_DIR/grub.cfg"

echo "[ OK ] BIOS El Torito boot entry detected"
echo "[ OK ] Embedded kernel is Multiboot2 and matches the build output"
echo "[ OK ] Embedded GRUB configuration matches the source"
echo "Verified ISO: $ISO"
