#!/bin/sh
set -eu

BUILD="${BUILD:-build}"
KERNEL="${KERNEL:-$BUILD/lionos.bin}"
ISO="${ISO:-$BUILD/lionos-qemu.iso}"
RES="${LIONOS_QEMU_RESOLUTION:-}"

detect_resolution() {
    if [ -n "$RES" ]; then
        return
    fi

    if command -v xrandr >/dev/null 2>&1 && [ -n "${DISPLAY:-}" ]; then
        RES="$(xrandr --current 2>/dev/null | awk '
            / connected( primary)? / {
                for (i = 1; i <= NF; ++i) {
                    if ($i ~ /^[0-9]+x[0-9]+\+/) {
                        sub(/\\+.*/, "", $i)
                        print $i
                        exit
                    }
                }
            }')"
    fi

    if [ -z "$RES" ] && command -v xdpyinfo >/dev/null 2>&1 && [ -n "${DISPLAY:-}" ]; then
        RES="$(xdpyinfo 2>/dev/null | awk '/dimensions:/ {print $2; exit}')"
    fi

    if [ -z "$RES" ]; then
        RES="1920x1080"
    fi
}

validate_resolution() {
    case "$RES" in
        [0-9]*x[0-9]*) ;;
        *) echo "Invalid LionOS QEMU resolution: $RES" >&2; exit 1 ;;
    esac

    WIDTH="${RES%x*}"
    HEIGHT="${RES#*x}"

    case "$WIDTH:$HEIGHT" in
        *[!0-9:]*|:*) echo "Invalid LionOS QEMU resolution: $RES" >&2; exit 1 ;;
    esac

    if [ "$WIDTH" -lt 640 ] || [ "$HEIGHT" -lt 480 ]; then
        echo "LionOS QEMU resolution is too small: $RES" >&2
        exit 1
    fi
}

detect_resolution
validate_resolution

echo "LionOS QEMU display: $RES (host resolution)"

rm -rf "$BUILD/qemu-iso"
mkdir -p "$BUILD/qemu-iso/boot/grub"
cp "$KERNEL" "$BUILD/qemu-iso/boot/lionos.bin"

sed "s/^set gfxmode=.*/set gfxmode=${RES}x32/" boot/grub.cfg \
    > "$BUILD/qemu-iso/boot/grub/grub.cfg"

grub-mkrescue -o "$ISO" "$BUILD/qemu-iso" >/dev/null

if [ "${LIONOS_QEMU_DRY_RUN:-0}" = "1" ]; then
    echo "LionOS QEMU image prepared: $ISO"
    exit 0
fi

DISPLAY_ARGS="-display"
DISPLAY_VALUE="gtk,fullscreen=on,zoom-to-fit=on"
if [ "${LIONOS_QEMU_FULLSCREEN:-1}" = "0" ]; then
    DISPLAY_VALUE="gtk,fullscreen=off,zoom-to-fit=on"
fi

exec qemu-system-i386 \
    -cdrom "$ISO" \
    -drive file="$BUILD/lionos-disk.img",format=raw,if=ide \
    -m 128M \
    -smp 2 \
    -netdev user,id=lionnet \
    -device rtl8139,netdev=lionnet \
    "$DISPLAY_ARGS" "$DISPLAY_VALUE"
