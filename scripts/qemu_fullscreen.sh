#!/bin/sh
set -eu

BUILD="${BUILD:-build}"
KERNEL="${KERNEL:-$BUILD/lionos.bin}"
ISO="${ISO:-$BUILD/lionos-qemu.iso}"

echo "LionOS QEMU display: native framebuffer mode (GRUB auto)"

rm -rf "$BUILD/qemu-iso"
mkdir -p "$BUILD/qemu-iso/boot/grub"
cp "$KERNEL" "$BUILD/qemu-iso/boot/lionos.bin"

# Do not rewrite gfxmode to the host resolution. GRUB selects the native
# framebuffer mode, and LionOS consumes the exact mode reported by Multiboot2.
cp boot/grub.cfg "$BUILD/qemu-iso/boot/grub/grub.cfg"

grub-mkrescue -o "$ISO" "$BUILD/qemu-iso" >/dev/null

if [ "${LIONOS_QEMU_DRY_RUN:-0}" = "1" ]; then
    echo "LionOS QEMU image prepared: $ISO"
    exit 0
fi

DISPLAY_VALUE="gtk,zoom-to-fit=on"
FULLSCREEN_ARGS="-full-screen"
if [ "${LIONOS_QEMU_FULLSCREEN:-1}" = "0" ]; then
    FULLSCREEN_ARGS=""
fi

exec qemu-system-i386 \
    -cdrom "$ISO" \
    -drive file="$BUILD/lionos-disk.img",format=raw,if=ide \
    -m 128M \
    -smp 2 \
    -netdev user,id=lionnet \
    -device rtl8139,netdev=lionnet \
    -display "$DISPLAY_VALUE" $FULLSCREEN_ARGS
