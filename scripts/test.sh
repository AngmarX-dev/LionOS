#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

TIMEOUT_SECONDS="${LIONOS_TEST_TIMEOUT:-10}"
QEMU_MEMORY="${LIONOS_TEST_MEMORY:-128M}"

run_qemu() {
    local log_file="$1"
    set +e
    timeout "${TIMEOUT_SECONDS}s" qemu-system-i386 \
        -smp 2 \
        -cdrom build/lionos.iso \
        -drive file=build/lionos-disk.img,format=raw,if=ide \
        -m "$QEMU_MEMORY" \
        -display none \
        -serial none \
        -debugcon stdio \
        -global isa-debugcon.iobase=0xE9 >"$log_file" 2>&1
    local status=$?
    set -e

    cat "$log_file"
    if [ "$status" -ne 124 ]; then
        echo "LionOS test: QEMU exited unexpectedly with status $status" >&2
        return 1
    fi
}

echo "== LionOS Phase 23 stability test =="
echo "[1/4] Building kernel, userspace, ISO, and persistent disk"
make clean
make
make userland
make iso
make disk

echo "[2/4] Validating every userspace ELF"
for elf in build/*.elf; do
    test -s "$elf"
    test "$(stat -c%s "$elf")" -le 16384
    readelf -h "$elf" | grep -q 'Class:[[:space:]]*ELF32'
    readelf -h "$elf" | grep -q 'Machine:[[:space:]]*Intel 80386'
    readelf -l "$elf" | grep -q 'LOAD'
done

echo "[3/4] First SMP boot / filesystem initialization"
run_qemu build/qemu-first.log
grep -q 'LIONOS:READY' build/qemu-first.log
grep -q 'LIONOS:PERSIST-INIT' build/qemu-first.log
grep -q 'LIONOS:SMP-CPU-ONLINE' build/qemu-first.log

echo "[4/4] Second SMP boot / persistence verification"
run_qemu build/qemu-second.log
grep -q 'LIONOS:READY' build/qemu-second.log
grep -q 'LIONOS:PERSIST-OK' build/qemu-second.log
grep -q 'LIONOS:SMP-CPU-ONLINE' build/qemu-second.log

echo "LionOS Phase 23 stability test: PASS"
